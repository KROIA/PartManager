#include "controllers/PartManager_MainWindowController.h"

#include "controllers/PartManager_PartEditorController.h"
#include "controllers/PartManager_StockController.h"
#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_ListColumnRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include "search/PartManager_SearchEngine.h"
#include "units/PartManager_ValueParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <algorithm>
#include <set>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// Recursively builds one node and everything under it, summing in-stock counts upwards.
		CategoryNode buildNode(int typeId,
			const std::map<int, const PartType*>& byId,
			const std::map<int, std::vector<int>>& childrenOf,
			const std::map<int, int>& inStockByType,
			const std::map<int, int>& matchByType,
			const std::map<int, int>& partCountByType,
			std::set<int>& visited)
		{
			CategoryNode node;
			node.typeId = typeId;
			auto typeIt = byId.find(typeId);
			if (typeIt != byId.end())
			{
				node.name = toQt(typeIt->second->name);
			}

			auto ownIt = inStockByType.find(typeId);
			node.inStockCount = ownIt != inStockByType.end() ? ownIt->second : 0;
			auto matchIt = matchByType.find(typeId);
			node.matchCount = matchIt != matchByType.end() ? matchIt->second : 0;
			auto countIt = partCountByType.find(typeId);
			node.partCount = countIt != partCountByType.end() ? countIt->second : 0;

			auto childIt = childrenOf.find(typeId);
			if (childIt != childrenOf.end())
			{
				for (int childId : childIt->second)
				{
					// A parent_type_id cycle would otherwise recurse forever — a database
					// edited by hand can contain one, so drop the repeat instead of trusting it.
					if (!visited.insert(childId).second)
					{
						continue;
					}
					node.children.push_back(buildNode(childId, byId, childrenOf, inStockByType,
						matchByType, partCountByType, visited));
					node.inStockCount += node.children.back().inStockCount;
					node.matchCount += node.children.back().matchCount;
					node.partCount += node.children.back().partCount;
				}
			}

			std::sort(node.children.begin(), node.children.end(),
				[](const CategoryNode& a, const CategoryNode& b) { return a.name < b.name; });
			return node;
		}
	}

	std::vector<CategoryNode> buildCategoryTree(const std::vector<PartType>& types,
		const std::map<int, int>& inStockByType,
		const std::map<int, int>& matchByType,
		const std::map<int, int>& partCountByType)
	{
		std::map<int, const PartType*> byId;
		for (const PartType& type : types)
		{
			byId[type.id] = &type;
		}

		std::map<int, std::vector<int>> childrenOf;
		std::vector<int> rootIds;
		for (const PartType& type : types)
		{
			// A parent that isn't in the list at all can't be nested under anything, so it becomes a root.
			if (type.parentTypeId != NoParentType && byId.count(type.parentTypeId) != 0)
			{
				childrenOf[type.parentTypeId].push_back(type.id);
			}
			else
			{
				rootIds.push_back(type.id);
			}
		}

		std::set<int> visited(rootIds.begin(), rootIds.end());
		std::vector<CategoryNode> roots;
		for (int rootId : rootIds)
		{
			roots.push_back(buildNode(rootId, byId, childrenOf, inStockByType, matchByType,
				partCountByType, visited));
		}

		std::sort(roots.begin(), roots.end(),
			[](const CategoryNode& a, const CategoryNode& b) { return a.name < b.name; });
		return roots;
	}

	std::vector<int> typeIdWithDescendants(const std::vector<PartType>& types, int typeId)
	{
		std::vector<int> result;
		if (typeId == NoParentType)
		{
			return result;
		}
		result.push_back(typeId);

		// Breadth-first over the flat rows; `result` doubles as the queue and as the cycle guard.
		for (size_t i = 0; i < result.size(); ++i)
		{
			for (const PartType& type : types)
			{
				if (type.parentTypeId != result[i])
				{
					continue;
				}
				if (std::find(result.begin(), result.end(), type.id) == result.end())
				{
					result.push_back(type.id);
				}
			}
		}
		return result;
	}

	std::vector<PartColumn> deriveColumns(const std::vector<PartTypeAttribute>& effectiveAttributes)
	{
		std::vector<PartColumn> columns;

		auto builtIn = [&columns](const char* key, const QString& label)
		{
			PartColumn column;
			column.key = QString::fromLatin1(key);
			column.label = label;
			columns.push_back(column);
		};

		builtIn("name", QObject::tr("Name"));
		builtIn("manufacturer", QObject::tr("Manufacturer"));
		builtIn("mpn", QObject::tr("MPN"));
		builtIn("package", QObject::tr("Package"));

		for (const PartTypeAttribute& attribute : effectiveAttributes)
		{
			PartColumn column;
			column.key = toQt(attribute.key);
			column.label = toQt(attribute.label); // user-defined label — never tr()'d
			column.isAttribute = true;
			column.unit = toQt(attribute.unit);
			column.datatype = attribute.datatype;
			columns.push_back(column);
		}

		// Glyphs, not text — see MainWindow's Files column. Last but one so it sits beside Stock,
		// which is the other "state of this part" column rather than an identity one.
		builtIn("files", QObject::tr("Files"));
		builtIn("stock_qty", QObject::tr("Stock"));
		return columns;
	}

	std::vector<PartColumn> applyColumnConfig(const std::vector<PartColumn>& derived,
		const std::vector<PartTypeListColumn>& config)
	{
		if (config.empty())
		{
			return derived;   // never customized (or a database older than §7b) — derived order stands
		}

		std::vector<PartColumn> result;
		std::set<QString> placed;
		for (const PartTypeListColumn& saved : config)
		{
			const QString key = toQt(saved.columnKey);
			auto match = std::find_if(derived.begin(), derived.end(),
				[&key](const PartColumn& column) { return column.key == key; });
			if (match == derived.end() || !placed.insert(key).second)
			{
				continue; // an attribute that was deleted since, or a duplicated key in a hand-edited DB
			}
			PartColumn column = *match;
			column.visible = saved.visible;
			column.widthPx = saved.widthPx;
			column.labelOverride = toQt(saved.labelOverride);
			if (!column.labelOverride.isEmpty())
			{
				column.label = column.labelOverride; // user-typed text — never tr()'d
			}
			result.push_back(column);
		}
		// Anything the type gained after the layout was saved: appended, visible. Dropping it
		// instead would make a newly added attribute invisible with no hint that it exists.
		for (const PartColumn& column : derived)
		{
			if (placed.find(column.key) == placed.end())
			{
				result.push_back(column);
			}
		}

		// The table hangs the part id and the tag chips off column 0 (see header note).
		auto name = std::find_if(result.begin(), result.end(),
			[](const PartColumn& column) { return column.key == "name"; });
		if (name != result.end())
		{
			name->visible = true;
			std::rotate(result.begin(), name, name + 1);
		}
		return result;
	}

	std::vector<PartTypeListColumn> toColumnConfig(const std::vector<PartColumn>& columns)
	{
		std::vector<PartTypeListColumn> config;
		int sortOrder = 0;
		for (const PartColumn& column : columns)
		{
			PartTypeListColumn row;
			row.columnKey = column.key.toStdString();
			row.labelOverride = column.labelOverride.toStdString();
			row.visible = column.visible;
			row.sortOrder = sortOrder++;
			row.widthPx = column.widthPx;
			config.push_back(row);
		}
		return config;
	}

	QString formatAttributeValue(const QString& attributesJson, const PartColumn& column)
	{
		QJsonObject root = QJsonDocument::fromJson(attributesJson.toUtf8()).object();
		QJsonValue stored = root.value(column.key);
		if (stored.isUndefined() || stored.isNull())
		{
			return QString();
		}

		// §2a stores a dimensioned attribute as {value, unit}; everything else is a bare JSON value.
		QString unit = column.unit;
		if (stored.isObject())
		{
			QJsonObject object = stored.toObject();
			if (object.contains("unit"))
			{
				unit = object.value("unit").toString();
			}
			stored = object.value("value");
		}

		if (stored.isBool())
		{
			return stored.toBool() ? QObject::tr("Yes") : QObject::tr("No");
		}
		if (stored.isString())
		{
			return stored.toString(); // user data
		}
		if (!stored.isDouble())
		{
			return QString();
		}

		double value = stored.toDouble();
		if (column.datatype == AttributeDataType::Dimension)
		{
			return toQt(ValueParser::format(value, unit.toStdString()));
		}
		return QString::number(value, 'g', 10);
	}

	QString formatCell(const Part& part, const PartColumn& column)
	{
		if (column.isAttribute)
		{
			return formatAttributeValue(toQt(part.attributes), column);
		}
		if (column.key == "name")
		{
			return toQt(part.name);
		}
		if (column.key == "manufacturer")
		{
			return toQt(part.manufacturer);
		}
		if (column.key == "mpn")
		{
			return toQt(part.mpn);
		}
		if (column.key == "package")
		{
			return toQt(part.package);
		}
		if (column.key == "stock_qty")
		{
			return QString::number(part.stockQty);
		}
		// "files" is painted from PartRow::attachments, not written as text — a cell with both a
		// glyph strip and a caption in it would be unreadable at row height.
		return QString();
	}

	QString datasheetState(const QString& fileName, bool onDisk)
	{
		if (fileName.isEmpty())
		{
			return QObject::tr("None");
		}
		// A part_file row whose file is gone has to say so — otherwise the panel advertises a
		// datasheet that the editor's Open button then silently fails to open.
		return onDisk ? fileName : QObject::tr("%1 (file missing)").arg(fileName);
	}

	PartPreview buildPreview(const Part& part, const std::vector<PartColumn>& columns,
		const std::vector<Tag>& tags, const QString& datasheet)
	{
		PartPreview preview;
		preview.partId = part.id;
		preview.name = toQt(part.name);
		preview.description = toQt(part.description);
		preview.tags = tags;

		for (const PartColumn& column : columns)
		{
			if (column.key == "name")
			{
				continue; // already the panel's title
			}
			// An empty manufacturer or an attribute this part never filled in would be a blank
			// line in a narrow panel; the table keeps the column, the preview just omits the line.
			const QString value = formatCell(part, column);
			if (!value.isEmpty())
			{
				preview.fields.push_back({ column.label, value });
			}
		}
		preview.fields.push_back({ QObject::tr("Datasheet"), datasheet });
		return preview;
	}

	QString searchError(const QString& filterText)
	{
		SearchQuery query = SearchQuery::parse(filterText.toStdString());
		// The parser's message is developer-written English, not user data, but it is not a
		// tr() literal either — it comes out of core. Shown as-is.
		return query.ok ? QString() : toQt(query.error);
	}

	MainWindowController::MainWindowController(std::unique_ptr<DatabaseHandle> handle)
		: m_handle(std::move(handle))
	{
	}

	DatabaseHandle* MainWindowController::handle() const
	{
		return m_handle.get();
	}

	QString MainWindowController::databaseName() const
	{
		return QFileInfo(pmdbPath()).absoluteDir().dirName();
	}

	QString MainWindowController::pmdbPath() const
	{
		return m_handle ? QString::fromStdString(m_handle->pmdbPath()) : QString();
	}

	std::vector<CategoryNode> MainWindowController::categoryTree(const QString& filterText) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			SQLiteWrapper::SQLite& db = m_handle->connection();
			std::vector<PartType> types = PartTypeRepository::listTypes(db);

			// §7a: the tree filter only counts, it never hides a category. A query that fails to
			// parse counts zero everywhere, same as one that simply matches nothing.
			std::map<int, int> matchByType;
			if (!filterText.trimmed().isEmpty())
			{
				SearchQuery query = SearchQuery::parse(filterText.toStdString());
				for (const PartType& type : types)
				{
					matchByType[type.id] = static_cast<int>(SearchEngine::searchIds(db, query, type.id).size());
				}
			}

			// One listParts() per type rather than a GROUP BY, so the count comes from the
			// same repository the table reads — a hand-written aggregate here would be a
			// second definition of "in stock" to keep in sync.
			// ponytail: O(types) queries, each loading full rows. Ceiling is a few hundred
			// types; upgrade path is a dedicated PartRepository::countInStockByType().
			std::map<int, int> inStockByType;
			std::map<int, int> partCountByType;
			for (const PartType& type : types)
			{
				int count = 0;
				int total = 0;
				for (const Part& part : PartRepository::listParts(db, type.id))
				{
					++total;
					if (part.stockQty > 0)
					{
						++count;
					}
				}
				inStockByType[type.id] = count;
				partCountByType[type.id] = total;
			}
			return buildCategoryTree(types, inStockByType, matchByType, partCountByType);
		}
#else
		Q_UNUSED(filterText);
#endif
		return std::vector<CategoryNode>();
	}

	std::vector<PartColumn> MainWindowController::allColumnsFor(int typeId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			SQLiteWrapper::SQLite& db = m_handle->connection();
			return applyColumnConfig(deriveColumns(PartTypeRepository::effectiveAttributes(db, typeId)),
				ListColumnRepository::effectiveColumns(db, typeId));
		}
#endif
		return deriveColumns(std::vector<PartTypeAttribute>());
	}

	std::vector<PartColumn> MainWindowController::columnsFor(int typeId) const
	{
		std::vector<PartColumn> visible;
		for (const PartColumn& column : allColumnsFor(typeId))
		{
			if (column.visible)
			{
				visible.push_back(column);
			}
		}
		return visible;
	}

	bool MainWindowController::saveColumns(int typeId, const std::vector<PartColumn>& columns) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen() && typeId != NoParentType)
		{
			return ListColumnRepository::saveColumns(m_handle->connection(), typeId, toColumnConfig(columns));
		}
#else
		Q_UNUSED(typeId);
		Q_UNUSED(columns);
#endif
		return false;
	}

	bool MainWindowController::resetColumns(int typeId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen() && typeId != NoParentType)
		{
			return ListColumnRepository::clearColumns(m_handle->connection(), typeId);
		}
#else
		Q_UNUSED(typeId);
#endif
		return false;
	}

	bool MainWindowController::saveColumnWidth(int typeId, const QString& columnKey, int widthPx) const
	{
		// Written through the whole layout rather than a single-column upsert: a lone width row
		// would be the only entry in the config, and applyColumnConfig() would then order the
		// table by it — one dragged divider would jumble every other column.
		std::vector<PartColumn> columns = allColumnsFor(typeId);
		bool found = false;
		for (PartColumn& column : columns)
		{
			if (column.key == columnKey)
			{
				column.widthPx = widthPx;
				found = true;
			}
		}
		return found && saveColumns(typeId, columns);
	}

	std::vector<PartRow> MainWindowController::partsFor(int typeId, const std::vector<PartColumn>& columns,
		const QString& filterText) const
	{
		std::vector<PartRow> rows;
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (!m_handle || !m_handle->isOpen())
		{
			return rows;
		}
		SQLiteWrapper::SQLite& db = m_handle->connection();

		// One query for every part's thumbnail rather than one per visible row — a category with
		// a few hundred parts would otherwise pay a round trip per row on every keystroke of the
		// table filter.
		std::map<int, QString> imageByPart;
		{
			FileStore store(m_handle->filestorePath());
			for (const PartFile& file : PartRepository::listFilesWithRole(db, PartFileRole::Image))
			{
				// Newest wins, same rule as PartEditorController::roleFile().
				imageByPart[file.partId] = toQt(store.absolutePath(file.relativePath));
			}
		}

		// Same one-query-per-role trick as the thumbnails above: four queries for the whole table
		// rather than four per row. Only the presence of a row matters here, not the file behind
		// it, so unlike the image path this does not have to touch the disk at all.
		std::map<int, int> attachmentsByPart;
		{
			// Not `slots`: Qt #defines that as a keyword, and the error it produces names the
			// array rather than the macro.
			struct RoleFlag { PartFileRole role; int flag; };
			const RoleFlag roleSlots[] = {
				{ PartFileRole::Datasheet, AttachmentDatasheet },
				{ PartFileRole::KicadSymbol, AttachmentKicadSymbol },
				{ PartFileRole::KicadFootprint, AttachmentKicadFootprint },
				{ PartFileRole::Kicad3DModel, Attachment3DModel },
			};
			for (const RoleFlag& slot : roleSlots)
			{
				for (const PartFile& file : PartRepository::listFilesWithRole(db, slot.role))
				{
					attachmentsByPart[file.partId] |= slot.flag;
				}
			}
		}

		const bool filtered = !filterText.trimmed().isEmpty();
		SearchQuery query;
		if (filtered)
		{
			query = SearchQuery::parse(filterText.toStdString());
		}

		// Type names for the placeholder icon a part with no photo gets. Looked up once for the
		// whole table rather than per row, which would be a query per part.
		std::map<int, QString> typeNameById;
		for (const PartType& type : PartTypeRepository::listTypes(db))
		{
			typeNameById[type.id] = toQt(type.name);
		}

		for (int id : typeIdWithDescendants(PartTypeRepository::listTypes(db), typeId))
		{
			// The table filter is scoped to the selected category, but that category includes
			// its descendants — so the id set is collected per descendant type, not once.
			std::set<int> matched;
			if (filtered)
			{
				std::vector<int> ids = SearchEngine::searchIds(db, query, id);
				matched.insert(ids.begin(), ids.end());
			}

			for (const Part& part : PartRepository::listParts(db, id))
			{
				if (filtered && matched.count(part.id) == 0)
				{
					continue;
				}
				PartRow row;
				row.partId = part.id;
				row.stockQty = part.stockQty;
				row.stockMinQty = part.stockMinQty;
				row.tags = TagRepository::listPartTags(db, part.id);
				row.imagePath = imageByPart.count(part.id) ? imageByPart[part.id] : QString();
				row.typeName = typeNameById.count(part.partTypeId)
					? typeNameById[part.partTypeId] : QString();
				row.attachments = attachmentsByPart.count(part.id)
					? attachmentsByPart[part.id] : 0;

				for (const PartColumn& column : columns)
				{
					row.cells.append(formatCell(part, column));
				}
				rows.push_back(row);
			}
		}
#else
		Q_UNUSED(typeId);
		Q_UNUSED(columns);
		Q_UNUSED(filterText);
#endif
		return rows;
	}

	PartPreview MainWindowController::previewFor(int partId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen() && partId != 0)
		{
			// The editor and stock controllers are non-owning wrappers over this same handle, so
			// the panel reads the part, its datasheet and its quantity through the paths that
			// already exist rather than growing a second set of queries here.
			PartEditorController editor(m_handle.get());
			Part part;
			if (editor.loadPart(partId, part))
			{
				// part.stock_qty is the cached column; the log's sum is the authoritative one (§3).
				part.stockQty = StockController(m_handle.get()).quantity(partId);

				PartFile file;
				const QString fileName = editor.datasheetFile(part, file)
					? toQt(file.originalFilename)
					: QString();

				PartPreview preview = buildPreview(part, columnsFor(part.partTypeId),
					TagRepository::listPartTags(m_handle->connection(), partId),
					datasheetState(fileName, !editor.datasheetPath(part).empty()));
				preview.imagePath = toQt(editor.roleFilePath(partId, PartFileRole::Image));
				preview.kicadSymbolPath =
					toQt(editor.roleFilePath(partId, PartFileRole::KicadSymbol));
				preview.kicadFootprintPath =
					toQt(editor.roleFilePath(partId, PartFileRole::KicadFootprint));
				preview.model3DPath =
					toQt(editor.roleFilePath(partId, PartFileRole::Kicad3DModel));
				for (const PartType& type : editor.types())
				{
					if (type.id == part.partTypeId) { preview.typeName = toQt(type.name); break; }
				}
				return preview;
			}
		}
#else
		Q_UNUSED(partId);
#endif
		return PartPreview();
	}

}
