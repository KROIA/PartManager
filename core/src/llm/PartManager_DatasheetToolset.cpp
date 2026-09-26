#include "llm/PartManager_DatasheetToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "pdf/PartManager_PdfText.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_Part.h"
	#include "domain/PartManager_PartFile.h"
	#include "domain/PartManager_PartFileRole.h"
	#include "domain/PartManager_PartType.h"
	#include "filestore/PartManager_FileStore.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "SQLite.h"
#endif

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <string>
#include <vector>

namespace PartManager
{
	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// Spelled exactly as PartToolset, MouserToolset and KicadToolset spell them: a model that
		// has learned what one of these refusals means has learned what all of them mean.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";

		// UI-only metadata (QtLLM::Tool), never sent to the model, so it is an identifier rather
		// than prose that would need translating.
		const char* ToolGroup = "Datasheets";

		// How many entries an error message spells out before it says "and N more".
		constexpr int ErrorListLimit = 40;

		// How many PDFs one `list_datasheets` call will open to count pages. A page count costs a
		// full object scan of the file, so a library with hundreds of datasheets would otherwise
		// turn a listing into a multi-second tool call. Past the budget the field is simply
		// absent, and the answer says so rather than leaving the model to read a missing page
		// count as a broken file.
		constexpr int PageCountBudget = 60;

		// How many pages of each of those a listing actually reads. `pageCount` comes off the
		// page tree and needs none of them, but `scanned` is a *density* judgement (see
		// PdfText.cpp), so a one-page sample would call a datasheet with a sparse cover page a
		// scan and send the model away from a file it could have read. Four pages averages the
		// cover out and still decodes only four content streams.
		constexpr int ListingSamplePages = 4;

		// How far `search_datasheet` reads into one file. Real datasheets run to tens of pages;
		// this is the ceiling for a reference manual that runs to hundreds, and when it bites the
		// answer reports `pagesScanned` so a model knows the tail was not searched.
		constexpr int MaxPagesScanned = 400;

		// How many pages one `read_datasheet` call will return, before `maxChars` cuts it
		// further. A model that asks for pages 1-9999 wants the beginning, not the file.
		constexpr int MaxPagesPerRead = 25;

		// Defaults and bounds for the two caps the model may set itself.
		constexpr int DefaultMaxMatches = 5;
		constexpr int MaxMatchesCeiling = 20;
		constexpr int DefaultMaxChars = 4000;
		constexpr int MaxCharsCeiling = 20000;
		constexpr int SnippetContextChars = 400;


		// ---- argument reading ------------------------------------------------------------
		// The same copies PartToolset.cpp and KicadToolset.cpp carry, and deliberately so:
		// forgiving about *shape* and strict about *meaning*. A local model quotes its integers
		// about as often as not, and rejecting `"5"` costs a whole turn to re-learn something the
		// handler could have read.

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


		// ---- shared formatting -----------------------------------------------------------

		// User data (a part name, the filename the vendor gave their PDF) straight into a JSON
		// value. Never translated — it is what the vendor or the user wrote.
		QJsonValue jsonText(const std::string& text)
		{
			return QJsonValue(QString::fromStdString(text));
		}

		QString idNameSummary(const QStringList& entries)
		{
			if (entries.isEmpty())
			{
				return QStringLiteral("none");
			}
			const QStringList shown = entries.mid(0, ErrorListLimit);
			QString text = shown.join(QStringLiteral(", "));
			if (entries.size() > shown.size())
			{
				text += QStringLiteral(" (and %1 more)").arg(entries.size() - shown.size());
			}
			return text;
		}

		QJsonObject unknownCategoryError(SQLiteWrapper::SQLite& db, int categoryId)
		{
			const std::vector<PartType> types = PartTypeRepository::listTypes(db);
			QStringList labels;
			QJsonArray choices;
			for (const PartType& type : types)
			{
				labels.append(QStringLiteral("%1=%2").arg(type.id)
					.arg(QString::fromStdString(type.name)));
				QJsonObject entry;
				entry["id"] = type.id;
				entry["name"] = jsonText(type.name);
				entry["parentId"] = type.parentTypeId;
				choices.append(entry);
			}
			QJsonObject extra;
			extra["categories"] = choices;
			return llmError(QStringLiteral("no category with id %1. Existing categories (id=name): %2")
				.arg(categoryId).arg(idNameSummary(labels)), extra);
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

		QJsonObject missingIdError(const QString& name, const QString& hint)
		{
			return llmError(QStringLiteral("'%1' is required and must be an integer id. %2")
				.arg(name).arg(hint));
		}


		// ---- reaching the file --------------------------------------------------------------

		// A part's datasheet, as a path on disk. Everything about this is internal: §14f's rule
		// is that no tool takes or returns a filesystem path, so this never leaves the handler —
		// the model names a part and gets text back.
		//
		// `Datasheet` is a single-slot role (FileStore.h), so "the" datasheet is well defined.
		bool datasheetPathOf(SQLiteWrapper::SQLite& db, FileStore& store, int partId,
			PartFile& outFile, std::string& outPath)
		{
			if (!FileStore::roleFile(db, partId, PartFileRole::Datasheet, outFile))
			{
				return false;
			}
			outPath = store.absolutePath(outFile.relativePath);
			// An empty path is a row pointing at bytes that are no longer on disk — a normal
			// state, because a database folder can be copied without its filestore.
			return !outPath.empty();
		}

		// The name a human would recognise the file by, preferring what the vendor called it
		// over the content-addressed path it is stored under.
		std::string displayName(const PartFile& file)
		{
			return file.originalFilename.empty() ? file.relativePath : file.originalFilename;
		}

		QJsonObject noDatasheetError(int partId, const Part& part)
		{
			return llmError(QStringLiteral("part %1 (\"%2\") carries no datasheet. Use "
				"list_datasheets to see which parts have one — most parts do not, because Mouser "
				"returns an empty DataSheetUrl for most real parts.")
				.arg(partId).arg(QString::fromStdString(part.name)));
		}

		// The one refusal that matters most in this toolset. A datasheet that could not be read
		// must come back as a *failure that names itself*: a model handed an empty answer will
		// fill the gap from its own memory of what the part does, and that is the single worst
		// thing this feature could do.
		QJsonObject unreadableError(int partId, const PartFile& file, const PdfTextResult& text)
		{
			QJsonObject extra;
			extra["fileName"] = jsonText(displayName(file));
			return llmError(QStringLiteral("the datasheet attached to part %1 (\"%2\") could not "
				"be read: %3. Say so — do not answer from memory of what this part does.")
				.arg(partId)
				.arg(QString::fromStdString(displayName(file)))
				.arg(QString::fromStdString(text.error)), extra);
		}

		// The other half of the same rule. A file that parsed, is not a scan, and still yielded
		// nothing is a file this extractor did not understand — vector-outlined text, a font
		// whose codes carry no `/ToUnicode`, a truncated download. Every one of those looks
		// exactly like "the datasheet does not mention that" unless it is said out loud.
		QJsonObject noTextError(int partId, const PartFile& file, int pageCount)
		{
			QJsonObject extra;
			extra["fileName"] = jsonText(displayName(file));
			extra["pageCount"] = pageCount;
			return llmError(QStringLiteral("no text could be extracted from the datasheet of part "
				"%1 (\"%2\"), although the file itself parsed and is not a scan — its %3 page%4 "
				"may be drawn as outlines rather than set as text. Tell the user you cannot read "
				"this datasheet; do not answer from memory of what the part does.")
				.arg(partId)
				.arg(QString::fromStdString(displayName(file)))
				.arg(pageCount)
				.arg(pageCount == 1 ? QString() : QStringLiteral("s")), extra);
		}

		int countNonBlank(const std::string& text)
		{
			int count = 0;
			for (const char c : text)
			{
				if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != '\f')
				{
					++count;
				}
			}
			return count;
		}


		// ---- the tools -------------------------------------------------------------------

		LlmTool makeListDatasheets(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("list_datasheets")
				.setDescription("Which parts carry a datasheet file, and how many pages it has. "
					"\"hasDatasheet\": false is the common case and not a problem — most parts in "
					"a real library have no datasheet attached. An entry that carries one but "
					"reports \"scanned\": true is a picture of text: nothing can read it, and the "
					"honest answer about that part is that you cannot. Call this first to find "
					"the partId that search_datasheet should work on.")
				.setGroup(ToolGroup)
				.addParameter("categoryId", "integer",
					"Restrict to this one category. Child categories are not included. Omit to "
					"list the whole database.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}

				int categoryId = 0;
				if (hasArg(args, QStringLiteral("categoryId"))
					&& !intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Omit it to list the whole database."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				if (categoryId != 0)
				{
					PartType type;
					if (!PartTypeRepository::findType(db, categoryId, type))
					{
						return unknownCategoryError(db, categoryId);
					}
				}

				FileStore store(context.database->filestorePath());
				QJsonArray entries;
				int withDatasheet = 0;
				int opened = 0;
				bool omittedAnyPageCount = false;
				for (const Part& part : PartRepository::listParts(db))
				{
					if (categoryId != 0 && part.partTypeId != categoryId)
					{
						continue;
					}
					QJsonObject entry;
					entry["partId"] = part.id;
					entry["name"] = jsonText(part.name);
					entry["mpn"] = jsonText(part.mpn);

					PartFile file;
					std::string path;
					if (!datasheetPathOf(db, store, part.id, file, path))
					{
						entry["hasDatasheet"] = false;
						entry["fileName"] = QString();
						entries.append(entry);
						continue;
					}
					++withDatasheet;
					entry["hasDatasheet"] = true;
					entry["fileName"] = jsonText(displayName(file));
					if (opened >= PageCountBudget)
					{
						omittedAnyPageCount = true;
						entries.append(entry);
						continue;
					}
					++opened;
					// `pageCount` is read off the page tree and is the real total whatever
					// `maxPages` says, so this pays for the object scan and for four content
					// streams rather than for a hundred.
					const PdfTextResult text = PdfText::extract(path, ListingSamplePages);
					if (!text.ok)
					{
						entry["error"] = jsonText(text.error);
					}
					else
					{
						entry["pageCount"] = text.pageCount;
						entry["scanned"] = text.looksScanned;
					}
					entries.append(entry);
				}

				QJsonObject payload;
				payload["parts"] = entries;
				payload["total"] = entries.size();
				payload["withDatasheet"] = withDatasheet;
				if (omittedAnyPageCount)
				{
					// Said out loud, because a missing pageCount and an unreadable file look the
					// same to a model that was not told which happened.
					payload["pageCountsOmitted"] = true;
					payload["pageCountBudget"] = PageCountBudget;
				}
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSearchDatasheet(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("search_datasheet")
				.setDescription("Finds where a part's datasheet says something, and hands back a "
					"few hundred characters around each hit with the page it is on. This is the "
					"tool to use for a question about a part's specification — search for the "
					"term (\"supply voltage\", \"thermal resistance\", \"pin 3\") rather than "
					"reading the file. Quote the snippet and cite its page. An empty \"matches\" "
					"with status ok means the datasheet does not contain that term, which is an "
					"answer. \"scanned\": true means the file is a picture of text and there is "
					"nothing to search — say that, and do not answer from memory instead. The "
					"text is extracted without layout, so a table comes back as interleaved "
					"words; that is why every hit carries its page number.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or list_datasheets.",
					true)
				.addParameter("query", "string",
					"Text to look for, case-insensitive. A short phrase works better than a "
					"sentence — the datasheet has to contain it literally.", true)
				.addParameter("maxMatches", "integer",
					"How many hits to return, 1-20. Default 5.", false);

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
						QStringLiteral("Call list_datasheets or search_parts to find one."));
				}
				const QString query = stringArg(args, QStringLiteral("query"));
				if (query.isEmpty())
				{
					return llmError("'query' is required and must be the text to look for, e.g. "
						"\"supply voltage\". There is no way to ask for the whole datasheet; use "
						"read_datasheet with a page number for that.");
				}
				int maxMatches = DefaultMaxMatches;
				if (hasArg(args, QStringLiteral("maxMatches"))
					&& !intArg(args, QStringLiteral("maxMatches"), maxMatches))
				{
					return llmError("'maxMatches' must be a whole number between 1 and 20.");
				}
				maxMatches = std::max(1, std::min(MaxMatchesCeiling, maxMatches));

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				FileStore store(context.database->filestorePath());
				PartFile file;
				std::string path;
				if (!datasheetPathOf(db, store, partId, file, path))
				{
					return noDatasheetError(partId, part);
				}

				const PdfTextResult text = PdfText::extract(path, MaxPagesScanned);
				if (!text.ok)
				{
					return unreadableError(partId, file, text);
				}

				const int textChars = countNonBlank(text.allText());
				if (textChars == 0 && !text.looksScanned)
				{
					return noTextError(partId, file, text.pageCount);
				}

				QJsonArray matches;
				for (const PdfTextMatch& match : PdfText::search(text, query.toStdString(),
					maxMatches, SnippetContextChars))
				{
					QJsonObject entry;
					entry["page"] = match.page;
					entry["snippet"] = jsonText(match.snippet);
					matches.append(entry);
				}

				QJsonObject payload;
				payload["scanned"] = text.looksScanned;
				payload["pageCount"] = text.pageCount;
				payload["fileName"] = jsonText(displayName(file));
				// How much text there was to search at all. A scan and a file whose fonts could
				// not be decoded both produce no matches, and the difference between "the
				// datasheet does not say that" and "nothing could be read out of this file" is
				// the difference between an answer and an invention.
				payload["textChars"] = textChars;
				if (text.pageCount > static_cast<int>(text.pages.size()))
				{
					payload["pagesScanned"] = static_cast<int>(text.pages.size());
				}
				payload["matches"] = matches;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeReadDatasheet(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("read_datasheet")
				.setDescription("The text of one page range of a part's datasheet, for when you "
					"already know which page you want — search_datasheet is the tool that finds "
					"it. Capped at 25 pages and at maxChars characters; when the cap cuts the "
					"text the answer says \"truncated\": true, and an answer built on a truncated "
					"page must say it was cut off rather than finish the sentence itself. "
					"\"scanned\": true means the file is a picture of text and there is nothing "
					"to read. There is no layout: a table comes back as interleaved words.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer", "Part id from search_parts or list_datasheets.",
					true)
				.addParameter("fromPage", "integer",
					"First page to read, 1-based as the datasheet prints it. Default 1.", false)
				.addParameter("toPage", "integer",
					"Last page to read, inclusive. Default is the same as fromPage — one page.",
					false)
				.addParameter("maxChars", "integer",
					"Character cap on the returned text, 1-20000. Default 4000.", false);

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
						QStringLiteral("Call list_datasheets or search_parts to find one."));
				}

				int fromPage = 1;
				if (hasArg(args, QStringLiteral("fromPage"))
					&& !intArg(args, QStringLiteral("fromPage"), fromPage))
				{
					return llmError("'fromPage' must be a whole page number, 1 for the first page.");
				}
				if (fromPage < 1)
				{
					return llmError(QStringLiteral("'fromPage' is 1-based, as the datasheet prints "
						"its pages — %1 is not a page.").arg(fromPage));
				}
				int toPage = fromPage;
				if (hasArg(args, QStringLiteral("toPage"))
					&& !intArg(args, QStringLiteral("toPage"), toPage))
				{
					return llmError("'toPage' must be a whole page number, or omitted to read the "
						"single page 'fromPage' names.");
				}
				if (toPage < fromPage)
				{
					toPage = fromPage;
				}
				int maxChars = DefaultMaxChars;
				if (hasArg(args, QStringLiteral("maxChars"))
					&& !intArg(args, QStringLiteral("maxChars"), maxChars))
				{
					return llmError("'maxChars' must be a whole number between 1 and 20000.");
				}
				maxChars = std::max(1, std::min(MaxCharsCeiling, maxChars));
				toPage = std::min(toPage, fromPage + MaxPagesPerRead - 1);

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				FileStore store(context.database->filestorePath());
				PartFile file;
				std::string path;
				if (!datasheetPathOf(db, store, partId, file, path))
				{
					return noDatasheetError(partId, part);
				}

				// `pageCount` is read off the page tree and is the real total whatever `maxPages`
				// caps the extraction at, so asking for what was requested is enough to find out
				// the request was past the end.
				const PdfTextResult text = PdfText::extract(path, toPage);
				if (!text.ok)
				{
					return unreadableError(partId, file, text);
				}
				if (fromPage > text.pageCount)
				{
					QJsonObject extra;
					extra["pageCount"] = text.pageCount;
					return llmError(QStringLiteral("the datasheet of part %1 (\"%2\") has %3 "
						"page%4; page %5 is past its end.")
						.arg(partId)
						.arg(QString::fromStdString(displayName(file)))
						.arg(text.pageCount)
						.arg(text.pageCount == 1 ? QString() : QStringLiteral("s"))
						.arg(fromPage), extra);
				}
				if (countNonBlank(text.allText()) == 0 && !text.looksScanned)
				{
					return noTextError(partId, file, text.pageCount);
				}
				toPage = std::min(toPage, text.pageCount);

				std::string joined;
				for (const PdfTextPage& page : text.pages)
				{
					if (page.number < fromPage || page.number > toPage)
					{
						continue;
					}
					if (!joined.empty())
					{
						joined += '\n';
					}
					joined += page.text;
				}
				const bool truncated = static_cast<int>(joined.size()) > maxChars;
				if (truncated)
				{
					joined.resize(static_cast<size_t>(maxChars));
				}

				QJsonObject payload;
				payload["scanned"] = text.looksScanned;
				payload["pageCount"] = text.pageCount;
				payload["fromPage"] = fromPage;
				payload["toPage"] = toPage;
				payload["truncated"] = truncated;
				payload["fileName"] = jsonText(displayName(file));
				payload["textChars"] = countNonBlank(joined);
				payload["text"] = jsonText(joined);
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> DatasheetToolset::tools(const LlmToolContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeListDatasheets(context));
		tools.push_back(makeSearchDatasheet(context));
		tools.push_back(makeReadDatasheet(context));
		return tools;
#else
		// No SQLite means no parts, and a datasheet is only ever reached through one (§14f). The
		// toolset still exists so a caller does not have to know which optional dependency was
		// left out; it simply offers nothing.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
