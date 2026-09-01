#include "persistence/PartManager_PartlistRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cstdlib>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		Partlist rowToPartlist(const std::vector<std::string>& row)
		{
			Partlist partlist;
			partlist.id = std::atoi(row[0].c_str());
			partlist.name = row[1];
			partlist.description = row[2];
			partlist.projectLinkUrl = row[3];
			partlist.multiplier = std::atoi(row[4].c_str());
			partlist.source = row[5];
			partlist.createdAt = row[6];
			partlist.updatedAt = row[7];
			return partlist;
		}

		PartlistItem rowToItem(const std::vector<std::string>& row)
		{
			PartlistItem item;
			item.id = std::atoi(row[0].c_str());
			item.partlistId = std::atoi(row[1].c_str());
			// A NULL part_id comes back as an empty string, which atoi() reads as 0 — the same
			// value NoPartId already means, so the unresolved state survives the round trip.
			item.partId = std::atoi(row[2].c_str());
			item.designators = row[3];
			item.quantityPerUnit = std::atoi(row[4].c_str());
			item.rawImportData = row[5];
			return item;
		}

		const char* const PartlistColumns =
			"id,name,description,project_link_url,multiplier,source,created_at,updated_at";
		const char* const ItemColumns =
			"id,partlist_id,part_id,designators,quantity_per_unit,raw_import_data";

		// A multiplier of 0 or less would silently make every needed quantity 0, so it is clamped
		// on the way in rather than trusted — §4 calls it a PCB production count.
		int sanitizedMultiplier(int multiplier)
		{
			return multiplier < 1 ? 1 : multiplier;
		}
	}

	bool PartlistRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = db.execute(
			"CREATE TABLE IF NOT EXISTS partlist ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL,"
			"description TEXT,"
			"project_link_url TEXT,"
			"multiplier INTEGER NOT NULL DEFAULT 1,"
			"source TEXT NOT NULL DEFAULT 'manual',"
			"created_at TEXT NOT NULL DEFAULT (datetime('now')),"
			"updated_at TEXT NOT NULL DEFAULT (datetime('now'))"
			");");
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS partlist_item ("
			"id INTEGER PRIMARY KEY,"
			"partlist_id INTEGER NOT NULL REFERENCES partlist(id),"
			"part_id INTEGER REFERENCES part(id),"
			"designators TEXT,"
			"quantity_per_unit INTEGER NOT NULL,"
			"raw_import_data TEXT"
			");") && ok;
		return ok;
	}

	int PartlistRepository::insertPartlist(SQLiteWrapper::SQLite& db, const Partlist& partlist)
	{
		const bool ok = db.executeWithParams(
			"INSERT INTO partlist (name, description, project_link_url, multiplier, source) "
			"VALUES (?, ?, ?, ?, ?);",
			{ partlist.name, partlist.description, partlist.projectLinkUrl,
			  std::to_string(sanitizedMultiplier(partlist.multiplier)), partlist.source });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoPartlistId;
	}

	bool PartlistRepository::updatePartlist(SQLiteWrapper::SQLite& db, const Partlist& partlist)
	{
		// `source` is deliberately not updatable: where a list came from is a fact about its
		// history, not a field.
		return db.executeWithParams(
			"UPDATE partlist SET name=?, description=?, project_link_url=?, multiplier=?, "
			"updated_at=datetime('now') WHERE id=?;",
			{ partlist.name, partlist.description, partlist.projectLinkUrl,
			  std::to_string(sanitizedMultiplier(partlist.multiplier)), std::to_string(partlist.id) });
	}

	bool PartlistRepository::deletePartlist(SQLiteWrapper::SQLite& db, int partlistId)
	{
		// Same reasoning as PartRepository::deletePart(): the REFERENCES clause enforces nothing
		// while PRAGMA foreign_keys stays off, so the items have to go by hand and here, where
		// every delete path passes through.
		const std::string id = std::to_string(partlistId);
		db.executeWithParams("DELETE FROM partlist_item WHERE partlist_id=?;", { id });
		return db.executeWithParams("DELETE FROM partlist WHERE id=?;", { id });
	}

	bool PartlistRepository::findPartlist(SQLiteWrapper::SQLite& db, int partlistId, Partlist& outPartlist)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			std::string("SELECT ") + PartlistColumns + " FROM partlist WHERE id="
			+ std::to_string(partlistId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outPartlist = rowToPartlist(rows.front());
		return true;
	}

	std::vector<Partlist> PartlistRepository::listPartlists(SQLiteWrapper::SQLite& db)
	{
		std::vector<Partlist> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + PartlistColumns + " FROM partlist ORDER BY updated_at DESC, id DESC;"))
		{
			result.push_back(rowToPartlist(row));
		}
		return result;
	}

	int PartlistRepository::itemCount(SQLiteWrapper::SQLite& db, int partlistId)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT COUNT(*) FROM partlist_item WHERE partlist_id=" + std::to_string(partlistId) + ";");
		if (rows.empty() || rows.front().empty())
		{
			return 0;
		}
		return std::atoi(rows.front().front().c_str());
	}

	std::vector<PartlistItem> PartlistRepository::listItems(SQLiteWrapper::SQLite& db, int partlistId)
	{
		std::vector<PartlistItem> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + ItemColumns + " FROM partlist_item WHERE partlist_id="
			+ std::to_string(partlistId) + " ORDER BY id;"))
		{
			result.push_back(rowToItem(row));
		}
		return result;
	}

	std::vector<PartlistLine> PartlistRepository::lines(SQLiteWrapper::SQLite& db, int partlistId)
	{
		std::vector<PartlistLine> result;
		Partlist partlist;
		if (!findPartlist(db, partlistId, partlist))
		{
			return result;
		}
		const int multiplier = sanitizedMultiplier(partlist.multiplier);

		// LEFT JOIN, not JOIN: an unresolved row has no part and still has to appear, orange, in
		// the editor — dropping it would hide exactly the rows the user has to act on.
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT i.id,i.partlist_id,i.part_id,i.designators,i.quantity_per_unit,i.raw_import_data,"
			"p.name,p.mpn,p.stock_qty "
			"FROM partlist_item i LEFT JOIN part p ON p.id=i.part_id "
			"WHERE i.partlist_id=" + std::to_string(partlistId) + " ORDER BY i.id;"))
		{
			PartlistLine line;
			line.item = rowToItem(row);
			line.resolved = line.item.partId != NoPartId && !row[6].empty();
			line.neededQty = line.item.quantityPerUnit * multiplier;
			if (line.resolved)
			{
				line.partName = row[6];
				line.partMpn = row[7];
				line.stockQty = std::atoi(row[8].c_str());
				line.shortfallQty = std::max(0, line.neededQty - line.stockQty);
			}
			result.push_back(line);
		}
		return result;
	}

	bool PartlistRepository::saveItems(SQLiteWrapper::SQLite& db, int partlistId,
		const std::vector<PartlistItem>& items)
	{
		bool ok = db.executeWithParams("DELETE FROM partlist_item WHERE partlist_id=?;",
			{ std::to_string(partlistId) });
		for (const PartlistItem& item : items)
		{
			// part_id has to go in as a real NULL when unresolved, or the LEFT JOIN in lines()
			// would match part id 0 instead of failing to match — hence the two statements.
			if (item.partId == NoPartId)
			{
				ok = db.executeWithParams(
					"INSERT INTO partlist_item (partlist_id, part_id, designators, quantity_per_unit, "
					"raw_import_data) VALUES (?, NULL, ?, ?, ?);",
					{ std::to_string(partlistId), item.designators,
					  std::to_string(item.quantityPerUnit), item.rawImportData }) && ok;
			}
			else
			{
				ok = db.executeWithParams(
					"INSERT INTO partlist_item (partlist_id, part_id, designators, quantity_per_unit, "
					"raw_import_data) VALUES (?, ?, ?, ?, ?);",
					{ std::to_string(partlistId), std::to_string(item.partId), item.designators,
					  std::to_string(item.quantityPerUnit), item.rawImportData }) && ok;
			}
		}
		return ok;
	}

#endif

}
