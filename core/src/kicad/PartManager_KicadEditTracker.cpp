#include "kicad/PartManager_KicadEditTracker.h"
#include "persistence/PartManager_SqlLiteral.h"
#include "PartManager_global.h"

#include <cstdio>
#include <cstdlib>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	std::string KicadEditTracker::hashContent(const std::string& content)
	{
		// FNV-1a 64, same as the filestore's and for the same reason: this is a change detector,
		// not an integrity guarantee. A collision means one hand edit is wrongly overwritten,
		// which is why the user can always re-baseline rather than relying on it.
		unsigned long long hash = 1469598103934665603ULL;
		for (unsigned char c : content)
		{
			hash ^= c;
			hash *= 1099511628211ULL;
		}
		char buffer[17] = { 0 };
		std::snprintf(buffer, sizeof(buffer), "%016llx", hash);
		return buffer;
	}

	KicadItemState KicadEditTracker::stateOf(const std::string& recordedHash,
		const std::string& onDiskContent, bool wasEverGenerated)
	{
		if (!wasEverGenerated)
		{
			return KicadItemState::New;
		}
		if (onDiskContent.empty())
		{
			// We wrote it and it is gone — the user deleted the library, or moved the folder.
			// Writing it again is right; treating it as an "edit" would freeze it out forever.
			return KicadItemState::Missing;
		}
		return hashContent(onDiskContent) == recordedHash
			? KicadItemState::Unchanged
			: KicadItemState::EditedExternally;
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		const char* const ItemColumns =
			"id,part_id,item_type,target_path,last_generated_hash,last_generated_at";

		KicadGeneratedItem rowToItem(const std::vector<std::string>& row)
		{
			KicadGeneratedItem item;
			item.id = std::atoi(row[0].c_str());
			item.partId = std::atoi(row[1].c_str());
			item.itemType = row[2];
			item.targetPath = row[3];
			item.lastGeneratedHash = row[4];
			item.lastGeneratedAt = row[5];
			return item;
		}
	}

	bool KicadEditTracker::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = db.execute(
			"CREATE TABLE IF NOT EXISTS kicad_generated_item ("
			"id INTEGER PRIMARY KEY,"
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"item_type TEXT NOT NULL,"
			"target_path TEXT NOT NULL,"
			"last_generated_hash TEXT NOT NULL,"
			"last_generated_at TEXT NOT NULL DEFAULT (datetime('now'))"
			");");
		// UNIQUE on target_path: one artifact is one row, and record() relies on that to upsert
		// rather than accumulate a baseline per regeneration.
		ok = db.execute("CREATE UNIQUE INDEX IF NOT EXISTS idx_kicad_generated_item_target "
			"ON kicad_generated_item(target_path);") && ok;
		ok = db.execute("CREATE INDEX IF NOT EXISTS idx_kicad_generated_item_part "
			"ON kicad_generated_item(part_id);") && ok;
		return ok;
	}

	bool KicadEditTracker::find(SQLiteWrapper::SQLite& db, const std::string& targetPath,
		KicadGeneratedItem& outItem)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			std::string("SELECT ") + ItemColumns + " FROM kicad_generated_item WHERE target_path="
			+ sqlLiteral(targetPath) + ";");
		if (rows.empty())
		{
			return false;
		}
		outItem = rowToItem(rows.front());
		return true;
	}

	int KicadEditTracker::record(SQLiteWrapper::SQLite& db, int partId, const std::string& itemType,
		const std::string& targetPath, const std::string& content)
	{
		const std::string hash = hashContent(content);

		KicadGeneratedItem existing;
		if (find(db, targetPath, existing))
		{
			return db.executeWithParams(
				"UPDATE kicad_generated_item SET part_id=?, item_type=?, last_generated_hash=?, "
				"last_generated_at=datetime('now') WHERE id=?;",
				{ std::to_string(partId), itemType, hash, std::to_string(existing.id) })
				? existing.id : 0;
		}
		return db.executeWithParams(
			"INSERT INTO kicad_generated_item (part_id, item_type, target_path, last_generated_hash) "
			"VALUES (?, ?, ?, ?);",
			{ std::to_string(partId), itemType, targetPath, hash })
			? static_cast<int>(db.getLastInsertRowId()) : 0;
	}

	bool KicadEditTracker::rebaseline(SQLiteWrapper::SQLite& db, const std::string& targetPath,
		const std::string& onDiskContent)
	{
		KicadGeneratedItem existing;
		if (!find(db, targetPath, existing))
		{
			return false;
		}
		// The file is not touched — only what we consider the baseline. That is the whole
		// difference between "accept my edit" and "throw my edit away".
		return db.executeWithParams(
			"UPDATE kicad_generated_item SET last_generated_hash=?, "
			"last_generated_at=datetime('now') WHERE id=?;",
			{ hashContent(onDiskContent), std::to_string(existing.id) });
	}

	bool KicadEditTracker::forget(SQLiteWrapper::SQLite& db, const std::string& targetPath)
	{
		return db.executeWithParams("DELETE FROM kicad_generated_item WHERE target_path=?;",
			{ targetPath });
	}

	KicadItemState KicadEditTracker::stateOf(SQLiteWrapper::SQLite& db, const std::string& targetPath,
		const std::string& onDiskContent)
	{
		KicadGeneratedItem item;
		const bool known = find(db, targetPath, item);
		return stateOf(item.lastGeneratedHash, onDiskContent, known);
	}

	std::vector<KicadGeneratedItem> KicadEditTracker::itemsForPart(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<KicadGeneratedItem> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + ItemColumns + " FROM kicad_generated_item WHERE part_id="
			+ std::to_string(partId) + " ORDER BY id;"))
		{
			result.push_back(rowToItem(row));
		}
		return result;
	}

	std::vector<KicadGeneratedItem> KicadEditTracker::allItems(SQLiteWrapper::SQLite& db)
	{
		std::vector<KicadGeneratedItem> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + ItemColumns + " FROM kicad_generated_item ORDER BY target_path;"))
		{
			result.push_back(rowToItem(row));
		}
		return result;
	}

#endif

}
