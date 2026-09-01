#include "persistence/PartManager_ListColumnRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <cstdlib>
#include <unordered_set>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		PartTypeListColumn rowToColumn(const std::vector<std::string>& row)
		{
			PartTypeListColumn column;
			column.id = std::atoi(row[0].c_str());
			column.partTypeId = std::atoi(row[1].c_str());
			column.columnKey = row[2];
			column.labelOverride = row[3];
			column.visible = std::atoi(row[4].c_str()) != 0;
			column.sortOrder = std::atoi(row[5].c_str());
			column.widthPx = std::atoi(row[6].c_str());
			return column;
		}
	}

	bool ListColumnRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		return db.execute(
			"CREATE TABLE IF NOT EXISTS part_type_list_column ("
			"id INTEGER PRIMARY KEY,"
			"part_type_id INTEGER NOT NULL REFERENCES part_type(id),"
			"column_key TEXT NOT NULL,"
			"label_override TEXT,"
			"visible INTEGER NOT NULL DEFAULT 1,"
			"sort_order INTEGER NOT NULL DEFAULT 0,"
			"width_px INTEGER,"
			"UNIQUE(part_type_id, column_key)"
			");");
	}

	std::vector<PartTypeListColumn> ListColumnRepository::listOwnColumns(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeListColumn> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_type_id,column_key,label_override,visible,sort_order,width_px "
			"FROM part_type_list_column WHERE part_type_id=" + std::to_string(typeId) + " ORDER BY sort_order,id;"))
		{
			result.push_back(rowToColumn(row));
		}
		return result;
	}

	std::vector<PartTypeListColumn> ListColumnRepository::effectiveColumns(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeListColumn> accumulated;
		for (int level : PartTypeRepository::ancestorChainRootFirst(db, typeId))
		{
			std::vector<PartTypeListColumn> ownRows = listOwnColumns(db, level);
			if (ownRows.empty())
			{
				continue;   // this level was never customized; keep whatever the ancestors said
			}
			std::unordered_set<std::string> overriddenKeys;
			for (const PartTypeListColumn& row : ownRows)
			{
				overriddenKeys.insert(row.columnKey);
			}
			std::vector<PartTypeListColumn> merged;
			for (const PartTypeListColumn& existing : accumulated)
			{
				if (overriddenKeys.find(existing.columnKey) == overriddenKeys.end())
				{
					merged.push_back(existing);
				}
			}
			for (const PartTypeListColumn& row : ownRows)
			{
				merged.push_back(row);
			}
			accumulated = std::move(merged);
		}
		return accumulated;
	}

	bool ListColumnRepository::saveColumns(SQLiteWrapper::SQLite& db, int typeId,
		const std::vector<PartTypeListColumn>& columns)
	{
		// Delete-then-insert rather than per-row upserts: the layout is one value, and a key the
		// user dropped has to disappear, which a row-by-row update would never notice.
		bool ok = clearColumns(db, typeId);
		int sortOrder = 0;
		for (const PartTypeListColumn& column : columns)
		{
			ok = db.executeWithParams(
				"INSERT INTO part_type_list_column (part_type_id, column_key, label_override, visible, "
				"sort_order, width_px) VALUES (?, ?, ?, ?, ?, ?);",
				{ std::to_string(typeId), column.columnKey, column.labelOverride,
				  column.visible ? "1" : "0", std::to_string(sortOrder), std::to_string(column.widthPx) }) && ok;
			++sortOrder;
		}
		return ok;
	}

	bool ListColumnRepository::clearColumns(SQLiteWrapper::SQLite& db, int typeId)
	{
		return db.executeWithParams("DELETE FROM part_type_list_column WHERE part_type_id=?;",
			{ std::to_string(typeId) });
	}

#endif

}
