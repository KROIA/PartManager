#include "llm/PartManager_UiToolset.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1

#include "database/PartManager_DatabaseHandle.h"
#include "llm/PartManager_LlmUiBridge.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_Part.h"
	#include "domain/PartManager_PartType.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_PartTypeRepository.h"
	#include "SQLite.h"
#endif

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

namespace PartManager
{
	bool UiToolContext::isUsable() const
	{
		return ui != nullptr && database != nullptr && database->isOpen();
	}

	namespace
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

		// Spelled exactly as the four core toolsets spell them: a model that has learned what one
		// of these refusals means has learned what all of them mean.
		const char* NoDatabaseMessage = "no database is open. Ask the user to open one first.";

		// The other half of that, and the one only this toolset can hit: the tools were registered
		// by a host that has no window to drive. It cannot happen in the app — LlmController only
		// registers them when it was handed a bridge — but a handler that has captured a null must
		// still answer rather than crash.
		const char* NoWindowMessage = "there is no Component Browser window to read or drive. Say "
			"so; there is nothing to retry.";

		// UI-only metadata (QtLLM::Tool), never sent to the model, so it is an identifier rather
		// than prose that would need translating.
		const char* ToolGroup = "App window";

		// How many entries an error message spells out before it says "and N more".
		constexpr int ErrorListLimit = 40;


		// ---- argument reading ------------------------------------------------------------
		// The same copies PartToolset.cpp, KicadToolset.cpp and DatasheetToolset.cpp carry, and
		// deliberately so: forgiving about *shape* and strict about *meaning*. A local model quotes
		// its integers about as often as not, and rejecting `"5"` costs a whole turn to re-learn
		// something the handler could have read.

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

		// Same forgiveness one type further along. A model that has been told `"boolean"` still
		// answers `"true"` and, from the smaller ones, `1` — and a tick box set from the wrong one
		// of those is a change to the user's view made for no reason, so the shapes that are
		// unambiguous are accepted and everything else is refused rather than guessed at.
		bool boolArg(const QJsonObject& args, const QString& key, bool& outValue)
		{
			const QJsonValue value = args.value(key);
			if (value.isBool())
			{
				outValue = value.toBool();
				return true;
			}
			if (value.isDouble())
			{
				const double raw = value.toDouble();
				if (raw != 0.0 && raw != 1.0)
				{
					return false;
				}
				outValue = raw != 0.0;
				return true;
			}
			if (value.isString())
			{
				const QString text = value.toString().trimmed().toLower();
				if (text == QStringLiteral("true") || text == QStringLiteral("yes")
					|| text == QStringLiteral("1"))
				{
					outValue = true;
					return true;
				}
				if (text == QStringLiteral("false") || text == QStringLiteral("no")
					|| text == QStringLiteral("0"))
				{
					outValue = false;
					return true;
				}
			}
			return false;
		}


		// ---- shared formatting -----------------------------------------------------------

		// User data (a part name, a category name) straight into a JSON value. Never translated —
		// it is what the user typed.
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
			for (const PartType& type : types)
			{
				labels.append(QStringLiteral("%1=%2").arg(type.id)
					.arg(QString::fromStdString(type.name)));
			}
			return llmError(QStringLiteral("no category with id %1. Existing categories (id=name): "
				"%2").arg(categoryId).arg(idNameSummary(labels)));
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

		// The one refusal only this toolset has: a real row the user's own §7a filters are keeping
		// off the screen. It is reported apart from an unknown id because it is the fixable one —
		// the model can clear the filter with ui_set_filter and try again, and telling it "no such
		// category" instead would send it looking for a different id that does not exist either.
		QJsonObject hiddenByFilterError(const QString& what, const LlmUiBridge& ui)
		{
			QStringList blame;
			if (!ui.treeFilter().trimmed().isEmpty())
			{
				blame.append(QStringLiteral("the category filter is set to \"%1\"")
					.arg(ui.treeFilter()));
			}
			if (ui.hideEmptyCategories())
			{
				blame.append(QStringLiteral("empty categories are hidden"));
			}
			QJsonObject extra;
			extra["treeFilter"] = ui.treeFilter();
			extra["hideEmptyCategories"] = ui.hideEmptyCategories();
			if (blame.isEmpty())
			{
				// Neither filter is on and the tree still does not carry it. That is the category
				// being gone from the tree for a reason this toolset cannot name, and inventing one
				// would send the model round a loop clearing filters that are already clear.
				return llmError(QStringLiteral("%1, but it is not in the category tree and no "
					"filter is hiding it. Tell the user; do not retry.").arg(what), extra);
			}
			return llmError(QStringLiteral("%1, but it is not in the category tree because %2. "
				"Clear that with ui_set_filter and call this again.")
				.arg(what).arg(blame.join(QStringLiteral(" and "))), extra);
		}


		// ---- the tools -------------------------------------------------------------------

		LlmTool makeGetState(const UiToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("ui_get_state")
				.setDescription("What the user has selected in the Component Browser right now: "
					"the open category, the highlighted part, and the four search boxes. Call this "
					"whenever the user says \"the selected part\", \"this component\", \"the one I "
					"am looking at\" or anything else that points at the screen instead of naming "
					"a part — it is the only way to find out which part they mean, and the partId "
					"it returns is the one to hand to search_datasheet, get_part or update_part. "
					"\"selected\": false means nothing is selected: ask the user which part they "
					"mean rather than picking one.")
				.setGroup(ToolGroup);

			tool.handler = [context](const QJsonObject&) -> QJsonObject
			{
				if (context.ui == nullptr)
				{
					return llmError(NoWindowMessage);
				}
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				const LlmUiBridge& ui = *context.ui;
				SQLiteWrapper::SQLite& db = context.database->connection();

				QJsonObject category;
				const int categoryId = ui.selectedCategoryId();
				category["selected"] = categoryId != NoParentType;
				if (categoryId != NoParentType)
				{
					category["categoryId"] = categoryId;
					category["name"] = ui.selectedCategoryName();
				}
				else
				{
					// Null rather than 0. A bare zero reads as an id, and a model that takes it as
					// one goes looking for category zero instead of asking the user.
					category["categoryId"] = QJsonValue(QJsonValue::Null);
					category["note"] = QStringLiteral(
						"nothing selected: no category is open in the tree.");
				}

				QJsonObject part;
				QString rowName;
				const int partId = ui.selectedPartId(&rowName);
				part["selected"] = partId != 0;
				if (partId != 0)
				{
					part["partId"] = partId;
					// Read back out of the database rather than off the table cell: the cell holds
					// whatever §7b's first column is configured to show, which is the part name
					// today and need not stay that way, and it carries no MPN at all.
					Part row;
					if (PartRepository::findPart(db, partId, row))
					{
						part["name"] = jsonText(row.name);
						part["mpn"] = jsonText(row.mpn);
						part["categoryId"] = row.partTypeId;
					}
					else
					{
						// The row went away under a table that has not been refreshed yet. Said out
						// loud, because every other tool in the app will refuse this id next.
						part["name"] = rowName;
						part["note"] = QStringLiteral(
							"this part is selected in the table but no longer in the database — "
							"the view is stale. Tell the user to refresh.");
					}
				}
				else
				{
					part["partId"] = QJsonValue(QJsonValue::Null);
					part["note"] = QStringLiteral(
						"nothing selected: the user has not highlighted a row in the part table. "
						"Ask which part they mean; do not guess one.");
				}

				QJsonObject filters;
				// §7a. All four are reported even when empty, so "the user has no search running"
				// is an answer rather than an absence the model has to infer.
				filters["tableFilter"] = ui.tableFilter();
				filters["treeFilter"] = ui.treeFilter();
				filters["allCategoriesSearch"] = ui.allCategoriesSearch();
				filters["hideEmptyCategories"] = ui.hideEmptyCategories();

				QJsonObject payload;
				payload["category"] = category;
				payload["part"] = part;
				payload["filters"] = filters;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSelectCategory(const UiToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("ui_select_category")
				.setDescription("Opens a category in the Component Browser's tree, so the user sees "
					"its parts. Use it to show the user where something is rather than only "
					"describing it. The id must have come out of a tool result in this "
					"conversation — list_categories or ui_get_state.")
				.setGroup(ToolGroup)
				.addParameter("categoryId", "integer",
					"Category id from list_categories, create_category or ui_get_state.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (context.ui == nullptr)
				{
					return llmError(NoWindowMessage);
				}
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int categoryId = 0;
				if (!intArg(args, QStringLiteral("categoryId"), categoryId))
				{
					return missingIdError(QStringLiteral("categoryId"),
						QStringLiteral("Call list_categories or ui_get_state to find one."));
				}

				// The database is asked before the window is: §14c rule 2 wants an unknown id
				// answered with the ids that exist, and the tree cannot tell an id that names
				// nothing from one a filter is hiding.
				SQLiteWrapper::SQLite& db = context.database->connection();
				PartType type;
				if (!PartTypeRepository::findType(db, categoryId, type))
				{
					return unknownCategoryError(db, categoryId);
				}
				if (!context.ui->selectCategory(categoryId))
				{
					return hiddenByFilterError(QStringLiteral("category %1 (\"%2\") exists")
						.arg(categoryId).arg(QString::fromStdString(type.name)), *context.ui);
				}

				QJsonObject payload;
				payload["categoryId"] = categoryId;
				payload["name"] = jsonText(type.name);
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSelectPart(const UiToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("ui_select_part")
				.setDescription("Selects a part in the Component Browser, so the user is looking at "
					"the one you are talking about. It opens the part's category on the way if "
					"that is a different one, and clears the user's table search if that is what "
					"is hiding the row — both are changes to their view, so both are reported "
					"back. When \"filterCleared\" is true, tell the user you cleared their search "
					"and what it was: \"clearedTableFilter\" carries the text so you can offer to "
					"put it back.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer",
					"Part id from search_parts, get_part or ui_get_state.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (context.ui == nullptr)
				{
					return llmError(NoWindowMessage);
				}
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts or ui_get_state to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				// Read before the call, because the call is what empties it.
				const QString previousTableFilter = context.ui->tableFilter();
				const SelectPartResult result = context.ui->selectPart(partId);
				if (!result.selected)
				{
					// `found` is already known to be true — the repository just answered. So this
					// is the tree filter leaving the part's category out, which is the one case
					// selectPart() refuses to fix on its own: clearing a second filter the user set
					// is a bigger decision than showing one part.
					return hiddenByFilterError(QStringLiteral("part %1 (\"%2\") exists and is filed "
						"under a category")
						.arg(partId).arg(QString::fromStdString(part.name)), *context.ui);
				}

				QJsonObject payload;
				payload["partId"] = partId;
				payload["name"] = jsonText(part.name);
				payload["categoryId"] = part.partTypeId;
				payload["categoryChanged"] = result.categoryChanged;
				payload["filterCleared"] = result.filterCleared;
				if (result.filterCleared)
				{
					payload["clearedTableFilter"] = previousTableFilter;
				}
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeSetFilter(const UiToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("ui_set_filter")
				.setDescription("Changes the Component Browser's search boxes. Every parameter is "
					"optional and an omitted one is left exactly as the user had it — so send only "
					"the ones you mean to change. An empty string is a value, not an omission: it "
					"is how a search gets cleared. These are the user's own boxes; say what you "
					"changed afterwards.")
				.setGroup(ToolGroup)
				.addParameter("tableFilter", "string",
					"The search box above the part table. It filters the open category's rows and "
					"takes the same query syntax the user types, including tag: terms. \"\" clears "
					"it.", false)
				.addParameter("treeFilter", "string",
					"The search box above the category tree. It hides categories that match "
					"nothing. \"\" clears it.", false)
				.addParameter("allCategoriesSearch", "boolean",
					"When true the table search covers the whole database instead of only the open "
					"category.", false)
				.addParameter("hideEmptyCategories", "boolean",
					"When true, categories with no parts filed under them are left out of the "
					"tree.", false);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (context.ui == nullptr)
				{
					return llmError(NoWindowMessage);
				}
				LlmUiBridge& ui = *context.ui;

				const bool wantsTable = hasArg(args, QStringLiteral("tableFilter"));
				const bool wantsTree = hasArg(args, QStringLiteral("treeFilter"));
				const bool wantsAll = hasArg(args, QStringLiteral("allCategoriesSearch"));
				const bool wantsHideEmpty = hasArg(args, QStringLiteral("hideEmptyCategories"));
				if (!wantsTable && !wantsTree && !wantsAll && !wantsHideEmpty)
				{
					// A refusal rather than a no-op on purpose: an empty call is what a model sends
					// when it has read the tool as "show me the filters", and answering it with a
					// cheerful "changed nothing" teaches it that the call worked.
					return llmError("nothing to change: name at least one of 'tableFilter', "
						"'treeFilter', 'allCategoriesSearch' or 'hideEmptyCategories'. Use "
						"ui_get_state to read what they are set to now.");
				}

				// Every argument is parsed before any of them is applied, so a call that names a
				// good filter and a malformed tick box leaves the view untouched instead of half
				// changed — the user would have no way of telling which half took.
				bool allCategories = false;
				if (wantsAll && !boolArg(args, QStringLiteral("allCategoriesSearch"), allCategories))
				{
					return llmError("'allCategoriesSearch' must be true or false.");
				}
				bool hideEmpty = false;
				if (wantsHideEmpty
					&& !boolArg(args, QStringLiteral("hideEmptyCategories"), hideEmpty))
				{
					return llmError("'hideEmptyCategories' must be true or false.");
				}
				if (wantsTable && !args.value(QStringLiteral("tableFilter")).isString())
				{
					return llmError("'tableFilter' must be a string. Send \"\" to clear the box.");
				}
				if (wantsTree && !args.value(QStringLiteral("treeFilter")).isString())
				{
					return llmError("'treeFilter' must be a string. Send \"\" to clear the box.");
				}

				// Reported as from/to pairs rather than as the new state alone, because the user's
				// previous search is the thing they will want back and the model cannot ask for it
				// again once it is gone.
				QJsonArray changed;
				auto record = [&changed](const char* control, const QJsonValue& from,
					const QJsonValue& to)
				{
					QJsonObject entry;
					entry["control"] = QString::fromLatin1(control);
					entry["from"] = from;
					entry["to"] = to;
					changed.append(entry);
				};

				if (wantsTable)
				{
					const QString text = args.value(QStringLiteral("tableFilter")).toString();
					const QString before = ui.tableFilter();
					if (before != text)
					{
						ui.setTableFilter(text);
						record("tableFilter", before, text);
					}
				}
				if (wantsTree)
				{
					const QString text = args.value(QStringLiteral("treeFilter")).toString();
					const QString before = ui.treeFilter();
					if (before != text)
					{
						ui.setTreeFilter(text);
						record("treeFilter", before, text);
					}
				}
				if (wantsAll && ui.allCategoriesSearch() != allCategories)
				{
					record("allCategoriesSearch", ui.allCategoriesSearch(), allCategories);
					ui.setAllCategoriesSearch(allCategories);
				}
				if (wantsHideEmpty && ui.hideEmptyCategories() != hideEmpty)
				{
					record("hideEmptyCategories", ui.hideEmptyCategories(), hideEmpty);
					ui.setHideEmptyCategories(hideEmpty);
				}

				QJsonObject payload;
				payload["changed"] = changed;
				if (changed.isEmpty())
				{
					// Everything named was already set that way. An ok answer, but one that has to
					// say so — otherwise the model reports a change the user cannot see.
					payload["note"] = QStringLiteral("every control you named was already set to "
						"that value; nothing on screen changed.");
				}
				// What the boxes hold now, so the model does not need a second call to find out.
				QJsonObject filters;
				filters["tableFilter"] = ui.tableFilter();
				filters["treeFilter"] = ui.treeFilter();
				filters["allCategoriesSearch"] = ui.allCategoriesSearch();
				filters["hideEmptyCategories"] = ui.hideEmptyCategories();
				payload["filters"] = filters;
				return llmOk(payload);
			};
			return tool;
		}

		LlmTool makeOpenPartEditor(const UiToolContext& context)
		{
			LlmTool tool;
			tool.schema.setName("ui_open_part_editor")
				.setDescription("Opens the part editor window on a part, which is what the user "
					"gets by double-clicking a row. Use it when they ask to edit, see or check a "
					"part's fields — it puts the whole record in front of them instead of you "
					"reciting it. The window is not modal, so this returns immediately and the "
					"user can keep talking to you. \"raisedExisting\": true means that part was "
					"already open and its window was brought to the front; say that rather than "
					"claiming you opened it.")
				.setGroup(ToolGroup)
				.addParameter("partId", "integer",
					"Part id from search_parts, get_part or ui_get_state.", true);

			tool.handler = [context](const QJsonObject& args) -> QJsonObject
			{
				if (context.ui == nullptr)
				{
					return llmError(NoWindowMessage);
				}
				if (!context.isUsable())
				{
					return llmError(NoDatabaseMessage);
				}
				int partId = 0;
				if (!intArg(args, QStringLiteral("partId"), partId))
				{
					return missingIdError(QStringLiteral("partId"),
						QStringLiteral("Call search_parts or ui_get_state to find one."));
				}

				SQLiteWrapper::SQLite& db = context.database->connection();
				Part part;
				if (!PartRepository::findPart(db, partId, part))
				{
					return unknownPartError(db, partId);
				}

				bool raisedExisting = false;
				if (!context.ui->openPartEditor(partId, &raisedExisting))
				{
					return llmError(QStringLiteral("the editor for part %1 (\"%2\") could not be "
						"opened. Tell the user; there is nothing to retry.")
						.arg(partId).arg(QString::fromStdString(part.name)));
				}

				QJsonObject payload;
				payload["partId"] = partId;
				payload["name"] = jsonText(part.name);
				payload["raisedExisting"] = raisedExisting;
				return llmOk(payload);
			};
			return tool;
		}

#endif // SQLITEWRAPPER_LIBRARY_AVAILABLE
	}

	std::vector<LlmTool> UiToolset::tools(const UiToolContext& context)
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::vector<LlmTool> tools;
		tools.push_back(makeGetState(context));
		tools.push_back(makeSelectCategory(context));
		tools.push_back(makeSelectPart(context));
		tools.push_back(makeSetFilter(context));
		tools.push_back(makeOpenPartEditor(context));
		return tools;
#else
		// No SQLite means no parts and no categories, and every tool here names one (§14c rule 2
		// is what forces that — an id is checked against the database before the window sees it).
		// The toolset still exists so a caller does not have to know which optional dependency was
		// left out; it simply offers nothing.
		PM_UNUSED(context);
		return std::vector<LlmTool>();
#endif
	}

}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE
