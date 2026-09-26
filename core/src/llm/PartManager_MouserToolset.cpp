#include "llm/PartManager_MouserToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "mouser/PartManager_MouserClient.h"
#include "mouser/PartManager_MouserSearchService.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_PartFileRole.h"
	#include "domain/PartManager_PartTypeMatcher.h"
	#include "filestore/PartManager_FileStore.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "persistence/PartManager_SellerRepository.h"
	#include "units/PartManager_UnitTable.h"
	#include "SQLite.h"
#endif

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QThread>
#include <algorithm>

namespace PartManager
{
	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// The refusals shared with PartToolset, spelled the same way on purpose: a model that has
		// learned what one of them means has learned what all of them mean.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";
		const char* ReadOnlyMessage = "this assistant may not change the database";

		// UI-only metadata (QtLLM::Tool), never sent to the model, so it is an identifier rather
		// than prose that would need translating.
		const char* ToolGroup = "Mouser";

		// How many entries an error message spells out before it says "and N more".
		constexpr int ErrorListLimit = 40;

		// Rows mouser_search answers with when the model does not say. Five is enough to show a
		// part and its packaging variants; a keyword search that answers fifty rows costs a whole
		// context window to say the same thing.
		constexpr int DefaultSearchLimit = 5;
		constexpr int MaxSearchLimit = 20;

		// Minimum spacing between two Mouser requests, in milliseconds. See throttleMouserRequest().
		constexpr qint64 MouserMinSpacingMs = 2100;


		// ---- argument reading ------------------------------------------------------------
		// Deliberately forgiving about *shape* and strict about *meaning*, exactly as
		// PartToolset.cpp's copies are: a local model quotes its integers about as often as not,
		// and rejecting `"5"` costs a whole turn to re-learn something the handler could read.

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

		bool boolArg(const QJsonObject& args, const QString& key, bool defaultValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isBool())
			{
				return value.toBool();
			}
			if (value.isDouble())
			{
				return value.toDouble() != 0.0;
			}
			if (value.isString())
			{
				const QString text = value.toString().trimmed().toLower();
				if (text == QStringLiteral("true") || text == QStringLiteral("1")
					|| text == QStringLiteral("yes"))
				{
					return true;
				}
				if (text == QStringLiteral("false") || text == QStringLiteral("0")
					|| text == QStringLiteral("no"))
				{
					return false;
				}
			}
			return defaultValue;
		}


		// ---- shared formatting -----------------------------------------------------------

		// User and vendor data straight into a JSON value. Never translated.
		QJsonValue jsonText(const std::string& text)
		{
			return QJsonValue(QString::fromStdString(text));
		}

		// Unicode-aware fold, the same one PartToolset uses to decide two names are one name.
		QString folded(const QString& text)
		{
			return text.trimmed().toCaseFolded();
		}

		QString folded(const std::string& text)
		{
			return folded(QString::fromStdString(text));
		}

		QString idNameSummary(const QStringList& entries)
		{
			if (entries.isEmpty())
			{
				return QStringLiteral("none");
			}
			QStringList shown = entries.mid(0, ErrorListLimit);
			QString text = shown.join(QStringLiteral(", "));
			if (entries.size() > shown.size())
			{
				text += QStringLiteral(" (and %1 more)").arg(entries.size() - shown.size());
			}
			return text;
		}

		QJsonArray categoryChoices(const std::vector<PartType>& types)
		{
			QJsonArray array;
			for (const PartType& type : types)
			{
				QJsonObject entry;
				entry["id"] = type.id;
				entry["name"] = jsonText(type.name);
				entry["parentId"] = type.parentTypeId;
				array.append(entry);
			}
			return array;
		}

		// The same shape PartToolset answers an invented id with, because it is the same mistake:
		// the message names the categories that exist and the payload carries them as data.
		QJsonObject unknownCategoryError(SQLiteWrapper::SQLite& db, int categoryId)
		{
			const std::vector<PartType> types = PartTypeRepository::listTypes(db);
			QStringList labels;
			for (const PartType& type : types)
			{
				labels.append(QStringLiteral("%1=%2").arg(type.id)
					.arg(QString::fromStdString(type.name)));
			}
			QJsonObject extra;
			extra["categories"] = categoryChoices(types);
			return llmError(QStringLiteral("no category with id %1. Existing categories (id=name): %2")
				.arg(categoryId)
				.arg(idNameSummary(labels)), extra);
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


		// ---- talking to Mouser -----------------------------------------------------------

		// Mouser answers HTTP 403 once a key passes roughly 30 requests in a minute, and it arrives
		// mid-run reading exactly like a dead key (ORIENTATION §7; measured 2026-09-03 at 29 rows
		// through a 116-row list). A model that loops on mouser_search would reach that in seconds,
		// so every request this toolset makes is spaced. A normal migration never waits here: its
		// three lookups are tens of seconds apart on their own, because a turn of a local 20B model
		// sits between them. GUI-thread only, like every other handler — the static needs no lock.
		void throttleMouserRequest()
		{
			static qint64 lastRequestMs = 0;
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			const qint64 sinceLast = now - lastRequestMs;
			if (lastRequestMs != 0 && sinceLast >= 0 && sinceLast < MouserMinSpacingMs)
			{
				QThread::msleep(static_cast<unsigned long>(MouserMinSpacingMs - sinceLast));
			}
			lastRequestMs = QDateTime::currentMSecsSinceEpoch();
		}

		// The one place a missing key is reported. It names the variable and says a parameter is
		// not an option, because the next thing a model tries is to offer one (§14f).
		QJsonObject noApiKeyError()
		{
			return llmError("the Mouser API key is not available to this process. It is read from "
				"the MOUSER_SEARCH_API environment variable and from nowhere else — no tool takes "
				"a key as a parameter. Ask the user to set it and restart PartManager.");
		}

		// One article number (or manufacturer part number, or pasted product-page URL) resolved to
		// the prefill the New Part dialog would have been filled with.
		//
		// Both mouser_suggest_category and mouser_import_part go through here and neither may take
		// a different route: a suggestion made from one search and an import made from another can
		// disagree about which packaging variant the number meant, and nothing downstream would
		// notice. Answers false with a ready-to-send error in `outError`.
		bool lookupPrefill(const QString& partNumber, MouserPartPrefill& outPrefill,
			QJsonObject& outError)
		{
			if (!MouserClient::hasApiKey())
			{
				outError = noApiKeyError();
				return false;
			}

			// A pasted URL is reduced to its part number before the API sees it — string work, and
			// a token spent on it is a token wasted (PartManager_MigrationAgent.h).
			const std::string fromUrl = MouserSearchService::partNumberFromUrl(partNumber.toStdString());
			const std::string query = fromUrl.empty() ? partNumber.toStdString() : fromUrl;

			MouserClient client;
			throttleMouserRequest();
			const MouserSearchResult result = client.searchByPartNumber(query);
			if (!result.ok)
			{
				outError = llmError(QStringLiteral("Mouser refused the lookup of \"%1\": %2")
					.arg(QString::fromStdString(query))
					.arg(QString::fromStdString(result.errorMessage)));
				return false;
			}
			if (result.parts.empty())
			{
				outError = llmError(QStringLiteral("Mouser knows no part \"%1\". Use mouser_search "
					"to find the article number first, and pass the mouserPartNumber it answers "
					"with.").arg(QString::fromStdString(query)));
				return false;
			}

			// Mouser answers an article number with the part itself *and* its packaging variants,
			// and the first row it returns is not reliably the one that was asked for — the same
			// ranking MouserSearchDialog and PartImport apply.
			std::vector<MouserPartDto> parts = result.parts;
			MouserSearchService::rankByMatch(parts, query);
			outPrefill = MouserSearchService::toPrefill(parts.front());
			return true;
		}


		// ---- attributes ------------------------------------------------------------------

		// What splitAttributes() concluded: the §2a object to store, and the two lists the answer
		// reports so a model can follow up with add_category_attribute for what did not fit.
		struct AttributeSplit
		{
			QJsonObject stored;
			QStringList written;
			QStringList unmapped;
		};

		// The prefill's attributes filtered down to the ones the *target* category declares.
		//
		// The prefill's keys come from MouserSearchService's own built-in templates, and the
		// category the model picked is the user's. The two agree often and not always, and a key
		// the category never declared would land in `part.attributes` where the editor cannot show
		// it, the search cannot filter on it and nothing ever looks at it again. A unit that
		// disagrees is dropped for the same reason set_part_attribute refuses one: storing a
		// resistance as farads hides the misreading forever.
		AttributeSplit splitAttributes(SQLiteWrapper::SQLite& db, int categoryId,
			const std::string& prefillJson)
		{
			AttributeSplit split;
			const QJsonDocument document =
				QJsonDocument::fromJson(QByteArray::fromStdString(prefillJson));
			if (!document.isObject())
			{
				return split;
			}

			const QJsonObject parsed = document.object();
			const std::vector<PartTypeAttribute> declared =
				PartTypeRepository::effectiveAttributes(db, categoryId);

			for (QJsonObject::const_iterator entry = parsed.begin(); entry != parsed.end(); ++entry)
			{
				const PartTypeAttribute* attribute = nullptr;
				for (const PartTypeAttribute& candidate : declared)
				{
					if (folded(candidate.key) == folded(entry.key()))
					{
						attribute = &candidate;
					}
				}
				if (attribute == nullptr)
				{
					split.unmapped.append(entry.key());
					continue;
				}

				const QJsonValue raw = entry.value();
				const QString storedKey = QString::fromStdString(attribute->key);
				switch (attribute->datatype)
				{
				case AttributeDataType::Number:
				case AttributeDataType::Dimension:
				{
					const QJsonValue number = raw.isObject()
						? raw.toObject().value(QStringLiteral("value")) : raw;
					const QString unit = raw.isObject()
						? raw.toObject().value(QStringLiteral("unit")).toString() : QString();
					if (!number.isDouble()
						|| (!unit.isEmpty() && !UnitTable::unitsEqual(unit.toStdString(), attribute->unit)))
					{
						split.unmapped.append(entry.key());
						continue;
					}
					QJsonObject value;
					value["value"] = number.toDouble();
					value["unit"] = jsonText(attribute->unit);
					split.stored[storedKey] = value;
					break;
				}
				case AttributeDataType::Enum:
				{
					std::string chosen;
					for (const std::string& option : attribute->enumOptions)
					{
						if (folded(option) == folded(raw.toString()))
						{
							chosen = option;
						}
					}
					if (chosen.empty())
					{
						split.unmapped.append(entry.key());
						continue;
					}
					split.stored[storedKey] = jsonText(chosen);
					break;
				}
				case AttributeDataType::Bool:
				{
					if (!raw.isBool())
					{
						split.unmapped.append(entry.key());
						continue;
					}
					split.stored[storedKey] = raw.toBool();
					break;
				}
				case AttributeDataType::Text:
				default:
				{
					// A number where the category wants text is reported rather than stringified:
					// the prefill produced `{"value":...}` for it, and flattening that to "0603"
					// here would be a guess about which of the two numbers was meant.
					if (!raw.isString())
					{
						split.unmapped.append(entry.key());
						continue;
					}
					split.stored[storedKey] = raw.toString();
					break;
				}
				}
				split.written.append(storedKey);
			}
			return split;
		}

		std::string attributeJson(const QJsonObject& attributes)
		{
			return QString::fromUtf8(QJsonDocument(attributes).toJson(QJsonDocument::Compact))
				.toStdString();
		}


		// ---- writing the part ------------------------------------------------------------

		// The part_seller_link row, and the quote hung off it. This is the line the whole toolset
		// exists to get right: the Mouser article number lives here and never in `part.mpn`, which
		// is the *manufacturer's* number and is not what the Cart API accepts (§3, §6).
		bool writeSellerLink(SQLiteWrapper::SQLite& db, int partId, const MouserPartPrefill& prefill)
		{
			if (partId == 0 || prefill.mouserPartNumber.empty())
			{
				// A link with an empty seller_part_number would satisfy "has a Mouser link" while
				// still being unorderable, which is worse than no link at all.
				return false;
			}

			PartSellerLink link;
			link.partId = partId;
			link.sellerId = SellerRepository::ensureMouserSeller(db);
			link.sellerPartNumber = prefill.mouserPartNumber;
			link.url = prefill.productDetailUrl;
			// The first (usually only) seller a part gets is the one the order path should use.
			link.isPrimary = true;
			const int linkId = SellerRepository::linkPart(db, link);
			if (linkId == NoPartSellerLinkId)
			{
				return false;
			}
			if (!prefill.priceBreaks.empty())
			{
				SellerRepository::recordQuote(db, linkId, prefill.priceBreaks);
			}
			return true;
		}

		// Downloads one remote file into the slot, leaving an occupied slot alone. A failure is
		// reported to the caller and never fails the import: a part with no datasheet is a part,
		// and re-running the import backfills it.
		bool attachRemoteFile(FileStore& store, SQLiteWrapper::SQLite& db, int partId,
			PartFileRole role, const std::string& url)
		{
			if (partId == 0 || url.empty())
			{
				return false;
			}
			PartFile occupied;
			if (FileStore::roleFile(db, partId, role, occupied))
			{
				return true;
			}

			const FileStoreResult stored = store.downloadFile(url);
			if (!stored.ok)
			{
				return false;
			}
			const int fileId = store.adoptStoredFile(db, partId, role, stored);
			if (fileId == 0)
			{
				return false;
			}

			// `part.datasheet_file_id` is a second pointer at the same row. The editor falls back
			// to the row now, but the column is kept correct rather than left as a lie.
			if (role == PartFileRole::Datasheet)
			{
				Part owner;
				if (PartRepository::findPart(db, partId, owner) && owner.datasheetFileId != fileId)
				{
					owner.datasheetFileId = fileId;
					PartRepository::updatePart(db, owner);
				}
			}
			return true;
		}

		// The part already in the database carrying this mpn, or 0. Folded, because a manufacturer
		// number typed in by hand and one read off the API differ in case often enough.
		int existingPartWithMpn(SQLiteWrapper::SQLite& db, const std::string& mpn, Part& outPart)
		{
			if (mpn.empty())
			{
				return 0;
			}
			for (const Part& part : PartRepository::listParts(db))
			{
				if (!part.mpn.empty() && folded(part.mpn) == folded(mpn))
				{
					outPart = part;
					return part.id;
				}
			}
			return 0;
		}


		// ---- the tools -------------------------------------------------------------------

		LlmTool makeMouserSearch(const LlmToolContext& context)
		{
			PM_UNUSED(context);
			LlmTool tool;
			tool.schema.setName("mouser_search")
				.setDescription("Looks a part up in Mouser's catalogue. Accepts a Mouser article "
					"number, a manufacturer part number, a pasted product-page URL or free text, "
					"and tries the part-number endpoint before falling back to a keyword search. "
					"Every answer row carries a mouserPartNumber — that is what "
					"mouser_suggest_category and mouser_import_part take, never the mpn and never "
					"the text that was searched for.")
				.setGroup(ToolGroup)
				.addParameter("query", "string",
					"What to look up, e.g. \"595-LM358DR\", \"CRCW060342K2FKEA\" or \"10k 0603 "
					"resistor\".", true)
				.addParameter("limit", "integer",
					"Maximum rows to return, 1-20. Default 5. Mouser answers an article number "
					"with the part itself and its packaging variants, so the first row is normally "
					"the one that was meant.", false);

			tool.handler = [](const QJsonObject& args) -> QJsonObject
			{
				// No database check on purpose: looking a part number up is useful before one is
				// open, and the tools that write say so themselves.
				const QString query = stringArg(args, QStringLiteral("query"));
				if (query.isEmpty())
				{
					return llmError("'query' is required: an article number, a manufacturer part "
						"number, a Mouser product URL or a few words to search for.");
				}
				if (!MouserClient::hasApiKey())
				{
					return noApiKeyError();
				}

				int limit = DefaultSearchLimit;
				if (hasArg(args, QStringLiteral("limit"))
					&& !intArg(args, QStringLiteral("limit"), limit))
				{
					return llmError("'limit' must be a whole number between 1 and 20.");
				}
				limit = std::max(1, std::min(MaxSearchLimit, limit));

				const std::string fromUrl =
					MouserSearchService::partNumberFromUrl(query.toStdString());
				const std::string text = fromUrl.empty() ? query.toStdString() : fromUrl;

				// The part-number endpoint first, the same order MouserSearchDialog uses: asking
				// the model to choose an endpoint asks it to know the answer before it searches.
				MouserClient client;
				throttleMouserRequest();
				MouserSearchResult result = client.searchByPartNumber(text);
				const char* endpoint = "partnumber";
				if (!result.ok || result.parts.empty())
				{
					throttleMouserRequest();
					const MouserSearchResult keyword = client.searchByKeyword(text, limit);
					if (keyword.ok)
					{
						result = keyword;
						endpoint = "keyword";
					}
					else if (!result.ok)
					{
						return llmError(QStringLiteral("Mouser refused the search for \"%1\": %2")
							.arg(QString::fromStdString(text))
							.arg(QString::fromStdString(keyword.errorMessage)));
					}
				}

				std::vector<MouserPartDto> parts = result.parts;
				MouserSearchService::rankByMatch(parts, text);

				QJsonArray results;
				for (const MouserPartDto& dto : parts)
				{
					if (results.size() >= limit)
					{
						break;
					}
					// The prefill is a pure function of the DTO and is what fills `package`: the
					// case code lives in a parametric row, in the description tail or in the part
					// number itself, and which of the three answers varies per manufacturer (§6).
					const MouserPartPrefill prefill = MouserSearchService::toPrefill(dto);
					QJsonObject entry;
					entry["mouserPartNumber"] = jsonText(dto.mouserPartNumber);
					entry["mpn"] = jsonText(dto.manufacturerPartNumber);
					entry["manufacturer"] = jsonText(dto.manufacturer);
					entry["description"] = jsonText(dto.description);
					entry["mouserCategory"] = jsonText(dto.category);
					entry["package"] = jsonText(prefill.part.package);
					entry["hasDatasheet"] = !prefill.datasheetUrl.empty();
					entry["hasImage"] = !prefill.imageUrl.empty();
					results.append(entry);
				}

				QJsonObject payload;
				payload["results"] = results;
				payload["total"] = result.numberOfResults;
				payload["endpoint"] = QString::fromLatin1(endpoint);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeMouserSuggestCategory(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("mouser_suggest_category")
				.setDescription("Matches one Mouser part against the categories this database "
					"actually holds, and says how sure it is. confident=false means nothing fitted "
					"well enough — call list_categories and decide, or create_category to make one. "
					"A near miss is worse than no answer: the wrong category attaches the wrong "
					"attribute template, so the part gets fields it does not have and loses the "
					"ones it does.")
				.setGroup(ToolGroup)
				.addParameter("mouserPartNumber", "string",
					"Mouser article number from mouser_search, e.g. \"71-CRCW0603-42.2K-E3\".", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				const QString partNumber = stringArg(args, QStringLiteral("mouserPartNumber"));
				if (partNumber.isEmpty())
				{
					return llmError("'mouserPartNumber' is required: an article number from "
						"mouser_search.");
				}

				MouserPartPrefill prefill;
				QJsonObject error;
				if (!lookupPrefill(partNumber, prefill, error))
				{
					return error;
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				const std::vector<PartType> types = PartTypeRepository::listTypes(db);
				// Against the user's *own* list, not against MouserSearchService's built-in names:
				// a category the user made themselves ("Elko") could never win a lookup by
				// template name, however plainly the vendor named it (PartTypeMatcher.h).
				const TypeMatch match = matchPartType(types, prefill.mouserCategory,
					prefill.part.description, prefill.suggestedTypeName);

				std::string matchedName;
				for (const PartType& type : types)
				{
					if (type.id == match.typeId)
					{
						matchedName = type.name;
					}
				}
				const bool confident = match.confident && match.typeId != NoParentType
					&& !matchedName.empty();

				QJsonObject payload;
				payload["suggestedCategoryId"] = confident ? match.typeId : 0;
				payload["suggestedCategoryName"] = confident ? jsonText(matchedName) : QJsonValue(QString());
				payload["mouserCategory"] = jsonText(prefill.mouserCategory);
				payload["confident"] = confident;
				payload["mouserPartNumber"] = jsonText(prefill.mouserPartNumber);
				payload["mpn"] = jsonText(prefill.part.mpn);
				payload["description"] = jsonText(prefill.part.description);
				// The reason, as a stable ascii key. For the log and the tests; a model that has
				// it can tell "matched the whole category name" from "found a word in the
				// description" and weigh a non-confident answer accordingly.
				payload["matchedOn"] = jsonText(match.matchedOn);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeMouserImportPart(const LlmToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("mouser_import_part")
				.setDescription("Creates one part in this database from a Mouser article number: "
					"the record, the attributes the chosen category declares, the Mouser article "
					"number as a seller link, and the datasheet and product photo as attachments. "
					"This is the whole import — do not call create_part or set_part_attribute for "
					"a Mouser part, because this path also writes the things those cannot: the "
					"article number belongs in the seller link and never in the mpn field, a price "
					"carries its own currency, and a product image is served under a filename that "
					"lies about its format. An mpn already in the database answers with the part "
					"that is already there instead of making a second one.")
				.setGroup(ToolGroup)
				.addParameter("mouserPartNumber", "string",
					"Mouser article number from mouser_search, e.g. \"871-B72660M0231K072\".", true)
				.addParameter("categoryId", "integer",
					"Category id from list_categories, create_category or "
					"mouser_suggest_category. A category name is not accepted.", true)
				.addParameter("downloadFiles", "boolean",
					"Fetch the datasheet and the product photo. Default true. Everything else is "
					"identical when it is false.", false);

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

				const QString partNumber = stringArg(args, QStringLiteral("mouserPartNumber"));
				if (partNumber.isEmpty())
				{
					return llmError("'mouserPartNumber' is required: an article number from "
						"mouser_search.");
				}

				int categoryId = 0;
				if (!intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return llmError("'categoryId' is required and must be an integer id. Call "
						"mouser_suggest_category or list_categories and use an id from there — a "
						"category name is not accepted.");
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				PartType type;
				if (!PartTypeRepository::findType(db, categoryId, type))
				{
					return unknownCategoryError(db, categoryId);
				}

				// The category is checked before the network call on purpose: an invented id is
				// the most common retry, and paying Mouser a request to discover it wastes one of
				// the ~30 per minute the key gets.
				MouserPartPrefill prefill;
				QJsonObject error;
				if (!lookupPrefill(partNumber, prefill, error))
				{
					return error;
				}

				FileStore store(context.database->filestorePath());
				const bool downloadFiles = boolArg(args, QStringLiteral("downloadFiles"), true);

				// Re-running a migration must not fork the part. The answer is the id that is
				// already there rather than a refusal, for the reason create_category is
				// idempotent: a model handed an error here calls the tool again.
				Part existing;
				const int existingId = existingPartWithMpn(db, prefill.part.mpn, existing);
				if (existingId != 0)
				{
					// Still linked and still attached: a part created before the Mouser link
					// existed would otherwise keep an empty article number forever, and re-running
					// the import is the only way to backfill it.
					const bool linked = writeSellerLink(db, existingId, prefill);
					const bool image = downloadFiles && attachRemoteFile(store, db, existingId,
						PartFileRole::Image, prefill.imageUrl);
					const bool datasheet = downloadFiles && attachRemoteFile(store, db, existingId,
						PartFileRole::Datasheet, prefill.datasheetUrl);

					std::string existingCategory;
					PartType existingType;
					if (PartTypeRepository::findType(db, existing.partTypeId, existingType))
					{
						existingCategory = existingType.name;
					}

					QJsonObject payload;
					payload["partId"] = existingId;
					payload["created"] = false;
					payload["categoryId"] = existing.partTypeId;
					payload["categoryName"] = jsonText(existingCategory);
					payload["mpn"] = jsonText(existing.mpn);
					payload["mouserPartNumber"] = jsonText(prefill.mouserPartNumber);
					payload["attributesWritten"] = QJsonArray();
					payload["unmappedAttributes"] = QJsonArray();
					payload["datasheetAttached"] = datasheet;
					payload["imageAttached"] = image;
					payload["sellerLinkWritten"] = linked;
					payload["note"] = QStringLiteral("a part with this mpn was already in the "
						"database; nothing was duplicated. The part is done — do not create it "
						"again.");
					return llmOk(payload);
				}

				const AttributeSplit attributes =
					splitAttributes(db, categoryId, prefill.part.attributes);

				Part part = prefill.part;
				part.partTypeId = categoryId;
				part.attributes = attributeJson(attributes.stored);
				const int partId = PartRepository::insertPart(db, part);
				if (partId == 0)
				{
					return llmError(QStringLiteral("the part \"%1\" could not be written.")
						.arg(QString::fromStdString(part.name)));
				}

				const bool linked = writeSellerLink(db, partId, prefill);
				const bool image = downloadFiles && attachRemoteFile(store, db, partId,
					PartFileRole::Image, prefill.imageUrl);
				const bool datasheet = downloadFiles && attachRemoteFile(store, db, partId,
					PartFileRole::Datasheet, prefill.datasheetUrl);

				// Mouser's own attribute names that mapped to nothing, plus the mapped ones this
				// category does not declare. Both are the same answer to the model: a value that
				// exists and has nowhere to go, which add_category_attribute can fix.
				QStringList unmapped = attributes.unmapped;
				for (const std::string& name : prefill.unmappedAttributes)
				{
					unmapped.append(QString::fromStdString(name));
				}

				QJsonObject payload;
				payload["partId"] = partId;
				payload["created"] = true;
				payload["categoryId"] = categoryId;
				payload["categoryName"] = jsonText(type.name);
				payload["name"] = jsonText(part.name);
				payload["mpn"] = jsonText(part.mpn);
				payload["mouserPartNumber"] = jsonText(prefill.mouserPartNumber);
				payload["attributesWritten"] = asJsonArray(attributes.written);
				payload["unmappedAttributes"] = asJsonArray(unmapped);
				payload["datasheetAttached"] = datasheet;
				payload["imageAttached"] = image;
				payload["sellerLinkWritten"] = linked;
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> MouserToolset::tools(const LlmToolContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeMouserSearch(context));
		tools.push_back(makeMouserSuggestCategory(context));
		tools.push_back(makeMouserImportPart(context));
		return tools;
#else
		// Two of the three tools write to a database, and a toolset that offered only the search
		// would be a different toolset the caller has to know about. It offers nothing instead.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
