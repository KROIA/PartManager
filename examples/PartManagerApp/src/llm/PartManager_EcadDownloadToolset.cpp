#include "llm/PartManager_EcadDownloadToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "controllers/PartManager_PartEditorController.h"
#include "database/PartManager_DatabaseHandle.h"
#include "import/PartManager_EcadArchive.h"
#include "llm/PartManager_LlmUiBridge.h"
#include "pdf/PartManager_PdfText.h"
#include "settings/PartManager_Settings.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_Part.h"
	#include "domain/PartManager_PartFile.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "SQLite.h"
#endif

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <string>
#include <vector>

namespace PartManager
{
	bool EcadDownloadContext::isUsable() const
	{
		return database != nullptr && database->isOpen();
	}

	QStringList EcadDownloadContext::defaultRoots()
	{
		QStringList roots;
		// The same fallback EcadFetchDialog::downloadsFolder() makes, and for the same reason: a
		// profile that reports no Downloads location would otherwise leave the assistant with an
		// empty allow-list and no way to say what is missing.
		QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
		if (downloads.isEmpty())
		{
			downloads = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
		}
		if (!downloads.isEmpty())
		{
			roots << downloads;
		}

		// §14f: empty is the default and means Downloads only. A configured folder that no longer
		// exists is dropped rather than reported — the user set it on another machine or on
		// another drive, and every listing complaining about it helps nobody.
		const QString extra = QString::fromStdString(Settings::getPreferences().llmDownloadFolder)
			.trimmed();
		if (!extra.isEmpty())
		{
			roots << extra;
		}

		QStringList existing;
		for (const QString& root : roots)
		{
			const QFileInfo info(root);
			if (info.isDir() && !existing.contains(info.absoluteFilePath()))
			{
				existing << info.absoluteFilePath();
			}
		}
		return existing;
	}

	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// Spelled exactly as the other toolsets spell it: a model that has learned one of these
		// refusals has learned all of them.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";

		// The host registered the tools with nothing to look in. It cannot happen in the app —
		// LlmController only registers them when defaultRoots() found a folder — but a handler
		// that captured an empty list must still answer rather than list the whole disk.
		const char* NoFolderMessage = "there is no download folder configured for me to look in. "
			"Tell the user to save the archive in their Downloads folder.";

		const char* ReadOnlyMessage = "this assistant may not change the database, so it cannot "
			"attach files to a part. Say so; there is nothing to retry.";

		// UI-only metadata (QtLLM::Tool), never sent to the model, so it is an identifier rather
		// than prose that would need translating.
		const char* ToolGroup = "Downloaded libraries";

		// A listing the model can actually read. Twenty is plenty for "I just downloaded it" and
		// short enough that a whole Downloads folder does not become the turn's context.
		constexpr int DefaultMaxResults = 20;
		constexpr int MaxMaxResults = 200;

		// How many file names a "nothing matched" message spells out before it stops.
		constexpr int SampleLimit = 10;


		// ---- argument reading --------------------------------------------------------------
		// The same copies the other toolsets carry, and deliberately so: forgiving about *shape*
		// and strict about *meaning*. A local model quotes its integers about as often as not.

		// Null *and* "" both mean absent. Measured on gpt-oss:20b, 2026-09-27: asked for a listing
		// filtered by mpn it sent `"modifiedAfter": null` on one call and `"modifiedAfter": ""` on
		// the next, and refusing the empty string as an unparseable date cost a whole turn to
		// re-learn what "optional" meant. Every filter here omits cleanly, so an empty one is an
		// absence rather than a value to validate.
		bool hasArg(const QJsonObject& args, const QString& key)
		{
			const QJsonValue value = args.value(key);
			if (!args.contains(key) || value.isNull())
			{
				return false;
			}
			return !value.isString() || !value.toString().trimmed().isEmpty();
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

		// "within 2 hours" and "within 0.5 hours" are both things a model writes, so this is the
		// one numeric argument that is not an integer.
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

		QString stringArg(const QJsonObject& args, const QString& key)
		{
			return args.value(key).toString().trimmed();
		}


		// ---- the allow-list ----------------------------------------------------------------

		// Windows matches paths case-insensitively and Linux does not, and a check that gets this
		// wrong either lets a file through or refuses one that is really there.
		bool samePath(const QString& left, const QString& right)
		{
#ifdef Q_OS_WIN
			return QString::compare(left, right, Qt::CaseInsensitive) == 0;
#else
			return left == right;
#endif
		}

		// The rule §14f now carries, stated where it is enforced. A name that fails this is an
		// error that names the rule: the model has to be able to tell "I may not ask for that"
		// from "there is no such file", and a silent empty result says neither.
		QJsonObject badFileNameError(const QString& name, const QString& why, const QString& suffix)
		{
			return llmError(QStringLiteral("'%1' is not a file name I can use: %2. Pass the "
				"\"file\" value from a list_downloaded_libraries result exactly as it came back — "
				"a bare name like \"LIB_74HC4051PW-Q100,11%3\". I can only reach %3 files sitting "
				"directly in the user's download folder, and I never take a path.")
				.arg(name).arg(why).arg(suffix));
		}

		// Resolves a model-supplied *name* to a real file inside one of the allowed roots. Every
		// path in this toolset comes from here; no handler builds one itself.
		bool resolveInRoots(const EcadDownloadContext& context, const QString& rawName,
			const QString& suffix, QString& outPath, QJsonObject& outError)
		{
			const QString name = rawName.trimmed();
			if (name.isEmpty())
			{
				outError = llmError(QStringLiteral("'file' is required: the name of a downloaded "
					"%1 file, exactly as list_downloaded_libraries reported it.").arg(suffix));
				return false;
			}
			if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
			{
				outError = badFileNameError(name, QStringLiteral("it contains a path separator"),
					suffix);
				return false;
			}
			if (name.contains(QLatin1Char(':')))
			{
				outError = badFileNameError(name,
					QStringLiteral("it contains ':', which names a drive or a stream"), suffix);
				return false;
			}
			if (name.contains(QStringLiteral("..")))
			{
				outError = badFileNameError(name,
					QStringLiteral("it contains '..', which would leave the folder"), suffix);
				return false;
			}
			if (!name.endsWith(suffix, Qt::CaseInsensitive))
			{
				outError = badFileNameError(name,
					QStringLiteral("it is not a %1 file").arg(suffix), suffix);
				return false;
			}
			if (context.allowedRoots.isEmpty())
			{
				outError = llmError(QString::fromLatin1(NoFolderMessage));
				return false;
			}

			QStringList searched;
			for (const QString& root : context.allowedRoots)
			{
				const QString rootPath = QFileInfo(root).canonicalFilePath();
				if (rootPath.isEmpty())
				{
					continue;
				}
				searched << QDir::toNativeSeparators(rootPath);

				const QFileInfo candidate(QDir(rootPath).filePath(name));
				if (!candidate.exists() || !candidate.isFile())
				{
					continue;
				}
				// Canonical, so a symlink or a junction pointing out of the folder resolves to
				// where it really goes — and then fails this. Direct children only: nothing here
				// walks into a subfolder, so the parent has to *be* the root rather than merely
				// start with it, which is also the prefix trick ("...\Downloads2\x.zip") that a
				// startsWith() check gets wrong.
				const QString resolved = candidate.canonicalFilePath();
				if (resolved.isEmpty() || !samePath(QFileInfo(resolved).absolutePath(), rootPath))
				{
					outError = badFileNameError(name, QStringLiteral(
						"it resolves to a file outside the download folder"), suffix);
					return false;
				}
				outPath = resolved;
				return true;
			}

			outError = llmError(QStringLiteral("no file named '%1' in %2. Call "
				"list_downloaded_libraries to see what is actually there — the user may not have "
				"finished the download, or may have saved it somewhere else.")
				.arg(name).arg(searched.join(QStringLiteral(", "))));
			return false;
		}

		bool resolveArchive(const EcadDownloadContext& context, const QString& rawName,
			QString& outPath, QJsonObject& outError)
		{
			return resolveInRoots(context, rawName, QStringLiteral(".zip"), outPath, outError);
		}

		// The datasheet's own rule, and the **second** place §14f is relaxed: a bare name still
		// resolves inside the allowed roots like everything else here, and an *absolute* path is
		// accepted as well. The user asked for that in those words — a datasheet is usually saved
		// wherever they happened to be looking, and the alternative is telling them to move a file
		// into Downloads so the assistant is allowed to see it. Still `.pdf` only, still has to
		// exist, and the refusal says which of the two failed rather than only that it did.
		bool resolvePdf(const EcadDownloadContext& context, const QString& rawName,
			QString& outPath, QJsonObject& outError)
		{
			const QString name = rawName.trimmed();
			const QFileInfo given(name);
			if (!given.isAbsolute())
			{
				return resolveInRoots(context, name, QStringLiteral(".pdf"), outPath, outError);
			}
			if (!name.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
			{
				outError = llmError(QStringLiteral("'%1' is not a PDF. A datasheet has to be a "
					".pdf file; ask the user for the right one.").arg(name));
				return false;
			}
			if (!given.exists() || !given.isFile())
			{
				outError = llmError(QStringLiteral("there is no file at '%1'. Read the path back "
					"to the user and ask them to check it — I cannot look around for it.")
					.arg(QDir::toNativeSeparators(name)));
				return false;
			}
			outPath = given.canonicalFilePath();
			return true;
		}

		// Every `*.zip` in the allowed roots, newest first, one entry per name. The roots are
		// walked in the order resolveArchive() walks them, so a name that appears in two folders
		// is listed once and resolves to the same file it was listed from.
		QFileInfoList candidateArchives(const EcadDownloadContext& context)
		{
			QFileInfoList all;
			QStringList seen;
			for (const QString& root : context.allowedRoots)
			{
				const QFileInfoList entries = QDir(root).entryInfoList(
					QStringList() << QStringLiteral("*.zip"), QDir::Files, QDir::Time);
				for (const QFileInfo& entry : entries)
				{
					bool duplicate = false;
					for (const QString& name : seen)
					{
						if (samePath(name, entry.fileName()))
						{
							duplicate = true;
							break;
						}
					}
					if (duplicate)
					{
						continue;
					}
					seen << entry.fileName();
					all << entry;
				}
			}
			std::sort(all.begin(), all.end(), [](const QFileInfo& left, const QFileInfo& right)
				{
					return left.lastModified() > right.lastModified();
				});
			return all;
		}

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


		// ---- the tools ---------------------------------------------------------------------

		LlmTool makeListDownloads(const EcadDownloadContext& context)
		{
			LlmTool tool;
			tool.schema.setName("list_downloaded_libraries")
				.setDescription("The ZIP archives sitting in the user's download folder, newest "
					"first. This is how you find a symbol/footprint library the user downloaded "
					"themselves from Mouser, Component Search Engine, SnapEDA or Ultra Librarian "
					"— when they say \"I already downloaded the library/the ECAD zip for this "
					"part\", call this instead of asking them for a path. Every filter is "
					"optional; with none, you get the newest archives. \"modified\" is ISO-8601 "
					"UTC-offset local time: use the current_date_time tool if you need to reason "
					"about \"today\" or \"just now\". The \"file\" value is what "
					"inspect_downloaded_library and attach_downloaded_library take — pass it back "
					"exactly, it is a name and never a path.")
				.setGroup(ToolGroup)
				.addParameter("mpn", "string",
					"Manufacturer part number to match the file name against, e.g. "
					"\"74HC4051PW-Q100,11\". Punctuation and a browser's \"(1)\" suffix are "
					"ignored, so this finds \"LIB_74HC4051PW-Q100,11(5).zip\". The best filter "
					"when you know which part the user means.", false)
				.addParameter("nameContains", "string",
					"Plain case-insensitive text the file name must contain. Use it when the user "
					"described the file rather than naming the part.", false)
				.addParameter("modifiedWithinHours", "number",
					"Only archives modified in the last N hours. \"I downloaded it just now\" is "
					"about 1; \"today\" is 24.", false)
				.addParameter("modifiedAfter", "string",
					"Only archives modified after this ISO-8601 moment, e.g. \"2026-09-27\" or "
					"\"2026-09-27T14:30:00\". Use current_date_time to work out what to pass.",
					false)
				.addParameter("maxResults", "integer",
					"How many to return, newest first. Default 20.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (context.allowedRoots.isEmpty())
				{
					return llmError(QString::fromLatin1(NoFolderMessage));
				}

				QDateTime cutoff;
				if (hasArg(args, QStringLiteral("modifiedWithinHours")))
				{
					double hours = 0.0;
					if (!doubleArg(args, QStringLiteral("modifiedWithinHours"), hours) || hours <= 0.0)
					{
						return llmError(QStringLiteral("'modifiedWithinHours' must be a positive "
							"number of hours, e.g. 24 for \"today\". Omit it for no time limit."));
					}
					cutoff = QDateTime::currentDateTime().addSecs(
						static_cast<qint64>(-hours * 3600.0));
				}
				if (hasArg(args, QStringLiteral("modifiedAfter")))
				{
					const QString text = stringArg(args, QStringLiteral("modifiedAfter"));
					QDateTime parsed = QDateTime::fromString(text, Qt::ISODate);
					if (!parsed.isValid())
					{
						// A bare date is what a model writes for "today", and refusing it would
						// cost a turn to learn something readable as it stands.
						const QDate day = QDate::fromString(text, Qt::ISODate);
						if (day.isValid())
						{
							parsed = QDateTime(day, QTime(0, 0));
						}
					}
					if (!parsed.isValid())
					{
						return llmError(QStringLiteral("'modifiedAfter' must be ISO-8601, like "
							"\"2026-09-27\" or \"2026-09-27T14:30:00\". Got '%1'. Call "
							"current_date_time if you need to know what \"today\" is.").arg(text));
					}
					// Both filters given: the later one wins, which is what "and" means here.
					if (!cutoff.isValid() || parsed > cutoff)
					{
						cutoff = parsed;
					}
				}

				int maxResults = DefaultMaxResults;
				if (hasArg(args, QStringLiteral("maxResults")))
				{
					if (!intArg(args, QStringLiteral("maxResults"), maxResults) || maxResults <= 0)
					{
						return llmError(QStringLiteral("'maxResults' must be a positive whole "
							"number. Omit it for the default of %1.").arg(DefaultMaxResults));
					}
					maxResults = std::min(maxResults, MaxMaxResults);
				}

				const QString mpn = stringArg(args, QStringLiteral("mpn"));
				const QString nameContains = stringArg(args, QStringLiteral("nameContains"));

				const QFileInfoList candidates = candidateArchives(context);
				QJsonArray files;
				int matched = 0;
				QStringList rejectedSample;
				for (const QFileInfo& entry : candidates)
				{
					if (cutoff.isValid() && entry.lastModified() < cutoff)
					{
						continue;
					}
					if (!nameContains.isEmpty()
						&& !entry.fileName().contains(nameContains, Qt::CaseInsensitive))
					{
						continue;
					}
					if (!mpn.isEmpty() && !EcadArchive::matchesPartNumber(
						entry.fileName().toStdString(), mpn.toStdString()))
					{
						if (rejectedSample.size() < SampleLimit)
						{
							rejectedSample << entry.fileName();
						}
						continue;
					}
					++matched;
					if (files.size() >= maxResults)
					{
						continue;
					}
					QJsonObject file;
					file["file"] = entry.fileName();
					file["sizeBytes"] = static_cast<double>(entry.size());
					// ISO-8601 and not "2 hours ago": the model has current_date_time and can
					// subtract, and a relative time computed here is one more thing to get wrong.
					file["modified"] = entry.lastModified().toString(Qt::ISODate);
					files.append(file);
				}

				QJsonArray folders;
				for (const QString& root : context.allowedRoots)
				{
					folders.append(QDir::toNativeSeparators(root));
				}

				QJsonObject payload;
				payload["folders"] = folders;
				payload["files"] = files;
				payload["matched"] = matched;
				payload["returned"] = files.size();
				if (matched > files.size())
				{
					payload["note"] = QStringLiteral("%1 archives matched; the %2 newest are "
						"listed. Narrow it with mpn or modifiedWithinHours rather than raising "
						"maxResults.").arg(matched).arg(files.size());
				}
				else if (matched == 0)
				{
					// Which of the two "nothing" cases this is decides what the user should do,
					// so it is answered rather than left to the model to guess.
					payload["note"] = candidates.isEmpty()
						? QStringLiteral("the download folder holds no .zip archives at all. Ask "
							"the user to download the library first, or to say where they saved it.")
						: QStringLiteral("%1 archives are there, none matching. Nearest by name: "
							"%2. Ask the user which file they mean, or list again without filters.")
							.arg(candidates.size())
							.arg(rejectedSample.isEmpty()
								? QStringLiteral("none") : rejectedSample.join(QStringLiteral(", ")));
				}
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeInspectDownload(const EcadDownloadContext& context)
		{
			LlmTool tool;
			tool.schema.setName("inspect_downloaded_library")
				.setDescription("What is inside one downloaded ZIP: whether it carries a KiCad "
					"symbol, a footprint and a 3D model. Reads the archive's table of contents "
					"only — nothing is extracted and nothing is attached to any part. Call it "
					"before attach_downloaded_library when you are not certain the file is the "
					"right one, and to tell the user what they are about to get. "
					"\"legacyKicadOnly\": true means the archive has a KiCad folder holding only "
					"KiCad 5 files (.lib/.dcm/.mod), which PartManager cannot use — the user has "
					"to download the current KiCad version of the model.")
				.setGroup(ToolGroup)
				.addParameter("file", "string",
					"File name from list_downloaded_libraries, exactly as it came back. A name, "
					"never a path.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				QString path;
				QJsonObject error;
				if (!resolveArchive(context, stringArg(args, QStringLiteral("file")), path, error))
				{
					return error;
				}

				const EcadArchiveContents contents = EcadArchive::inspect(path.toStdString());
				if (!contents.ok)
				{
					return llmError(QStringLiteral("could not read '%1': %2. A download that is "
						"still running looks like this; ask the user to check it finished.")
						.arg(QFileInfo(path).fileName())
						.arg(QString::fromStdString(contents.errorMessage)));
				}

				// The entry names are inside the archive, not on the user's disk, so they are safe
				// to report — and they are what tells the user this really is their part.
				auto slot = [](const std::string& entry)
				{
					QJsonObject item;
					item["present"] = !entry.empty();
					if (!entry.empty())
					{
						item["entry"] = QString::fromStdString(entry);
					}
					return item;
				};

				QJsonObject payload;
				payload["file"] = QFileInfo(path).fileName();
				payload["hasAnything"] = contents.hasAnything();
				payload["symbol"] = slot(contents.symbolEntry);
				payload["footprint"] = slot(contents.footprintEntry);
				payload["model3d"] = slot(contents.modelEntry);
				payload["legacyKicadOnly"] = contents.legacyKicadOnly;
				payload["ignoredEntries"] = contents.ignoredEntries;
				if (!contents.hasAnything())
				{
					payload["note"] = contents.legacyKicadOnly
						? QStringLiteral("this archive does have KiCad files, but only KiCad 5 "
							"ones (.lib/.dcm/.mod), which PartManager does not import. Tell the "
							"user to download it again choosing KiCad 6 or newer.")
						: QStringLiteral("there is nothing in here PartManager can use: no "
							".kicad_sym, no .kicad_mod and no 3D model. It may be the wrong "
							"archive, or an export for a different CAD tool.");
				}
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeAttachDownload(const EcadDownloadContext& context)
		{
			LlmTool tool;
			tool.schema.setName("attach_downloaded_library")
				.setDescription("Attaches the KiCad symbol, footprint and 3D model out of a "
					"downloaded ZIP to a part. This is the step that finishes \"I downloaded the "
					"library for this part\": find the file with list_downloaded_libraries, then "
					"call this with the part's id. Each file replaces whatever that slot held, and "
					"the contents are copied into PartManager's own store, so the user can delete "
					"the ZIP afterwards. Afterwards, say which of the three were attached — an "
					"archive often carries only some of them.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer",
					"Part id from search_parts, create_part, get_part or ui_get_state. The part "
					"the files belong to.", true)
				.addParameter("file", "string",
					"File name from list_downloaded_libraries, exactly as it came back. A name, "
					"never a path.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(QString::fromLatin1(NoDatabaseMessage));
				}
				if (!context.allowWrites)
				{
					return llmError(QString::fromLatin1(ReadOnlyMessage));
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return llmError(QStringLiteral("'partId' is required and must be an integer "
						"id. Call search_parts or ui_get_state to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				QString path;
				QJsonObject error;
				if (!resolveArchive(context, stringArg(args, QStringLiteral("file")), path, error))
				{
					return error;
				}

				// The whole write path, filestore included, already exists and is what the ECAD
				// download dialog calls. The controller holds nothing but the handle, so building
				// one here shares no state with the app's own — there is none to share.
				PartEditorController editor(context.database);
				const PartEditorController::EcadImportSummary summary =
					editor.importEcadArchive(partId, path.toStdString());

				const QString fileName = QFileInfo(path).fileName();
				if (!summary.ok)
				{
					return llmError(QStringLiteral("could not import '%1': %2")
						.arg(fileName)
						.arg(summary.errorMessage.empty()
							? QStringLiteral("the archive could not be read")
							: QString::fromStdString(summary.errorMessage)));
				}

				QJsonObject payload;
				payload["partId"] = partId;
				payload["name"] = QString::fromStdString(part.name);
				payload["file"] = fileName;
				payload["symbolAttached"] = summary.symbolAttached;
				payload["footprintAttached"] = summary.footprintAttached;
				payload["modelAttached"] = summary.modelAttached;
				payload["legacyKicadOnly"] = summary.legacyKicadOnly;
				payload["ignoredEntries"] = summary.ignoredEntries;

				if (!summary.symbolAttached && !summary.footprintAttached && !summary.modelAttached)
				{
					// An error and not a cheerful nothing: the call did not do what its name says,
					// and both causes are things the user can act on. `extra` carries the flags so
					// the model can still report *why* precisely.
					QJsonObject extra;
					extra["partId"] = partId;
					extra["file"] = fileName;
					extra["legacyKicadOnly"] = summary.legacyKicadOnly;
					extra["ignoredEntries"] = summary.ignoredEntries;
					return llmError(summary.legacyKicadOnly
						? QStringLiteral("'%1' has a KiCad folder, but only KiCad 5 files "
							"(.lib/.dcm/.mod) in it, so nothing was attached to part %2 (\"%3\"). "
							"The archive does support KiCad — just not a version PartManager can "
							"read. Ask the user to download it again choosing KiCad 6 or newer.")
							.arg(fileName).arg(partId).arg(QString::fromStdString(part.name))
						: QStringLiteral("'%1' holds no KiCad symbol, footprint or 3D model, so "
							"nothing was attached to part %2 (\"%3\"). Check it with "
							"inspect_downloaded_library, or ask the user whether this is the right "
							"archive.")
							.arg(fileName).arg(partId).arg(QString::fromStdString(part.name)),
						extra);
				}
				if (summary.legacyKicadOnly)
				{
					payload["note"] = QStringLiteral("the archive's KiCad folder held only KiCad 5 "
						"files, so the symbol and footprint come from elsewhere in it or were not "
						"attached. Say which of the three you got.");
				}
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSetPartDatasheet(const EcadDownloadContext& context)
		{
			LlmTool tool;
			tool.schema.setName("set_part_datasheet")
				.setDescription("Gives a part its datasheet, from a web address or from a PDF on "
					"the user's machine. Pass exactly one of \"url\" and \"file\". A part holds "
					"one datasheet: this replaces whatever it had. Afterwards the result says "
					"whether the PDF's text could be read — \"readable\": false with "
					"\"looksScanned\": true means it is a scan of paper, so search_datasheet will "
					"find nothing in it and you must not answer questions from it; say so instead.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer",
					"Part id from search_parts, create_part, get_part or ui_get_state.", true)
				.addParameter("url", "string",
					"Direct http(s) link to the PDF — a manufacturer's datasheet URL, or the "
					"DataSheetUrl a mouser_search result carried. Not a product page: this "
					"downloads whatever is at the address.", false)
				.addParameter("file", "string",
					"A PDF the user already has: either a file name from "
					"list_downloaded_libraries, or a full path they gave you, e.g. "
					"\"C:\\\\Users\\\\me\\\\Documents\\\\74hc4051.pdf\". Never invent a path — use "
					"this only with one the user typed or a name a listing gave you.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(QString::fromLatin1(NoDatabaseMessage));
				}
				if (!context.allowWrites)
				{
					return llmError(QString::fromLatin1(ReadOnlyMessage));
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return llmError(QStringLiteral("'partId' is required and must be an integer "
						"id. Call search_parts or ui_get_state to find one."));
				}

				const bool hasUrl = hasArg(args, QStringLiteral("url"));
				const bool hasFile = hasArg(args, QStringLiteral("file"));
				// Both and neither are the two shapes a model produces when it has misread this,
				// and they need different corrections — so they get different messages.
				if (hasUrl == hasFile)
				{
					return llmError(hasUrl
						? QStringLiteral("give either 'url' or 'file', not both. Which one does "
							"the user actually have — a link, or a PDF on their machine?")
						: QStringLiteral("one of 'url' or 'file' is required: a direct http(s) "
							"link to the PDF, or a PDF the user already has. Ask them which, and "
							"do not guess a URL."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				// One controller, both paths: §3's contract is that each writes the `part_file`
				// row and leaves `part.datasheetFileId` in memory for the caller's save (below).
				PartEditorController editor(context.database);
				std::string error;
				int fileId = 0;
				QString source;
				if (hasUrl)
				{
					const QString url = stringArg(args, QStringLiteral("url"));
					// Checked here rather than left to the downloader: a model that has been asked
					// for a URL sometimes answers with a file path or a bare part number, and
					// "could not download" is a far worse account of that than naming it.
					if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
						&& !url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
					{
						return llmError(QStringLiteral("'%1' is not a web address. 'url' takes a "
							"http:// or https:// link to the PDF itself; for a file on the user's "
							"machine use 'file' instead.").arg(url));
					}
					source = QStringLiteral("url");
					fileId = editor.downloadDatasheet(part, url.toStdString(), &error);
					if (fileId == 0)
					{
						return llmError(QStringLiteral("could not download the datasheet from "
							"%1: %2").arg(url).arg(QString::fromStdString(error)));
					}
				}
				else
				{
					QString path;
					QJsonObject refused;
					if (!resolvePdf(context, stringArg(args, QStringLiteral("file")), path, refused))
					{
						return refused;
					}
					source = QStringLiteral("file");
					fileId = editor.attachDatasheet(part, path.toStdString(), &error);
					if (fileId == 0)
					{
						return llmError(QStringLiteral("could not attach '%1': %2")
							.arg(QFileInfo(path).fileName())
							.arg(QString::fromStdString(error)));
					}
				}

				// §3: both calls above only set `part.datasheetFileId` in memory — writing the
				// part back is the caller's §10 autosave, and here the caller is this handler. The
				// file is stored either way, so skipping this would leave a datasheet the part
				// does not point at.
				if (!editor.savePart(part))
				{
					return llmError(QStringLiteral("the datasheet was stored but part %1 could "
						"not be updated to point at it. Tell the user; the part is unchanged.")
						.arg(partId));
				}

				// The editor is modeless (§10), so it may be open on this very part holding the
				// copy of the row this handler has just made stale — whose next autosave would put
				// the old datasheet id straight back. Told to re-read rather than raced with.
				const bool reloaded = context.ui != nullptr
					&& context.ui->reloadOpenPartEditor(partId);

				QJsonObject payload;
				payload["partId"] = partId;
				payload["name"] = QString::fromStdString(part.name);
				payload["source"] = source;
				payload["editorReloaded"] = reloaded;

				PartFile stored;
				if (editor.datasheetFile(part, stored))
				{
					payload["file"] = QString::fromStdString(stored.originalFilename);
					payload["sizeBytes"] = static_cast<double>(stored.sizeBytes);
				}

				// §14g, and cheap where it matters: the model learns *now* whether this datasheet
				// can be searched at all. Finding out two turns later, from an empty
				// search_datasheet, is what makes a model answer from its own memory of the part.
				const std::string storedPath = editor.datasheetPath(part);
				const PdfTextResult text = PdfText::extract(storedPath);
				payload["readable"] = text.ok && !text.looksScanned && !text.allText().empty();
				payload["looksScanned"] = text.looksScanned;
				payload["pageCount"] = text.pageCount;
				if (!text.ok)
				{
					payload["readable"] = false;
					payload["note"] = QStringLiteral("the file was attached, but its text could "
						"not be read: %1. Tell the user it is stored and that you cannot search "
						"it.").arg(QString::fromStdString(text.error));
				}
				else if (text.looksScanned)
				{
					payload["note"] = QStringLiteral("this datasheet is a scan — images of pages, "
						"no text. It is attached and the user can open it, but search_datasheet "
						"will find nothing and you must not answer specifications from it.");
				}
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> EcadDownloadToolset::tools(const EcadDownloadContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeListDownloads(context));
		tools.push_back(makeInspectDownload(context));
		tools.push_back(makeAttachDownload(context));
		tools.push_back(makeSetPartDatasheet(context));
		return tools;
#else
		// Without SQLite there are no parts to attach an archive to, and listing files the
		// assistant could do nothing with is not a feature. The toolset still exists so a caller
		// need not know which optional dependency was left out; it simply offers nothing.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
