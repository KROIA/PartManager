#include "llm/PartManager_KicadToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_Part.h"
	#include "domain/PartManager_PartFile.h"
	#include "domain/PartManager_PartFileRole.h"
	#include "filestore/PartManager_FileStore.h"
	#include "kicad/PartManager_KicadGeometry.h"
	#include "kicad/PartManager_KicadLibraryGenerator.h"
	#include "kicad/PartManager_KicadSymbolWriter.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "SQLite.h"
#endif

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace PartManager
{
	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// The refusals shared with PartToolset and MouserToolset, spelled the same way on purpose:
		// a model that has learned what one of them means has learned what all of them mean.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";
		const char* ReadOnlyMessage = "this assistant may not change the database";

		// UI-only metadata (QtLLM::Tool), never sent to the model, so it is an identifier rather
		// than prose that would need translating.
		const char* ToolGroup = "KiCad";

		// How much two footprint outlines may differ and still count as the same one, in
		// millimetres. 0.05 is half the stroke width a vendor draws a courtyard with, so it
		// absorbs the rounding between two generators of the same land pattern and nothing wider.
		constexpr double DefaultToleranceMm = 0.05;


		// ---- argument reading ------------------------------------------------------------
		// The same copies PartToolset.cpp carries, and deliberately so: forgiving about *shape*
		// and strict about *meaning*. A local model quotes its integers about as often as not, and
		// rejecting `"5"` costs a whole turn to re-learn something the handler could have read.

		QString stringArg(const QJsonObject& args, const QString& key)
		{
			return args.value(key).toString().trimmed();
		}

		bool hasArg(const QJsonObject& args, const QString& key)
		{
			return args.contains(key) && !args.value(key).isNull();
		}

		bool intArg(const QJsonObject& args, const QString& key, int& outValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isDouble())
			{
				const double raw = value.toDouble();
				const int truncated = static_cast<int>(raw);
				if (static_cast<double>(truncated) != raw)
				{
					return false;
				}
				outValue = truncated;
				return true;
			}
			if (value.isString())
			{
				bool ok = false;
				const int parsed = value.toString().trimmed().toInt(&ok);
				if (ok)
				{
					outValue = parsed;
				}
				return ok;
			}
			return false;
		}

		bool doubleArg(const QJsonObject& args, const QString& key, double& outValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isDouble())
			{
				outValue = value.toDouble();
				return true;
			}
			if (value.isString())
			{
				bool ok = false;
				const double parsed = value.toString().trimmed().toDouble(&ok);
				if (ok)
				{
					outValue = parsed;
				}
				return ok;
			}
			return false;
		}


		// ---- shared formatting -----------------------------------------------------------

		// User data (a part name, a footprint's own name) straight into a JSON value. Never
		// translated — it is what the vendor or the user wrote, not the app's own chrome.
		QJsonValue jsonText(const std::string& text)
		{
			return QJsonValue(QString::fromStdString(text));
		}

		QString folded(const QString& text)
		{
			return text.trimmed().toCaseFolded();
		}

		QJsonArray asJsonArray(const QStringList& values)
		{
			QJsonArray array;
			for (const QString& value : values)
			{
				array.append(value);
			}
			return array;
		}

		QJsonArray asJsonArray(const std::vector<std::string>& values)
		{
			QJsonArray array;
			for (const std::string& value : values)
			{
				array.append(jsonText(value));
			}
			return array;
		}

		// The same refusal PartToolset answers an invented part id with, so the model reads one
		// rule rather than two. The id list is deliberately not spelled out: a library holds
		// thousands of parts and the way to find one is search_parts, which the message names.
		QJsonObject unknownPartError(SQLiteWrapper::SQLite& db, int partId)
		{
			const size_t count = PartRepository::listParts(db).size();
			QJsonObject extra;
			extra["partCount"] = static_cast<int>(count);
			return llmError(QStringLiteral("no part with id %1. The database holds %2 parts; "
				"use search_parts to find the right id.")
				.arg(partId)
				.arg(static_cast<int>(count)), extra);
		}

		QJsonObject missingIdError(const QString& name, const QString& hint)
		{
			return llmError(QStringLiteral("'%1' is required and must be an integer id. %2")
				.arg(name).arg(hint));
		}

		// A three-decimal round, so a courtyard measured at 2.8499999999999996 is reported as the
		// 2.85 the file actually says. Anything finer than a micrometre is noise a fabricator
		// could not hold anyway, and an unrounded double in a tool result is a number the model
		// will read back to the user verbatim.
		double rounded(double value)
		{
			return std::round(value * 1000.0) / 1000.0;
		}

		QJsonObject xyzObject(double x, double y, double z)
		{
			QJsonObject object;
			object["x"] = rounded(x);
			object["y"] = rounded(y);
			object["z"] = rounded(z);
			return object;
		}


		// ---- the KiCad file slots ----------------------------------------------------------

		// The three roles `set_part_kicad_file` accepts, as the model spells them. Named once
		// because the schema, the handler's re-validation and the error all need the same list,
		// and a fourth place spelling them differently is how a fixed vocabulary stops being one
		// (§14c).
		const QStringList& roleValues()
		{
			static const QStringList values{ QStringLiteral("symbol"),
				QStringLiteral("footprint"), QStringLiteral("model3d") };
			return values;
		}

		bool roleFromText(const QString& text, PartFileRole& outRole)
		{
			const QString wanted = folded(text);
			if (wanted == QStringLiteral("symbol")) { outRole = PartFileRole::KicadSymbol; return true; }
			if (wanted == QStringLiteral("footprint")) { outRole = PartFileRole::KicadFootprint; return true; }
			if (wanted == QStringLiteral("model3d")) { outRole = PartFileRole::Kicad3DModel; return true; }
			return false;
		}

		// The whole contents of a stored file, empty when the row points at bytes that are no
		// longer on disk. A missing file is a normal state — a database folder can be copied
		// without its filestore — so it is an empty answer here and a sentence in the caller,
		// never an exception.
		std::string storedFileText(FileStore& store, const PartFile& file)
		{
			const std::string absolute = store.absolutePath(file.relativePath);
			if (absolute.empty())
			{
				return std::string();
			}
			std::ifstream stream(absolute, std::ios::binary);
			if (!stream)
			{
				return std::string();
			}
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		// The text of one of a part's KiCad slots, with the row it came from. False when the part
		// carries nothing in that role or the bytes are gone.
		bool kicadFileText(SQLiteWrapper::SQLite& db, FileStore& store, int partId,
			PartFileRole role, PartFile& outFile, std::string& outText)
		{
			if (!FileStore::roleFile(db, partId, role, outFile))
			{
				return false;
			}
			outText = storedFileText(store, outFile);
			return !outText.empty();
		}

		// What one slot looks like to the model: whether the part carries anything there, and
		// under what name. "Not attached" is a normal answer and not an error — most parts are in
		// that state and §5a generates a symbol for them anyway.
		QJsonObject slotObject(SQLiteWrapper::SQLite& db, int partId, PartFileRole role)
		{
			QJsonObject slot;
			PartFile file;
			if (FileStore::roleFile(db, partId, role, file))
			{
				slot["attached"] = true;
				slot["name"] = jsonText(file.originalFilename.empty()
					? file.relativePath : file.originalFilename);
			}
			else
			{
				slot["attached"] = false;
				slot["name"] = QString();
			}
			return slot;
		}


		// ---- footprint geometry ------------------------------------------------------------

		// Everything `describe_kicad_footprint` answers with and `find_parts_sharing_a_footprint`
		// compares on, measured once per file. Both tools read the same numbers because a part
		// that "shares a footprint" has to mean the same thing the description just said.
		struct FootprintFacts
		{
			bool ok = false;
			std::string name;
			// Pad *entries*, which is what the copper actually is. `numbers` is the distinct set:
			// a thermal pad broken into pieces is several entries and one net, and matching on the
			// pieces would say two identical land patterns differ.
			int padCount = 0;
			std::vector<std::string> numbers;
			bool throughHole = false;
			std::vector<std::string> layers;
			double widthMm = 0.0;
			double heightMm = 0.0;
			bool hasCourtyard = false;
			double courtyardWidthMm = 0.0;
			double courtyardHeightMm = 0.0;
			KicadModelPlacement model;
		};

		void sortUnique(std::vector<std::string>& values)
		{
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}

		FootprintFacts footprintFactsOf(const std::string& footprintText)
		{
			FootprintFacts facts;
			const KicadDrawing drawing = KicadGeometry::footprint(footprintText);
			if (drawing.empty())
			{
				return facts;
			}
			facts.ok = true;
			facts.name = drawing.name;
			facts.model = drawing.model3D;

			std::vector<KicadShape> courtyard;
			for (const KicadShape& shape : drawing.shapes)
			{
				if (!shape.layer.empty())
				{
					facts.layers.push_back(shape.layer);
				}
				if (shape.layer == "F.CrtYd" || shape.layer == "B.CrtYd")
				{
					courtyard.push_back(shape);
				}
				if (shape.kind != KicadShapeKind::Pad)
				{
					continue;
				}
				++facts.padCount;
				facts.throughHole = facts.throughHole || shape.throughHole;
				// A pad with no number is copper with no net — a mounting hole, an NPTH, a bare
				// paste island — and is not a pin. KicadSymbolWriter::pinsFromFootprint() draws the
				// same line, so a footprint describes the same pin set it would derive a symbol
				// from.
				if (!shape.label.empty())
				{
					facts.numbers.push_back(shape.label);
				}
			}
			sortUnique(facts.numbers);
			sortUnique(facts.layers);

			// A footprint file measures Y downward and the board world upward, so the two
			// coordinate systems disagree about the sign of every y. A *height* is the distance
			// between two of them, which is the one number the flip cannot change — so these are
			// reported straight out of the file without one.
			KicadPoint min;
			KicadPoint max;
			if (drawing.bounds(min, max))
			{
				facts.widthMm = max.x - min.x;
				facts.heightMm = max.y - min.y;
			}
			if (!courtyard.empty())
			{
				KicadDrawing courtyardOnly;
				courtyardOnly.shapes = courtyard;
				if (courtyardOnly.bounds(min, max))
				{
					facts.hasCourtyard = true;
					facts.courtyardWidthMm = max.x - min.x;
					facts.courtyardHeightMm = max.y - min.y;
				}
			}
			return facts;
		}

		// How far apart two outlines are, as the larger of the two edge differences. One number
		// rather than two because the question it answers — "will this land pattern do?" — has one
		// answer, and a model handed a width delta and a height delta has to invent this rule
		// itself to use them.
		double outlineDelta(const FootprintFacts& left, const FootprintFacts& right)
		{
			return std::max(std::abs(left.widthMm - right.widthMm),
				std::abs(left.heightMm - right.heightMm));
		}


		// ---- the tools -------------------------------------------------------------------

		LlmTool makeListPartKicadFiles(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("list_part_kicad_files")
				.setDescription("What KiCad files a part actually carries: its symbol, its "
					"footprint, its 3D model and its datasheet, each either attached or not. Call "
					"this first — every other KiCad tool depends on the answer. \"Not attached\" is "
					"a normal state and not a problem: most parts are in it, and the library "
					"generator still produces a symbol for them.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				QJsonObject payload;
				payload["symbol"] = slotObject(db, partId, PartFileRole::KicadSymbol);
				payload["footprint"] = slotObject(db, partId, PartFileRole::KicadFootprint);
				payload["model3d"] = slotObject(db, partId, PartFileRole::Kicad3DModel);
				payload["datasheet"] = slotObject(db, partId, PartFileRole::Datasheet);
				// §2b: both resolve up the category tree, so a leaf category inherits the flag and
				// the library name from whichever ancestor declares them. Reporting the *effective*
				// value is the only useful one — the part's own row carries neither.
				payload["kicadCategory"] =
					jsonText(PartTypeRepository::effectiveKicadCategory(db, part.partTypeId));
				payload["kicadRelevant"] =
					PartTypeRepository::effectiveKicadRelevant(db, part.partTypeId);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeDescribeKicadFootprint(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("describe_kicad_footprint")
				.setDescription("Measures the footprint a part carries: how many pads it has and "
					"what they are numbered, whether they are through-hole, which layers it draws "
					"on, its overall and courtyard size in millimetres, and where its 3D model "
					"sits. The numbers come from parsing the file, so they are the ones the app's "
					"own preview draws — there is no need to read the file itself.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				FileStore store(context.database->filestorePath());
				PartFile file;
				std::string text;
				if (!kicadFileText(db, store, partId, PartFileRole::KicadFootprint, file, text))
				{
					return llmError(QStringLiteral("part %1 (\"%2\") carries no KiCad footprint. "
						"Use list_part_kicad_files to see what it does carry, or "
						"set_part_kicad_file to give it the footprint another part already has.")
						.arg(partId).arg(QString::fromStdString(part.name)));
				}

				const FootprintFacts facts = footprintFactsOf(text);
				if (!facts.ok)
				{
					return llmError(QStringLiteral("the footprint attached to part %1 (\"%2\") "
						"could not be read as a KiCad footprint.")
						.arg(partId).arg(QString::fromStdString(file.originalFilename)));
				}

				QJsonObject model;
				model["present"] = facts.model.present;
				// Millimetres, whichever spelling the file used: `(offset (xyz ...))` is already
				// mm and the legacy `(at (xyz ...))` is inches, converted on the way in. Degrees,
				// already un-negated — KiCad stores a rotation the opposite way round from the one
				// it applies, so the file's own numbers only match the picture after the flip.
				model["offsetMm"] = xyzObject(facts.model.offsetX, facts.model.offsetY,
					facts.model.offsetZ);
				model["rotationDeg"] = xyzObject(facts.model.rotateX, facts.model.rotateY,
					facts.model.rotateZ);
				model["scale"] = xyzObject(facts.model.scaleX, facts.model.scaleY,
					facts.model.scaleZ);

				QJsonObject courtyard;
				courtyard["w"] = rounded(facts.courtyardWidthMm);
				courtyard["h"] = rounded(facts.courtyardHeightMm);

				QJsonObject payload;
				payload["footprintName"] = jsonText(facts.name);
				payload["padCount"] = facts.padCount;
				payload["padNumbers"] = asJsonArray(facts.numbers);
				payload["throughHole"] = facts.throughHole;
				payload["layers"] = asJsonArray(facts.layers);
				payload["widthMm"] = rounded(facts.widthMm);
				payload["heightMm"] = rounded(facts.heightMm);
				payload["courtyardMm"] = courtyard;
				payload["model3d"] = model;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeDescribeKicadSymbol(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("describe_kicad_symbol")
				.setDescription("The schematic symbol a part carries: how many pins, what each is "
					"numbered, named and typed, and how large the body is in millimetres. "
					"IMPORTANT: check derivedFromFootprint in the answer. When it is true the part "
					"has no symbol of its own and the pins were read off its footprint's pads — the "
					"connections are real but the arrangement is not the part's pinout, so it is a "
					"stand-in and not a symbol anyone drew.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				FileStore store(context.database->filestorePath());
				const std::string symbolName = KicadSymbolWriter::sanitizeSymbolName(part.name);

				QJsonArray pins;
				KicadDrawing drawing;
				bool derived = false;

				PartFile symbolFile;
				std::string symbolText;
				if (kicadFileText(db, store, partId, PartFileRole::KicadSymbol, symbolFile, symbolText))
				{
					// **Not simply the first symbol in the file.** A `.kicad_sym` PartManager wrote
					// starts with the embedded `PM_*` bases every generated symbol extends, so
					// taking block zero would describe a bare resistor outline for every part.
					// KicadGeometry::symbol() applies exactly the generator's own pickPartSymbol
					// rule: the block named after the part when it is there, otherwise the first
					// that is not a base.
					drawing = KicadGeometry::symbol(symbolText, symbolName);
					for (const KicadShape& shape : drawing.shapes)
					{
						if (shape.kind != KicadShapeKind::Pin)
						{
							continue;
						}
						QJsonObject pin;
						pin["number"] = jsonText(shape.label);
						pin["name"] = jsonText(shape.pinName);
						pin["type"] = jsonText(shape.pinType);
						pins.append(pin);
					}
				}
				else
				{
					PartFile footprintFile;
					std::string footprintText;
					if (!kicadFileText(db, store, partId, PartFileRole::KicadFootprint,
						footprintFile, footprintText))
					{
						return llmError(QStringLiteral("part %1 (\"%2\") carries neither a KiCad "
							"symbol nor a footprint, so there is no symbol to describe. Use "
							"set_part_kicad_file to give it the symbol another part already has.")
							.arg(partId).arg(QString::fromStdString(part.name)));
					}

					// §5a's M5 path: a part with a footprint and no symbol gets pins read off its
					// pads. Those pins are real copper, so the stand-in places correctly — which is
					// precisely why the model has to be told it is one.
					derived = true;
					const std::vector<KicadDerivedPin> derivedPins =
						KicadSymbolWriter::pinsFromFootprint(footprintText);
					for (const KicadDerivedPin& pin : derivedPins)
					{
						QJsonObject entry;
						entry["number"] = jsonText(pin.number);
						entry["name"] = jsonText(pin.name);
						entry["type"] = jsonText(pin.type);
						pins.append(entry);
					}
					// Measured across the 30 `.kicad_mod` of a real library: every one yields
					// between 2 and 16 pins and not one carries a `pinfunction`, so an unnamed pin
					// is the normal case here rather than a parse failure.
					if (!derivedPins.empty())
					{
						KicadSymbolSpec spec;
						spec.name = symbolName;
						drawing = KicadGeometry::symbol(
							KicadSymbolWriter::library(
								{ KicadSymbolWriter::derivedSymbolBlock(spec, derivedPins) }),
							symbolName);
					}
				}

				KicadPoint min;
				KicadPoint max;
				double widthMm = 0.0;
				double heightMm = 0.0;
				if (drawing.bounds(min, max))
				{
					widthMm = max.x - min.x;
					heightMm = max.y - min.y;
				}

				QJsonObject payload;
				payload["symbolName"] = jsonText(drawing.name);
				payload["pinCount"] = static_cast<int>(pins.size());
				payload["pins"] = pins;
				payload["widthMm"] = rounded(widthMm);
				payload["heightMm"] = rounded(heightMm);
				payload["derivedFromFootprint"] = derived;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeFindPartsSharingAFootprint(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("find_parts_sharing_a_footprint")
				.setDescription("Other parts whose footprint has the same pads in the same places "
					"as this one's: same pad count, same pad numbers, and an outline within "
					"tolerantMm. Each match says whether the two files are \"identical\" — "
					"byte-for-byte the same file — or merely compatible, which is a much weaker "
					"claim: two land patterns can measure the same and still have been drawn for "
					"different parts. This tool reports; it does not merge or change anything.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer",
					"Part id whose footprint the others are compared against.", true)
				.addParameter("tolerantMm", "number",
					"How far two outlines may differ and still match, in millimetres. Default "
					"0.05. Raise it to find land patterns of the same package drawn by different "
					"vendors.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}

				double tolerantMm = DefaultToleranceMm;
				if (hasArg(args, QStringLiteral("tolerantMm"))
					&& !doubleArg(args, QStringLiteral("tolerantMm"), tolerantMm))
				{
					return llmError("'tolerantMm' must be a number of millimetres, e.g. 0.05.");
				}
				if (tolerantMm < 0.0)
				{
					return llmError("'tolerantMm' must not be negative — it is how far two outlines "
						"may differ, not a direction.");
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				FileStore store(context.database->filestorePath());
				PartFile subjectFile;
				std::string subjectText;
				if (!kicadFileText(db, store, partId, PartFileRole::KicadFootprint,
					subjectFile, subjectText))
				{
					return llmError(QStringLiteral("part %1 (\"%2\") carries no KiCad footprint, so "
						"there is nothing to compare. Use list_part_kicad_files to see what it "
						"does carry.").arg(partId).arg(QString::fromStdString(part.name)));
				}
				const FootprintFacts subject = footprintFactsOf(subjectText);
				if (!subject.ok)
				{
					return llmError(QStringLiteral("the footprint attached to part %1 (\"%2\") "
						"could not be read as a KiCad footprint.")
						.arg(partId).arg(QString::fromStdString(subjectFile.originalFilename)));
				}

				QJsonArray matches;
				for (const Part& candidate : PartRepository::listParts(db))
				{
					if (candidate.id == partId)
					{
						continue;
					}
					PartFile candidateFile;
					std::string candidateText;
					if (!kicadFileText(db, store, candidate.id, PartFileRole::KicadFootprint,
						candidateFile, candidateText))
					{
						continue;
					}
					const FootprintFacts other = footprintFactsOf(candidateText);
					if (!other.ok)
					{
						continue;
					}

					const bool padCountMatches =
						other.padCount == subject.padCount && other.numbers == subject.numbers;
					const double delta = outlineDelta(subject, other);
					if (!padCountMatches || delta > tolerantMm)
					{
						continue;
					}

					QJsonObject match;
					match["partId"] = candidate.id;
					match["name"] = jsonText(candidate.name);
					match["mpn"] = jsonText(candidate.mpn);
					match["footprintName"] = jsonText(other.name);
					// The bytes, not the hash. FileStore's content hash is a 64-bit FNV-1a chosen
					// to name a file, not to be a trusted integrity check, and the candidate's text
					// had to be read to measure it anyway — so the strong claim is made from the
					// strong evidence.
					match["identical"] = candidateText == subjectText;
					// Always true for a row that got this far. It stays in the answer because the
					// contract promises it and because a model reading one match has no other way
					// to see *why* it matched.
					match["padCountMatches"] = padCountMatches;
					match["outlineDeltaMm"] = rounded(delta);
					matches.append(match);
				}

				QJsonObject payload;
				payload["footprintName"] = jsonText(subject.name);
				payload["tolerantMm"] = tolerantMm;
				payload["matches"] = matches;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSetPartKicadFile(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("set_part_kicad_file")
				.setDescription("Gives one part the KiCad symbol, footprint or 3D model that "
					"another part in this database already carries. This is how a wrong footprint "
					"is fixed: find a part that has the right one — find_parts_sharing_a_footprint "
					"and search_parts both name candidates — and copy it across. The file can only "
					"come from another part; there is no way to name a file on disk.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id that receives the file.", true)
				.addEnumParameter("role", roleValues(),
					"Which of the part's three KiCad slots to write: \"symbol\" for the schematic "
					"symbol (.kicad_sym), \"footprint\" for the land pattern (.kicad_mod), "
					"\"model3d\" for the 3D body (.step/.stp/.wrl). These three spellings and no "
					"others — this is PartManager's own slot vocabulary, not a file extension and "
					"not a KiCad term.", true)
				.addParameter("sourcePartId", "integer",
					"Part id whose file is copied. It must already carry something in that role.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts to find one."));
				}
				int sourcePartId = 0;
				if (!intArg(args, QStringLiteral("sourcePartId"), sourcePartId))
				{
					return missingIdError(QStringLiteral("sourcePartId"),
						QStringLiteral("It is the part the file is copied *from*; a file path is "
							"not accepted."));
				}

				// §14c rule 3: the enum is re-checked here and not only by the client-side
				// validator. A fixed vocabulary declared as free text is how a model wrote
				// "Circuit Protection" into part_type.domain, and a refusal that hands back the
				// list is one retry where a bare rejection is a loop.
				const QString roleText = stringArg(args, QStringLiteral("role"));
				PartFileRole role = PartFileRole::KicadFootprint;
				if (!roleFromText(roleText, role))
				{
					QJsonObject extra;
					extra["allowed_values"] = asJsonArray(roleValues());
					return llmError(QStringLiteral("'role' must be one of %1 — got \"%2\". It names "
						"which of the part's KiCad slots to write, not a file type.")
						.arg(roleValues().join(QStringLiteral(", ")))
						.arg(roleText), extra);
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}
				Part source;
				if (!PartRepository::findPart(db, sourcePartId, source))
				{
					return unknownPartError(db, sourcePartId);
				}
				if (partId == sourcePartId)
				{
					return llmError(QStringLiteral("part %1 is already the source — copying a "
						"part's own file onto itself changes nothing. Name the part that carries "
						"the file you want as 'sourcePartId'.").arg(partId));
				}

				FileStore store(context.database->filestorePath());
				PartFile sourceFile;
				std::string bytes;
				if (!kicadFileText(db, store, sourcePartId, role, sourceFile, bytes))
				{
					QJsonObject carries;
					carries["symbol"] = slotObject(db, sourcePartId, PartFileRole::KicadSymbol);
					carries["footprint"] = slotObject(db, sourcePartId, PartFileRole::KicadFootprint);
					carries["model3d"] = slotObject(db, sourcePartId, PartFileRole::Kicad3DModel);
					QJsonObject extra;
					extra["sourceCarries"] = carries;
					return llmError(QStringLiteral("part %1 (\"%2\") carries no %3 to copy. Nothing "
						"was changed.")
						.arg(sourcePartId)
						.arg(QString::fromStdString(source.name))
						.arg(folded(roleText)), extra);
				}

				// Read the slot **before** writing it. roleFile() resolves a slot by highest id,
				// so asking afterwards returns the row just inserted and the old one is reported
				// as absent — which is how a single-slot role quietly ends up holding two files.
				// replaceRoleFileBytes() keeps the same order internally; this read is only for the
				// answer.
				PartFile previous;
				const bool replacedExisting = FileStore::roleFile(db, partId, role, previous);

				std::string error;
				const std::string fileName = sourceFile.originalFilename.empty()
					? sourceFile.relativePath : sourceFile.originalFilename;
				const int fileId =
					store.replaceRoleFileBytes(db, partId, role, bytes, fileName, &error);
				if (fileId == 0)
				{
					return llmError(QStringLiteral("the %1 of part %2 could not be written: %3")
						.arg(roleText).arg(partId).arg(QString::fromStdString(error)));
				}

				QJsonObject payload;
				payload["role"] = folded(roleText);
				payload["fileName"] = jsonText(fileName);
				payload["replacedExisting"] = replacedExisting;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeGenerateKicadLibraries(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("generate_kicad_libraries")
				.setDescription("Rewrites this database's KiCad libraries from the parts in it — "
					"one symbol library and one footprint library per KiCad category, plus the "
					"library tables KiCad reads. A symbol somebody edited in KiCad is never "
					"overwritten: it is carried across untouched and listed in \"preserved\". "
					"Parts with no KiCad files of their own are skipped on purpose and listed in "
					"\"skipped\" with the reason, because a user expecting 38 symbols and getting "
					"12 has to be told which went and that it was deliberate.")
				.setGroup(ToolGroup);

			tool.handler = [context](const QJsonObject&) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				if (!context.allowWrites)
				{
					return llmError(ReadOnlyMessage);
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				const KicadGenerationResult result = KicadLibraryGenerator::generate(db,
					context.database->kicadLibsPath(), context.database->filestorePath());
				if (!result.ok)
				{
					return llmError(QStringLiteral("the libraries could not be written: %1")
						.arg(QString::fromStdString(result.errorMessage)));
				}

				// One list, three reasons, each in the generator's own words. Three separate
				// arrays would make the model pick which one to read; one array with a reason on
				// every row is the same information in the shape the answer is given in.
				QJsonArray skipped;
				const auto appendSkipped = [&skipped](const std::vector<std::string>& names,
					const char* reason)
				{
					for (const std::string& name : names)
					{
						QJsonObject entry;
						entry["part"] = jsonText(name);
						entry["reason"] = QString::fromLatin1(reason);
						skipped.append(entry);
					}
				};
				appendSkipped(result.skippedForNoCategory,
					"its category resolves to no KiCad library name");
				appendSkipped(result.skippedForNoKicadFiles,
					"it carries neither a KiCad symbol nor a footprint");
				appendSkipped(result.skippedForNoSymbol,
					"it has a footprint but no symbol, and its pads carry no numbers to derive one from");

				QJsonArray preserved;
				for (const KicadSkippedItem& item : result.preserved)
				{
					QJsonObject entry;
					entry["partId"] = item.partId;
					entry["part"] = jsonText(item.partName);
					entry["target"] = jsonText(item.targetPath);
					// The part behind it no longer qualifies at all, so re-baselining the artifact
					// would mean nothing — removing it is the only real choice left.
					entry["stale"] = item.stale;
					preserved.append(entry);
				}

				QJsonObject payload;
				payload["written"] = asJsonArray(result.libraryNames);
				payload["skipped"] = skipped;
				payload["preserved"] = preserved;
				payload["summary"] = jsonText(result.summary());
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> KicadToolset::tools(const LlmToolContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeListPartKicadFiles(context));
		tools.push_back(makeDescribeKicadFootprint(context));
		tools.push_back(makeDescribeKicadSymbol(context));
		tools.push_back(makeFindPartsSharingAFootprint(context));
		tools.push_back(makeSetPartKicadFile(context));
		tools.push_back(makeGenerateKicadLibraries(context));
		return tools;
#else
		// No SQLite means no parts, and every tool here starts from a part. The toolset still
		// exists so a caller does not have to know which optional dependency was left out; it
		// simply offers nothing.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
