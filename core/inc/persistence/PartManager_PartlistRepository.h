// @file PartManager_PartlistRepository.h
// @brief CRUD for `partlist` / `partlist_item` (§4) — BOMs and their lines.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartRepository/ListColumnRepository.
//
// Items are **replaced wholesale** by saveItems() rather than updated row by
// row: the editor holds the whole list on screen and a line the user deleted
// has to disappear, which per-row updates would never notice. The same argument
// ListColumnRepository::saveColumns() makes.
//
// `shortfall()` is the one piece of arithmetic here, and it is deliberately in
// persistence rather than in the app: it needs the join between an item's
// needed quantity (`quantity_per_unit * partlist.multiplier`) and the part's
// current `stock_qty`, and item 9's order staging will want exactly the same
// number. An unresolved row (`part_id IS NULL`) has no stock to compare against
// and reports no shortfall — it reports that it is unresolved instead.
// @see docs/design/ARCHITECTURE.md §4
// @see PartManager_Partlist.h, PartManager_PartlistItem.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Partlist.h"
#include "domain/PartManager_PartlistItem.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// One partlist line with everything the editor's table shows, resolved in one query so the
	// grid does not issue a lookup per row.
	struct PART_MANAGER_API PartlistLine
	{
		PartlistItem item;
		std::string partName;       // empty while the row is unresolved
		std::string partMpn;
		int stockQty = 0;           // the part's current stock; 0 for an unresolved row
		int neededQty = 0;          // quantityPerUnit * partlist.multiplier
		int shortfallQty = 0;       // max(0, needed - stock); 0 for an unresolved row
		bool resolved = false;      // false => this line still points at no part (§4)
	};

	class PART_MANAGER_API PartlistRepository
	{
		PartlistRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates partlist/partlist_item if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// partlist CRUD. insertPartlist() returns the new id, NoPartlistId on failure.
		static int insertPartlist(SQLiteWrapper::SQLite& db, const Partlist& partlist);
		static bool updatePartlist(SQLiteWrapper::SQLite& db, const Partlist& partlist);
		// Also removes the list's items — foreign keys are declared but never enforced, so
		// nothing else would. Same reasoning as PartRepository::deletePart().
		static bool deletePartlist(SQLiteWrapper::SQLite& db, int partlistId);
		static bool findPartlist(SQLiteWrapper::SQLite& db, int partlistId, Partlist& outPartlist);
		// Newest first, which is the order the manager screen lists them in.
		static std::vector<Partlist> listPartlists(SQLiteWrapper::SQLite& db);

		// How many lines a list holds — the manager's "Items" column, without loading them.
		static int itemCount(SQLiteWrapper::SQLite& db, int partlistId);

		// The list's raw rows, insertion order.
		static std::vector<PartlistItem> listItems(SQLiteWrapper::SQLite& db, int partlistId);
		// The same rows joined against `part`, with needed/shortfall already worked out.
		// Returns an empty vector for a partlist that does not exist.
		static std::vector<PartlistLine> lines(SQLiteWrapper::SQLite& db, int partlistId);

		// Replaces the list's items with `items` in one shot. `partlistId` is taken from the
		// argument, not from the structs, so a caller cannot half-move rows between lists.
		static bool saveItems(SQLiteWrapper::SQLite& db, int partlistId,
			const std::vector<PartlistItem>& items);
#endif

	};

}
