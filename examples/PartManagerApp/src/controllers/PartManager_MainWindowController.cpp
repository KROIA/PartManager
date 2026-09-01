#include "controllers/PartManager_MainWindowController.h"

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
					node.children.push_back(buildNode(childId, byId, childrenOf, inStockByType, matchByType, visited));
					node.inStockCount += node.children.back().inStockCount;
					node.matchCount += node.children.back().matchCount;
				}
			}

			std::sort(node.children.begin(), node.children.end(),
				[](const CategoryNode& a, const CategoryNode& b) { return a.name < b.name; });
			return node;
		}
	}

	std::vector<CategoryNode> buildCategoryTree(const std::vector<PartType>& types,
		const std::map<int, int>& inStockByType,
		const std::map<int, int>& matchByType)
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
			roots.push_back(buildNode(rootId, byId, childrenOf, inStockByType, matchByType, visited));
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

		builtIn("stock_qty", QObject::tr("Stock"));
		return columns;
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
			for (const PartType& type : types)
			{
				int count = 0;
				for (const Part& part : PartRepository::listParts(db, type.id))
				{
					if (part.stockQty > 0)
					{
						++count;
					}
				}
				inStockByType[type.id] = count;
			}
			return buildCategoryTree(types, inStockByType, matchByType);
		}
#else
		Q_UNUSED(filterText);
#endif
		return std::vector<CategoryNode>();
	}

	std::vector<PartColumn> MainWindowController::columnsFor(int typeId) const
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_handle && m_handle->isOpen())
		{
			return deriveColumns(PartTypeRepository::effectiveAttributes(m_handle->connection(), typeId));
		}
#endif
		return deriveColumns(std::vector<PartTypeAttribute>());
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

		const bool filtered = !filterText.trimmed().isEmpty();
		SearchQuery query;
		if (filtered)
		{
			query = SearchQuery::parse(filterText.toStdString());
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

				for (const PartColumn& column : columns)
				{
					if (column.isAttribute)
					{
						row.cells.append(formatAttributeValue(toQt(part.attributes), column));
					}
					else if (column.key == "name")
					{
						row.cells.append(toQt(part.name));
					}
					else if (column.key == "manufacturer")
					{
						row.cells.append(toQt(part.manufacturer));
					}
					else if (column.key == "mpn")
					{
						row.cells.append(toQt(part.mpn));
					}
					else if (column.key == "package")
					{
						row.cells.append(toQt(part.package));
					}
					else if (column.key == "stock_qty")
					{
						row.cells.append(QString::number(part.stockQty));
					}
					else
					{
						row.cells.append(QString());
					}
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

}
