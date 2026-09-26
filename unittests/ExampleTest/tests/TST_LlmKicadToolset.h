#pragma once

#include "UnitTest.h"
#include "llm/PartManager_KicadToolset.h"
#include "tests/TST_LlmTestDatabase.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_PartFile.h"
	#include "domain/PartManager_PartFileRole.h"
	#include "filestore/PartManager_FileStore.h"
	#include "import/PartManager_EcadArchive.h"
	#include "kicad/PartManager_KicadSymbolWriter.h"
	#include "persistence/PartManager_PartRepository.h"
	#include <QByteArray>
	#include <QDir>
	#include <QDateTime>
	#include <QJsonArray>
	#include <QJsonObject>
	#include <QString>
	#include <QStringList>
	#include <private/qzipwriter_p.h>
	#include <cmath>
	#include <filesystem>
	#include <string>
	#include <vector>
#endif

// The §14 KiCad toolset driven the way a model drives it: a tool looked up by its model-facing
// name, called with a QJsonObject, against a throwaway database in %TEMP%. No Ollama, no network,
// no event loop — PartManager_LlmTool.h's split is what makes that possible.
//
// **The files are real.** Every footprint and symbol below is the verbatim text of a file out of a
// vendor ECAD download (SamacSys via Mouser), and it reaches the database the way a user's would:
// packed into an archive of the vendor's own shape and pulled back out through
// `EcadArchive::read()`, PartManager's own importer. Embedding the bytes rather than reading the
// downloads in `.claude/` is deliberate — that folder is gitignored, so a suite that needed it
// would pass here and fail on a clean clone. The only edit is CRLF to LF.
//
// Two of them are the *same 0603 land pattern from two parts of one family*
// (`LIB_CRCW060342K2FKEA` and `LIB_CRCW060352K3FKEA`). Measured: the two `RESC1608X50N.kicad_mod`
// files differ in exactly one line — the name of the `.stp` beside them — which makes them the
// pair `find_parts_sharing_a_footprint` has to call compatible and must not call identical. A
// second real file of another family (`SOP254P916X353-6N`, 6 pads) is measured too, because
// pairing logic that matched one sample perfectly and was wrong on the next is a mistake this
// project has already made once (ORIENTATION §8).
// @see PartManager_KicadToolset.h, PartManager_KicadGeometry.h, docs/design/ARCHITECTURE.md §14
class TST_LlmKicadToolset : public UnitTest::Test
{
	TEST_CLASS(TST_LlmKicadToolset)
public:
	TST_LlmKicadToolset()
		: Test("TST_LlmKicadToolset")
	{
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_LlmKicadToolset::everyDocumentedToolIsThereAndNoneTakesAPath);
		ADD_TEST(TST_LlmKicadToolset::footprintsAreMeasuredFromTheRealFiles);
		ADD_TEST(TST_LlmKicadToolset::aThroughHolePadIsReportedAsOne);
		ADD_TEST(TST_LlmKicadToolset::symbolSaysWhetherItWasDerivedFromTheFootprint);
		ADD_TEST(TST_LlmKicadToolset::theTwoResistorsOfOneFamilyShareAFootprint);
		ADD_TEST(TST_LlmKicadToolset::replacingAFileLeavesExactlyOneInTheSlot);
		ADD_TEST(TST_LlmKicadToolset::aBadRoleIsRefusedWithTheAllowedValues);
		ADD_TEST(TST_LlmKicadToolset::readOnlyBlocksBothWritesAndStillDescribes);
		ADD_TEST(TST_LlmKicadToolset::generateReportsWhatItWroteAndWhatItSkipped);
#endif
	}

private:

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// ---- the real vendor files ---------------------------------------------------------
	// `LIB_CRCW060342K2FKEA.zip`, entry `CRCW060342K2FKEA/KiCad/RESC1608X50N.kicad_mod`.
	// Note the traps it happens to carry: the legacy `(at (xyz ...))` model offset, which is in
	// *inches*, and a `(rotate ...)` KiCad stores negated.
	static const char* ResistorFootprint0603()
	{
		return R"kicad((module "RESC1608X50N" (layer F.Cu)
  (descr "CRCW0603")
  (tags "Resistor")
  (attr smd)
  (fp_text reference R** (at 0 0) (layer F.SilkS)
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_text user %R (at 0 0) (layer F.Fab)
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_text value "RESC1608X50N" (at 0 0) (layer F.SilkS) hide
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_line (start -1.425 -0.75) (end 1.425 -0.75) (layer F.CrtYd) (width 0.05))
  (fp_line (start 1.425 -0.75) (end 1.425 0.75) (layer F.CrtYd) (width 0.05))
  (fp_line (start 1.425 0.75) (end -1.425 0.75) (layer F.CrtYd) (width 0.05))
  (fp_line (start -1.425 0.75) (end -1.425 -0.75) (layer F.CrtYd) (width 0.05))
  (fp_line (start -0.788 -0.425) (end 0.788 -0.425) (layer F.Fab) (width 0.1))
  (fp_line (start 0.788 -0.425) (end 0.788 0.425) (layer F.Fab) (width 0.1))
  (fp_line (start 0.788 0.425) (end -0.788 0.425) (layer F.Fab) (width 0.1))
  (fp_line (start -0.788 0.425) (end -0.788 -0.425) (layer F.Fab) (width 0.1))
  (pad 1 smd rect (at -0.75 0 0) (size 0.85 1) (layers F.Cu F.Paste F.Mask))
  (pad 2 smd rect (at 0.75 0 0) (size 0.85 1) (layers F.Cu F.Paste F.Mask))
  (model CRCW060342K2FKEA.stp
    (at (xyz 0.030708660290936 0.016535432554605 0))
    (scale (xyz 1 1 1))
    (rotate (xyz -90 0 0))
  )
)
)kicad";
	}

	// The same land pattern out of `LIB_CRCW060352K3FKEA.zip`. The two vendor files were compared
	// byte for byte: this one line is their whole difference, so building it here rather than
	// pasting 1374 near-identical bytes keeps what matters visible.
	static std::string ResistorFootprint0603Sibling()
	{
		std::string text = ResistorFootprint0603();
		const std::string from = "CRCW060342K2FKEA.stp";
		const std::string to = "CRCW060352K3FKEA.stp";
		const size_t at = text.find(from);
		if (at != std::string::npos)
		{
			text.replace(at, from.size(), to);
		}
		return text;
	}

	// `LIB_MOC3083SR2M.zip`, entry `MOC3083SR2M/KiCad/SOP254P916X353-6N.kicad_mod`. The second
	// real file: six pads, a different family, a different outline.
	static const char* OptocouplerFootprintSop254()
	{
		return R"kicad((module "SOP254P916X353-6N" (layer F.Cu)
  (descr "PDIP6 8.51x6.35, 2.54P CASE 646BY ISSUE A")
  (tags "Integrated Circuit")
  (attr smd)
  (fp_text reference IC** (at 0 0) (layer F.SilkS)
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_text user %R (at 0 0) (layer F.Fab)
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_text value "SOP254P916X353-6N" (at 0 0) (layer F.SilkS) hide
    (effects (font (size 1.27 1.27) (thickness 0.254)))
  )
  (fp_line (start -5.55 -4.695) (end 5.55 -4.695) (layer F.CrtYd) (width 0.05))
  (fp_line (start 5.55 -4.695) (end 5.55 4.695) (layer F.CrtYd) (width 0.05))
  (fp_line (start 5.55 4.695) (end -5.55 4.695) (layer F.CrtYd) (width 0.05))
  (fp_line (start -5.55 4.695) (end -5.55 -4.695) (layer F.CrtYd) (width 0.05))
  (fp_line (start -3.175 -4.255) (end 3.175 -4.255) (layer F.Fab) (width 0.1))
  (fp_line (start 3.175 -4.255) (end 3.175 4.255) (layer F.Fab) (width 0.1))
  (fp_line (start 3.175 4.255) (end -3.175 4.255) (layer F.Fab) (width 0.1))
  (fp_line (start -3.175 4.255) (end -3.175 -4.255) (layer F.Fab) (width 0.1))
  (fp_line (start -3.175 -1.715) (end -0.635 -4.255) (layer F.Fab) (width 0.1))
  (fp_line (start -2.9 -4.255) (end 2.9 -4.255) (layer F.SilkS) (width 0.2))
  (fp_line (start 2.9 -4.255) (end 2.9 4.255) (layer F.SilkS) (width 0.2))
  (fp_line (start 2.9 4.255) (end -2.9 4.255) (layer F.SilkS) (width 0.2))
  (fp_line (start -2.9 4.255) (end -2.9 -4.255) (layer F.SilkS) (width 0.2))
  (fp_line (start -5.3 -3.24) (end -3.25 -3.24) (layer F.SilkS) (width 0.2))
  (pad 1 smd rect (at -4.275 -2.54 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (pad 2 smd rect (at -4.275 0 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (pad 3 smd rect (at -4.275 2.54 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (pad 4 smd rect (at 4.275 2.54 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (pad 5 smd rect (at 4.275 0 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (pad 6 smd rect (at 4.275 -2.54 90) (size 0.7 2.05) (layers F.Cu F.Paste F.Mask))
  (model MOC3083SR2M.stp
    (at (xyz 0 0 0))
    (scale (xyz 1 1 1))
    (rotate (xyz -90 0 0))
  )
)
)kicad";
	}

	// `LIB_CRCW060342K2FKEA.zip`, entry `CRCW060342K2FKEA/KiCad/CRCW060342K2FKEA.kicad_sym`.
	static const char* ResistorSymbol0603()
	{
		return R"kicad((kicad_symbol_lib (version 20211014) (generator SamacSys_ECAD_Model)
  (symbol "CRCW060342K2FKEA" (in_bom yes) (on_board yes) (pin_names hide)
    (property "Reference" "R" (at 13.97 6.35 0)
      (effects (font (size 1.27 1.27)) (justify left top))
    )
    (property "Value" "CRCW060342K2FKEA" (at 13.97 3.81 0)
      (effects (font (size 1.27 1.27)) (justify left top))
    )
    (property "Footprint" "RESC1608X50N" (at 13.97 -96.19 0)
      (effects (font (size 1.27 1.27)) (justify left top) hide)
    )
    (property "Datasheet" "http://www.vishay.com/docs/20035/dcrcwe3.pdf" (at 13.97 -196.19 0)
      (effects (font (size 1.27 1.27)) (justify left top) hide)
    )
    (property "Height" "0.5" (at 13.97 -396.19 0)
      (effects (font (size 1.27 1.27)) (justify left top) hide)
    )
    (property "Manufacturer_Name" "Vishay" (at 13.97 -696.19 0)
      (effects (font (size 1.27 1.27)) (justify left top) hide)
    )
    (property "Manufacturer_Part_Number" "CRCW060342K2FKEA" (at 13.97 -796.19 0)
      (effects (font (size 1.27 1.27)) (justify left top) hide)
    )
    (rectangle
      (start 5.08 1.27)
      (end 12.7 -1.27)
      (stroke (width 0.254) (type default))
      (fill (type background))
    )
    (pin passive line (at 0 0 0) (length 5.08)
      (name "1" (effects (font (size 1.27 1.27))))
      (number "1" (effects (font (size 1.27 1.27))))
    )
    (pin passive line (at 17.78 0 180) (length 5.08)
      (name "2" (effects (font (size 1.27 1.27))))
      (number "2" (effects (font (size 1.27 1.27))))
    )
  )
)
)kicad";
	}

	// The one fixture that is not a vendor file: none of the five real downloads is through-hole,
	// they are all `(attr smd)`. Hand-written in the same format so `throughHole` is proven true
	// somewhere as well as false on the real parts.
	static const char* ThroughHoleFootprint()
	{
		return R"kicad((module "TEST_THT_2P" (layer F.Cu)
  (descr "two leaded pads, 2.54 pitch")
  (attr through_hole)
  (fp_line (start -2.54 -1.27) (end 2.54 -1.27) (layer F.CrtYd) (width 0.05))
  (fp_line (start 2.54 -1.27) (end 2.54 1.27) (layer F.CrtYd) (width 0.05))
  (fp_line (start 2.54 1.27) (end -2.54 1.27) (layer F.CrtYd) (width 0.05))
  (fp_line (start -2.54 1.27) (end -2.54 -1.27) (layer F.CrtYd) (width 0.05))
  (pad 1 thru_hole circle (at -1.27 0) (size 1.6 1.6) (drill 0.8) (layers *.Cu *.Mask))
  (pad 2 thru_hole circle (at 1.27 0) (size 1.6 1.6) (drill 0.8) (layers *.Cu *.Mask))
)
)kicad";
	}


	// ---- driving the tools --------------------------------------------------------------

	static QJsonObject call(const std::vector<PartManager::LlmTool>& tools, const char* name,
		const QJsonObject& args = QJsonObject())
	{
		return callLlmTool(tools, name, args);
	}

	static bool isOk(const QJsonObject& result) { return llmResultOk(result); }

	static std::string messageOf(const QJsonObject& result) { return llmResultMessage(result); }

	static bool closeTo(double value, double expected)
	{
		return std::abs(value - expected) < 1e-6;
	}

	static QStringList textsOf(const QJsonArray& array)
	{
		QStringList values;
		for (const QJsonValue& entry : array)
		{
			values.append(entry.toString());
		}
		return values;
	}

	static int createPart(const std::vector<PartManager::LlmTool>& partTools, int categoryId,
		const char* name, const char* mpn)
	{
		QJsonObject args;
		args["categoryId"] = categoryId;
		args["name"] = QString::fromLatin1(name);
		args["mpn"] = QString::fromLatin1(mpn);
		return call(partTools, "create_part", args).value("id").toInt();
	}

	// How many `part_file` rows the part actually has in one role. The regression guard for the
	// single-slot bug asks this rather than asking FileStore::roleFile(), which answers "the
	// newest" and would say everything was fine with two rows in the slot.
	static int filesInRole(PartManager::DatabaseHandle& handle, int partId,
		PartManager::PartFileRole role)
	{
		int count = 0;
		for (const PartManager::PartFile& file :
			PartManager::PartRepository::listFiles(handle.connection(), partId))
		{
			if (PartManager::partFileRoleFromString(file.role) == role)
			{
				++count;
			}
		}
		return count;
	}

	// Packs `footprintText`/`symbolText` into an archive shaped like a real SamacSys download —
	// a KiCad folder, another CAD tool's folder beside it — and imports it onto `partId` through
	// `EcadArchive::read()`, which is the path the app's own ECAD import takes. Building the
	// archive rather than attaching the strings directly is the point: it proves the toolset is
	// describing files that arrived the way a user's files arrive.
	static bool importArchiveOnto(PartManager::DatabaseHandle& handle, int partId,
		const char* stem, const char* footprintName, const std::string& footprintText,
		const std::string& symbolText)
	{
		const std::filesystem::path folder =
			std::filesystem::path(QDir::tempPath().toStdString())
			/ ("PartManager_TST_LlmKicad_" + std::string(stem) + "_"
				+ std::to_string(QDateTime::currentMSecsSinceEpoch()));
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const std::filesystem::path zipPath = folder / ("LIB_" + std::string(stem) + ".zip");

		{
			QZipWriter writer(QString::fromStdString(zipPath.string()));
			if (!writer.isWritable())
			{
				return false;
			}
			const QString root = QString::fromLatin1(stem);
			// The decoy the real archives carry: a `.lib` that is not KiCad's.
			writer.addFile(root + QStringLiteral("/CADSTAR/") + root + QStringLiteral(".lib"),
				QByteArray("not kicad"));
			if (!footprintText.empty())
			{
				writer.addFile(root + QStringLiteral("/KiCad/") + QString::fromLatin1(footprintName),
					QByteArray(footprintText.c_str(), static_cast<int>(footprintText.size())));
			}
			if (!symbolText.empty())
			{
				writer.addFile(root + QStringLiteral("/KiCad/") + root + QStringLiteral(".kicad_sym"),
					QByteArray(symbolText.c_str(), static_cast<int>(symbolText.size())));
			}
			writer.close();
		}

		const PartManager::EcadArchivePayload payload =
			PartManager::EcadArchive::read(zipPath.string());
		std::filesystem::remove_all(folder, ec);
		if (!payload.contents.ok)
		{
			return false;
		}

		PartManager::FileStore store(handle.filestorePath());
		bool ok = true;
		if (!payload.footprintBytes.empty())
		{
			ok = ok && store.replaceRoleFileBytes(handle.connection(), partId,
				PartManager::PartFileRole::KicadFootprint, payload.footprintBytes,
				payload.footprintName) != 0;
		}
		if (!payload.symbolBytes.empty())
		{
			ok = ok && store.replaceRoleFileBytes(handle.connection(), partId,
				PartManager::PartFileRole::KicadSymbol, payload.symbolBytes,
				payload.symbolName) != 0;
		}
		return ok;
	}


	// ---- Tests ---------------------------------------------------------------------------

	// The six tools the contract header documents, and the property that makes leaving them all
	// enabled acceptable: §14f says the assistant reaches parts and nothing else, so not one of
	// them may take a filesystem path.
	TEST_FUNCTION(everyDocumentedToolIsThereAndNoneTakesAPath)
	{
		TEST_START;

		ScopedDatabase database("kicad_tools");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());

		const std::vector<PartManager::LlmTool> tools = database.kicadTools();
		TEST_COMPARE(tools.size(), static_cast<size_t>(6));

		const char* expected[] = { "list_part_kicad_files", "describe_kicad_footprint",
			"describe_kicad_symbol", "find_parts_sharing_a_footprint", "set_part_kicad_file",
			"generate_kicad_libraries" };
		for (const char* name : expected)
		{
			TEST_ASSERT_M(PartManager::findLlmTool(tools, QString::fromLatin1(name)) != nullptr,
				std::string("tool missing: ") + name);
		}

		// §14f, and the reason this whole toolset can be left on: a parameter that named a file
		// would reintroduce the filesystem access the built-in tools were deliberately left
		// unregistered to avoid. Checked over every parameter of every tool rather than by
		// reading the six schemas, so a seventh tool cannot quietly add one.
		const char* forbidden[] = { "path", "file", "dir", "folder", "url" };
		for (const PartManager::LlmTool& tool : tools)
		{
			const QJsonObject api = tool.schema.toApiObject();
			const std::string toolName = api.value("name").toString().toStdString();
			const QJsonObject properties =
				api.value("input_schema").toObject().value("properties").toObject();
			for (const QString& parameter : properties.keys())
			{
				const QString lowered = parameter.toLower();
				for (const char* word : forbidden)
				{
					TEST_ASSERT_M(!lowered.contains(QString::fromLatin1(word)),
						"a KiCad tool parameter names a filesystem object: "
						+ toolName + "." + parameter.toStdString());
				}
			}
		}

		// §14c rule 3: `role` is a declared enum, not free text. Free text on a fixed vocabulary
		// is how a model wrote "Circuit Protection" into part_type.domain.
		const PartManager::LlmTool* setFile =
			PartManager::findLlmTool(tools, QStringLiteral("set_part_kicad_file"));
		TEST_ASSERT(setFile != nullptr);
		const QJsonObject roleSchema = setFile->schema.toApiObject()
			.value("input_schema").toObject().value("properties").toObject()
			.value("role").toObject();
		TEST_COMPARE(roleSchema.value("enum").toArray().size(), 3);
		QJsonObject badRole;
		badRole["partId"] = 1;
		badRole["sourcePartId"] = 2;
		badRole["role"] = "kicad_mod";
		TEST_ASSERT_M(!setFile->schema.validate(badRole).isEmpty(),
			"the schema itself must reject a role outside the vocabulary");
	}

	// Every number here was read out of the file by hand first (ORIENTATION §8: measure, do not
	// assume), and two of them are the traps the geometry header records — a `(at (xyz ...))`
	// model offset in *inches*, and a `(rotate ...)` stored negated.
	TEST_FUNCTION(footprintsAreMeasuredFromTheRealFiles)
	{
		TEST_START;

		ScopedDatabase database("kicad_describe_fp");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int resistorCategory = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		TEST_ASSERT(resistorCategory != 0);

		const int resistorId =
			createPart(partTools, resistorCategory, "R 42k2 0603", "CRCW060342K2FKEA");
		const int optoId =
			createPart(partTools, resistorCategory, "MOC3083SR2M", "MOC3083SR2M");
		TEST_ASSERT(resistorId != 0 && optoId != 0);

		TEST_ASSERT_M(importArchiveOnto(*database.handle(), resistorId, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), ResistorSymbol0603()),
			"the vendor archive did not import");
		TEST_ASSERT_M(importArchiveOnto(*database.handle(), optoId, "MOC3083SR2M",
			"SOP254P916X353-6N.kicad_mod", OptocouplerFootprintSop254(), std::string()),
			"the second vendor archive did not import");

		// list_part_kicad_files is the answer every other tool depends on.
		QJsonObject byId;
		byId["partId"] = resistorId;
		const QJsonObject listed = call(tools, "list_part_kicad_files", byId);
		TEST_ASSERT_M(isOk(listed), messageOf(listed));
		TEST_ASSERT(listed.value("symbol").toObject().value("attached").toBool());
		TEST_ASSERT(listed.value("footprint").toObject().value("attached").toBool());
		TEST_COMPARE(listed.value("footprint").toObject().value("name").toString().toStdString(),
			std::string("RESC1608X50N.kicad_mod"));
		// Neither was in the archive, and "not attached" is a normal answer rather than an error.
		TEST_ASSERT(!listed.value("model3d").toObject().value("attached").toBool());
		TEST_ASSERT(!listed.value("datasheet").toObject().value("attached").toBool());
		TEST_ASSERT_M(listed.value("kicadRelevant").toBool(),
			"the seeded Resistor category is KiCad-relevant");

		const QJsonObject footprint = call(tools, "describe_kicad_footprint", byId);
		TEST_ASSERT_M(isOk(footprint), messageOf(footprint));
		TEST_COMPARE(footprint.value("footprintName").toString().toStdString(),
			std::string("RESC1608X50N"));
		TEST_COMPARE(footprint.value("padCount").toInt(), 2);
		TEST_COMPARE(textsOf(footprint.value("padNumbers").toArray()).join(QLatin1Char(','))
			.toStdString(), std::string("1,2"));
		TEST_ASSERT_M(!footprint.value("throughHole").toBool(),
			"a CRCW0603 is `(attr smd)` and every one of its pads is smd");
		const QStringList layers = textsOf(footprint.value("layers").toArray());
		TEST_ASSERT_M(layers.contains(QStringLiteral("F.Cu"))
			&& layers.contains(QStringLiteral("F.CrtYd")),
			"the layers a footprint draws on come from the shapes, not from the file header");
		TEST_ASSERT_M(closeTo(footprint.value("widthMm").toDouble(), 2.85),
			"the courtyard runs -1.425..1.425");
		TEST_ASSERT_M(closeTo(footprint.value("heightMm").toDouble(), 1.5),
			"a footprint measures Y downward, so a *height* is the one number the flip cannot change");
		TEST_ASSERT(closeTo(footprint.value("courtyardMm").toObject().value("w").toDouble(), 2.85));
		TEST_ASSERT(closeTo(footprint.value("courtyardMm").toObject().value("h").toDouble(), 1.5));

		// The two traps. `(at (xyz 0.0307... 0.0165... 0))` is in inches: read as millimetres the
		// correction becomes a fortieth of what it should be, which looks like no offset at all.
		const QJsonObject model = footprint.value("model3d").toObject();
		TEST_ASSERT_M(model.value("present").toBool(), "the vendor footprint names a .stp");
		TEST_ASSERT_M(closeTo(model.value("offsetMm").toObject().value("x").toDouble(), 0.78),
			"0.030708660290936 inches is 0.78 mm, not 0.031 mm");
		TEST_ASSERT(closeTo(model.value("offsetMm").toObject().value("y").toDouble(), 0.42));
		TEST_ASSERT(closeTo(model.value("offsetMm").toObject().value("z").toDouble(), 0.0));
		// `(rotate (xyz -90 0 0))` is stored the opposite way round from the rotation KiCad
		// applies, so the file's number only matches the picture after the sign flip.
		TEST_ASSERT_M(closeTo(model.value("rotationDeg").toObject().value("x").toDouble(), 90.0),
			"(rotate ...) is stored negated");
		TEST_ASSERT(closeTo(model.value("scale").toObject().value("x").toDouble(), 1.0));

		// The second real file, of another family: one sample proves nothing (ORIENTATION §8).
		QJsonObject optoArgs;
		optoArgs["partId"] = optoId;
		const QJsonObject second = call(tools, "describe_kicad_footprint", optoArgs);
		TEST_ASSERT_M(isOk(second), messageOf(second));
		TEST_COMPARE(second.value("footprintName").toString().toStdString(),
			std::string("SOP254P916X353-6N"));
		TEST_COMPARE(second.value("padCount").toInt(), 6);
		TEST_COMPARE(textsOf(second.value("padNumbers").toArray()).join(QLatin1Char(','))
			.toStdString(), std::string("1,2,3,4,5,6"));
		TEST_ASSERT(!second.value("throughHole").toBool());
		TEST_ASSERT_M(closeTo(second.value("widthMm").toDouble(), 11.1),
			"its courtyard runs -5.55..5.55");
		TEST_ASSERT(closeTo(second.value("heightMm").toDouble(), 9.39));
		// Its pads carry `(at x y 90)`, and a pad turned on its side is genuinely wider than its
		// unrotated size — 0.7 x 2.05 reaches 1.025 either way along x.
		TEST_ASSERT(closeTo(second.value("courtyardMm").toObject().value("w").toDouble(), 11.1));

		// A part with no footprint is told so, by name, rather than answered with zeroes.
		const int bareId = createPart(partTools, resistorCategory, "R 1k 0402", "");
		QJsonObject bare;
		bare["partId"] = bareId;
		const QJsonObject refused = call(tools, "describe_kicad_footprint", bare);
		TEST_ASSERT_M(!isOk(refused), "a part with no footprint must be an error, not a zero answer");
		TEST_ASSERT_M(messageOf(refused).find("list_part_kicad_files") != std::string::npos,
			"the refusal must name the tool that says what the part does carry");

		// And an invented id is refused the way PartToolset refuses one.
		QJsonObject ghost;
		ghost["partId"] = 99999;
		TEST_ASSERT(!isOk(call(tools, "list_part_kicad_files", ghost)));
		TEST_ASSERT(!isOk(call(tools, "describe_kicad_footprint", ghost)));
		TEST_ASSERT(!isOk(call(tools, "describe_kicad_symbol", ghost)));
		TEST_ASSERT(!isOk(call(tools, "find_parts_sharing_a_footprint", ghost)));
	}

	// None of the five real vendor downloads is through-hole, so the true case gets a fixture of
	// its own. The flag comes off the pad's type atom and not off its layer list, because a
	// through-hole pad is usually on "*.Cu" but a file is free to spell its layers out.
	TEST_FUNCTION(aThroughHolePadIsReportedAsOne)
	{
		TEST_START;

		ScopedDatabase database("kicad_tht");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k leaded", "MF0207");
		TEST_ASSERT(partId != 0);
		TEST_ASSERT(importArchiveOnto(*database.handle(), partId, "TEST_THT_2P",
			"TEST_THT_2P.kicad_mod", ThroughHoleFootprint(), std::string()));

		QJsonObject args;
		args["partId"] = partId;
		const QJsonObject described = call(tools, "describe_kicad_footprint", args);
		TEST_ASSERT_M(isOk(described), messageOf(described));
		TEST_ASSERT_M(described.value("throughHole").toBool(),
			"a `(pad 1 thru_hole ...)` must come back as through-hole");
		TEST_COMPARE(described.value("padCount").toInt(), 2);
		TEST_ASSERT(closeTo(described.value("widthMm").toDouble(), 5.08));
		TEST_ASSERT(closeTo(described.value("heightMm").toDouble(), 2.54));
	}

	// `derivedFromFootprint` is the whole point of the tool: only one of a real symbol and a
	// generated stand-in is worth answering questions about, and the model cannot tell them apart
	// any other way.
	TEST_FUNCTION(symbolSaysWhetherItWasDerivedFromTheFootprint)
	{
		TEST_START;

		ScopedDatabase database("kicad_describe_sym");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int withSymbol =
			createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		const int footprintOnly =
			createPart(partTools, categoryId, "R 52k3 0603", "CRCW060352K3FKEA");
		TEST_ASSERT(withSymbol != 0 && footprintOnly != 0);

		TEST_ASSERT(importArchiveOnto(*database.handle(), withSymbol, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), ResistorSymbol0603()));
		TEST_ASSERT(importArchiveOnto(*database.handle(), footprintOnly, "CRCW060352K3FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603Sibling(), std::string()));

		QJsonObject real;
		real["partId"] = withSymbol;
		const QJsonObject drawn = call(tools, "describe_kicad_symbol", real);
		TEST_ASSERT_M(isOk(drawn), messageOf(drawn));
		TEST_ASSERT_M(!drawn.value("derivedFromFootprint").toBool(),
			"a part with its own .kicad_sym has a real symbol, not a stand-in");
		TEST_COMPARE(drawn.value("symbolName").toString().toStdString(),
			std::string("CRCW060342K2FKEA"));
		TEST_COMPARE(drawn.value("pinCount").toInt(), 2);
		const QJsonArray pins = drawn.value("pins").toArray();
		TEST_COMPARE(pins.at(0).toObject().value("number").toString().toStdString(), std::string("1"));
		TEST_ASSERT_M(pins.at(0).toObject().value("type").toString() == QStringLiteral("passive"),
			"a symbol pin carries an electrical type, unlike a library footprint's pads");
		TEST_COMPARE(pins.at(1).toObject().value("name").toString().toStdString(), std::string("2"));
		TEST_ASSERT_M(closeTo(drawn.value("widthMm").toDouble(), 17.78),
			"the body runs 5.08..12.7 and the two pins reach 0 and 17.78");
		TEST_ASSERT(closeTo(drawn.value("heightMm").toDouble(), 2.54));

		// §5a's M5 path: no symbol of its own, so the pins are its pads. They are real copper, so
		// the stand-in places correctly — which is exactly why it has to be labelled as one.
		QJsonObject derived;
		derived["partId"] = footprintOnly;
		const QJsonObject standIn = call(tools, "describe_kicad_symbol", derived);
		TEST_ASSERT_M(isOk(standIn), messageOf(standIn));
		TEST_ASSERT_M(standIn.value("derivedFromFootprint").toBool(),
			"a part with a footprint and no symbol must say its pins came off the pads");
		TEST_COMPARE(standIn.value("pinCount").toInt(), 2);
		// Measured across 30 real `.kicad_mod`: not one carries a `pinfunction`, so an unnamed
		// derived pin is the normal case and not a parse failure.
		TEST_ASSERT_M(standIn.value("pins").toArray().at(0).toObject().value("name")
			.toString().isEmpty(),
			"a library footprint carries no pinfunction, so a derived pin has no name");
		TEST_ASSERT(standIn.value("widthMm").toDouble() > 0.0);

		// The trap the library writer sets: `library()` embeds the `PM_*` base symbols first, so
		// splitSymbols(...)[0] is PM_R and never the part. Attaching a PartManager-shaped library
		// and asking for the part must answer with the part.
		const int generated = createPart(partTools, categoryId, "R 10k 0603", "RC0603FR-0710KL");
		TEST_ASSERT(generated != 0);
		const std::string symbolName =
			PartManager::KicadSymbolWriter::sanitizeSymbolName("R 10k 0603");
		const std::vector<std::string> blocks =
			PartManager::KicadSymbolWriter::splitSymbols(ResistorSymbol0603());
		TEST_ASSERT_M(!blocks.empty(), "the vendor library holds one symbol block");
		const std::string library = PartManager::KicadSymbolWriter::library(
			{ PartManager::KicadSymbolWriter::renamedSymbol(blocks.front(), symbolName) });
		TEST_ASSERT(importArchiveOnto(*database.handle(), generated, "R_10k_0603",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), library));

		QJsonObject embedded;
		embedded["partId"] = generated;
		const QJsonObject picked = call(tools, "describe_kicad_symbol", embedded);
		TEST_ASSERT_M(isOk(picked), messageOf(picked));
		TEST_ASSERT_M(picked.value("symbolName").toString().toStdString() == symbolName,
			"the part's own block must win over the embedded bases, not block zero");
		TEST_ASSERT_M(picked.value("symbolName").toString() != QStringLiteral("PM_R"),
			"describing PM_R would report a bare resistor outline for every generated part");
		TEST_COMPARE(picked.value("pinCount").toInt(), 2);

		// A part with neither file is told so rather than answered with an empty symbol.
		const int bareId = createPart(partTools, categoryId, "R 220R", "");
		QJsonObject bare;
		bare["partId"] = bareId;
		const QJsonObject refused = call(tools, "describe_kicad_symbol", bare);
		TEST_ASSERT(!isOk(refused));
		TEST_ASSERT_M(messageOf(refused).find("set_part_kicad_file") != std::string::npos,
			"the refusal must name the tool that would fix it");
	}

	// The question the second feature request is really made of, on the pair it was written for:
	// two 0603 resistors of one family, whose vendor footprints differ in one line.
	TEST_FUNCTION(theTwoResistorsOfOneFamilyShareAFootprint)
	{
		TEST_START;

		ScopedDatabase database("kicad_sharing");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int first = createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		const int second = createPart(partTools, categoryId, "R 52k3 0603", "CRCW060352K3FKEA");
		const int opto = createPart(partTools, categoryId, "MOC3083SR2M", "MOC3083SR2M");
		TEST_ASSERT(first != 0 && second != 0 && opto != 0);

		TEST_ASSERT(importArchiveOnto(*database.handle(), first, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), std::string()));
		TEST_ASSERT(importArchiveOnto(*database.handle(), second, "CRCW060352K3FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603Sibling(), std::string()));
		TEST_ASSERT(importArchiveOnto(*database.handle(), opto, "MOC3083SR2M",
			"SOP254P916X353-6N.kicad_mod", OptocouplerFootprintSop254(), std::string()));

		QJsonObject args;
		args["partId"] = first;
		const QJsonObject found = call(tools, "find_parts_sharing_a_footprint", args);
		TEST_ASSERT_M(isOk(found), messageOf(found));
		const QJsonArray matches = found.value("matches").toArray();
		TEST_ASSERT_M(matches.size() == 1,
			"exactly the sibling matches — the six-pad optocoupler must not");
		const QJsonObject match = matches.at(0).toObject();
		TEST_COMPARE(match.value("partId").toInt(), second);
		TEST_COMPARE(match.value("mpn").toString().toStdString(), std::string("CRCW060352K3FKEA"));
		TEST_COMPARE(match.value("footprintName").toString().toStdString(),
			std::string("RESC1608X50N"));
		TEST_ASSERT(match.value("padCountMatches").toBool());
		TEST_ASSERT_M(closeTo(match.value("outlineDeltaMm").toDouble(), 0.0),
			"the two land patterns are the same land pattern");
		// The claim that matters. The two vendor files differ in the name of the `.stp` beside
		// them, so they are compatible and *not* the same file — conflating the two would tell a
		// user a footprint had been verified when only its outline had.
		TEST_ASSERT_M(!match.value("identical").toBool(),
			"two files that differ by one byte are compatible, never identical");
		TEST_ASSERT(closeTo(found.value("tolerantMm").toDouble(), 0.05));

		// The optocoupler shares with nobody: six pads against two.
		QJsonObject optoArgs;
		optoArgs["partId"] = opto;
		const QJsonObject optoFound = call(tools, "find_parts_sharing_a_footprint", optoArgs);
		TEST_ASSERT_M(isOk(optoFound), messageOf(optoFound));
		TEST_COMPARE(optoFound.value("matches").toArray().size(), 0);

		// Now make `identical` true the only way the toolset offers: copy the file across. A
		// third part carrying the first one's bytes is byte-for-byte the same file, and the
		// stronger claim has to appear the moment that becomes true.
		QJsonObject copy;
		copy["partId"] = opto;
		copy["role"] = "footprint";
		copy["sourcePartId"] = first;
		const QJsonObject copied = call(tools, "set_part_kicad_file", copy);
		TEST_ASSERT_M(isOk(copied), messageOf(copied));
		TEST_COMPARE(copied.value("role").toString().toStdString(), std::string("footprint"));
		TEST_COMPARE(copied.value("fileName").toString().toStdString(),
			std::string("RESC1608X50N.kicad_mod"));
		TEST_ASSERT_M(copied.value("replacedExisting").toBool(),
			"the optocoupler already had a footprint, and that has to be reported");

		const QJsonArray after =
			call(tools, "find_parts_sharing_a_footprint", args).value("matches").toArray();
		TEST_COMPARE(after.size(), 2);
		bool sawIdentical = false;
		bool sawCompatible = false;
		for (const QJsonValue& entry : after)
		{
			const QJsonObject row = entry.toObject();
			if (row.value("partId").toInt() == opto)
			{
				sawIdentical = row.value("identical").toBool();
			}
			if (row.value("partId").toInt() == second)
			{
				sawCompatible = !row.value("identical").toBool();
			}
		}
		TEST_ASSERT_M(sawIdentical, "the copied file is byte-for-byte the same one");
		TEST_ASSERT_M(sawCompatible, "the sibling is still only compatible");

		// A part with no footprint is told what is missing rather than answered with an empty list.
		const int bareId = createPart(partTools, categoryId, "R 220R", "");
		QJsonObject bare;
		bare["partId"] = bareId;
		TEST_ASSERT(!isOk(call(tools, "find_parts_sharing_a_footprint", bare)));

		// A negative tolerance is a misread parameter, not a filter that matches nothing.
		QJsonObject negative;
		negative["partId"] = first;
		negative["tolerantMm"] = -1.0;
		TEST_ASSERT(!isOk(call(tools, "find_parts_sharing_a_footprint", negative)));
	}

	// The regression guard for a bug this project has already shipped once, in the 3D-model slot:
	// roleFile() resolves a slot by highest id, so reading it *after* the insert returns the row
	// just written, the old one is never detached, and the slot quietly holds two files with the
	// lookup a coin flip between them.
	TEST_FUNCTION(replacingAFileLeavesExactlyOneInTheSlot)
	{
		TEST_START;

		ScopedDatabase database("kicad_single_slot");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int target = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		const int resistorSource =
			createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		const int optoSource = createPart(partTools, categoryId, "MOC3083SR2M", "MOC3083SR2M");
		TEST_ASSERT(target != 0 && resistorSource != 0 && optoSource != 0);

		TEST_ASSERT(importArchiveOnto(*database.handle(), resistorSource, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), ResistorSymbol0603()));
		TEST_ASSERT(importArchiveOnto(*database.handle(), optoSource, "MOC3083SR2M",
			"SOP254P916X353-6N.kicad_mod", OptocouplerFootprintSop254(), std::string()));
		TEST_ASSERT(importArchiveOnto(*database.handle(), target, "TEST_THT_2P",
			"TEST_THT_2P.kicad_mod", ThroughHoleFootprint(), std::string()));
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint), 1);

		QJsonObject copy;
		copy["partId"] = target;
		copy["role"] = "footprint";
		copy["sourcePartId"] = resistorSource;
		const QJsonObject first = call(tools, "set_part_kicad_file", copy);
		TEST_ASSERT_M(isOk(first), messageOf(first));
		TEST_ASSERT(first.value("replacedExisting").toBool());
		TEST_ASSERT_M(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint) == 1,
			"a replaced slot must hold one file, not the new one alongside the old");

		// And again with a different file, which is where reading the slot after the insert would
		// have left the second row behind.
		copy["sourcePartId"] = optoSource;
		const QJsonObject second = call(tools, "set_part_kicad_file", copy);
		TEST_ASSERT_M(isOk(second), messageOf(second));
		TEST_ASSERT_M(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint) == 1,
			"two replaces in a row must still leave one file in the slot");

		// The lookup is not a coin flip: what the slot holds is what was copied last.
		QJsonObject byId;
		byId["partId"] = target;
		const QJsonObject described = call(tools, "describe_kicad_footprint", byId);
		TEST_ASSERT_M(isOk(described), messageOf(described));
		TEST_COMPARE(described.value("footprintName").toString().toStdString(),
			std::string("SOP254P916X353-6N"));
		TEST_COMPARE(described.value("padCount").toInt(), 6);

		// Writing a slot the part did not have reports so rather than claiming a replacement.
		QJsonObject symbolCopy;
		symbolCopy["partId"] = target;
		symbolCopy["role"] = "symbol";
		symbolCopy["sourcePartId"] = resistorSource;
		const QJsonObject symbolWritten = call(tools, "set_part_kicad_file", symbolCopy);
		TEST_ASSERT_M(isOk(symbolWritten), messageOf(symbolWritten));
		TEST_ASSERT_M(!symbolWritten.value("replacedExisting").toBool(),
			"an empty slot was filled, not replaced");
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadSymbol), 1);
		// ...and the footprint slot was not disturbed by writing the symbol one.
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint), 1);

		// A source that carries nothing in that role refuses and changes nothing, naming what it
		// does carry so the model can pick a different source instead of retrying.
		QJsonObject noModel;
		noModel["partId"] = target;
		noModel["role"] = "model3d";
		noModel["sourcePartId"] = resistorSource;
		const QJsonObject refused = call(tools, "set_part_kicad_file", noModel);
		TEST_ASSERT_M(!isOk(refused), "a source with no 3D model must refuse");
		TEST_ASSERT(refused.value("sourceCarries").toObject()
			.value("footprint").toObject().value("attached").toBool());
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::Kicad3DModel), 0);

		// Copying a part's own file onto itself changes nothing and says so.
		QJsonObject itself;
		itself["partId"] = resistorSource;
		itself["role"] = "footprint";
		itself["sourcePartId"] = resistorSource;
		TEST_ASSERT_M(!isOk(call(tools, "set_part_kicad_file", itself)),
			"a part may not be its own source");
		TEST_COMPARE(filesInRole(*database.handle(), resistorSource,
			PartManager::PartFileRole::KicadFootprint), 1);
	}

	// §14c rule 3, in the handler as well as in the schema: a fixed vocabulary declared as free
	// text is how a model wrote "Circuit Protection" into part_type.domain.
	TEST_FUNCTION(aBadRoleIsRefusedWithTheAllowedValues)
	{
		TEST_START;

		ScopedDatabase database("kicad_bad_role");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int target = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		const int source = createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		TEST_ASSERT(target != 0 && source != 0);
		TEST_ASSERT(importArchiveOnto(*database.handle(), source, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), std::string()));

		// The spelling a model reaches for when it reads `role` as "which kind of file": the file
		// extension rather than PartManager's slot name.
		QJsonObject args;
		args["partId"] = target;
		args["role"] = "kicad_mod";
		args["sourcePartId"] = source;
		const QJsonObject refused = call(tools, "set_part_kicad_file", args);
		TEST_ASSERT_M(!isOk(refused), "a role outside the fixed vocabulary must be refused");
		TEST_ASSERT_M(refused.value("allowed_values").toArray().size() == 3,
			"an enum refusal must hand back the values that are allowed");
		TEST_ASSERT_M(messageOf(refused).find("footprint") != std::string::npos,
			"the refusal must name the spellings that would work");
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint), 0);

		// An omitted role is the same refusal, not a default.
		QJsonObject missing;
		missing["partId"] = target;
		missing["sourcePartId"] = source;
		TEST_ASSERT(!isOk(call(tools, "set_part_kicad_file", missing)));

		// Case and whitespace are not a different vocabulary, though.
		QJsonObject shouty;
		shouty["partId"] = target;
		shouty["role"] = "  Footprint ";
		shouty["sourcePartId"] = source;
		const QJsonObject accepted = call(tools, "set_part_kicad_file", shouty);
		TEST_ASSERT_M(isOk(accepted), messageOf(accepted));
		TEST_COMPARE(accepted.value("role").toString().toStdString(), std::string("footprint"));
	}

	// allowWrites == false is enforced in the handlers rather than by leaving tools
	// unregistered, so the model is told why instead of guessing at a missing capability.
	TEST_FUNCTION(readOnlyBlocksBothWritesAndStillDescribes)
	{
		TEST_START;

		ScopedDatabase database("kicad_read_only");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> writable = database.kicadTools();
		const std::vector<PartManager::LlmTool> readOnly = database.readOnlyKicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int target = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		const int source = createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		TEST_ASSERT(target != 0 && source != 0);
		TEST_ASSERT(importArchiveOnto(*database.handle(), source, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), ResistorSymbol0603()));

		// The read-only toolset still offers all six, because the refusal is the message.
		TEST_COMPARE(readOnly.size(), static_cast<size_t>(6));

		QJsonObject copy;
		copy["partId"] = target;
		copy["role"] = "footprint";
		copy["sourcePartId"] = source;
		const QJsonObject refusedCopy = call(readOnly, "set_part_kicad_file", copy);
		TEST_ASSERT_M(!isOk(refusedCopy), "a read-only toolset must refuse set_part_kicad_file");
		TEST_ASSERT_M(messageOf(refusedCopy).find("may not change") != std::string::npos,
			"the refusal must say why, not merely fail");
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint), 0);

		const QJsonObject refusedGenerate = call(readOnly, "generate_kicad_libraries");
		TEST_ASSERT_M(!isOk(refusedGenerate),
			"a read-only toolset must refuse generate_kicad_libraries — it writes files");
		TEST_ASSERT(messageOf(refusedGenerate).find("may not change") != std::string::npos);

		// The four describe/find tools are reads and keep working; that is the point of the flag.
		QJsonObject byId;
		byId["partId"] = source;
		TEST_ASSERT(isOk(call(readOnly, "list_part_kicad_files", byId)));
		const QJsonObject footprint = call(readOnly, "describe_kicad_footprint", byId);
		TEST_ASSERT_M(isOk(footprint), messageOf(footprint));
		TEST_COMPARE(footprint.value("padCount").toInt(), 2);
		TEST_ASSERT(isOk(call(readOnly, "describe_kicad_symbol", byId)));
		TEST_ASSERT(isOk(call(readOnly, "find_parts_sharing_a_footprint", byId)));

		// And the writable toolset can still do what the read-only one refused, so the difference
		// really is the flag and not a broken fixture.
		TEST_ASSERT(isOk(call(writable, "set_part_kicad_file", copy)));
		TEST_COMPARE(filesInRole(*database.handle(), target,
			PartManager::PartFileRole::KicadFootprint), 1);
	}

	// §5a's generator, unchanged, behind a tool: what it wrote, what it left alone, and — the
	// half a user has to be told about — what it skipped and why.
	TEST_FUNCTION(generateReportsWhatItWroteAndWhatItSkipped)
	{
		TEST_START;

		ScopedDatabase database("kicad_generate");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.kicadTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int generated =
			createPart(partTools, categoryId, "R 42k2 0603", "CRCW060342K2FKEA");
		const int empty = createPart(partTools, categoryId, "R 220R", "");
		TEST_ASSERT(generated != 0 && empty != 0);
		TEST_ASSERT(importArchiveOnto(*database.handle(), generated, "CRCW060342K2FKEA",
			"RESC1608X50N.kicad_mod", ResistorFootprint0603(), ResistorSymbol0603()));

		const QJsonObject result = call(tools, "generate_kicad_libraries");
		TEST_ASSERT_M(isOk(result), messageOf(result));
		TEST_ASSERT_M(result.value("written").toArray().size() > 0,
			"a part with both KiCad files must produce a library");
		TEST_ASSERT_M(!result.value("summary").toString().isEmpty(),
			"the summary is the one line a status bar shows");

		// A part with no KiCad files is skipped on purpose, and a skip nobody is told about looks
		// like a broken generator.
		bool sawSkip = false;
		for (const QJsonValue& entry : result.value("skipped").toArray())
		{
			if (entry.toObject().value("part").toString() == QStringLiteral("R 220R"))
			{
				sawSkip = true;
				TEST_ASSERT_M(!entry.toObject().value("reason").toString().isEmpty(),
					"every skip must carry the reason it was skipped");
			}
		}
		TEST_ASSERT_M(sawSkip, "a part with neither a symbol nor a footprint must be named as skipped");

		// Nothing was hand-edited, so nothing is preserved — and running it twice is the same
		// answer, not a growing pile.
		TEST_COMPARE(result.value("preserved").toArray().size(), 0);
		const QJsonObject again = call(tools, "generate_kicad_libraries");
		TEST_ASSERT_M(isOk(again), messageOf(again));
		TEST_COMPARE(again.value("written").toArray().size(),
			result.value("written").toArray().size());
	}

#endif

};

TEST_INSTANTIATE(TST_LlmKicadToolset);
