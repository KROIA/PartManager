// @file PartManager_ListColumnRepository.h
// @brief CRUD for `part_type_list_column` (§7b) — the saved Home-table layout per category.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartTypeRepository/StockRepository.
//
// **Nothing is seeded.** A type with no rows here is the normal, untouched
// state and the caller falls back to deriving columns from the type's effective
// attributes, which is exactly what the Home table did before this table
// existed — so every database that predates it keeps rendering unchanged.
// Rows appear the first time the user reorders, hides or resizes a column, and
// `clearColumns()` ("Reset to default") removes them again.
//
// `effectiveColumns()` resolves inheritance the same way attributes and file
// slots do (§2b): walk root ancestor -> typeId, a row on a more specific type
// overrides an ancestor's row of the same `column_key`. A subtype that has
// never been customized therefore inherits its parent's layout, and any
// attribute the subtype adds on top is simply not mentioned in that layout —
// the caller appends those, it does not drop them.
// @see docs/design/ARCHITECTURE.md §7b, §2b
// @see PartManager_PartTypeListColumn.h, PartManager_PartTypeRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_PartTypeListColumn.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API ListColumnRepository
	{
		ListColumnRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates part_type_list_column if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// This type's own rows only, in sort_order — not inherited (see effectiveColumns()).
		static std::vector<PartTypeListColumn> listOwnColumns(SQLiteWrapper::SQLite& db, int typeId);

		// §2b resolution over the ancestor chain. Empty means "never customized anywhere up the
		// chain" — the caller derives columns from the effective attributes instead.
		static std::vector<PartTypeListColumn> effectiveColumns(SQLiteWrapper::SQLite& db, int typeId);

		// Replaces this type's own rows with `columns` in one shot — the whole layout is written
		// together, so a partial row set can never leave a half-ordered table behind. `sortOrder`
		// is taken from the vector's order, `partTypeId` from the argument.
		static bool saveColumns(SQLiteWrapper::SQLite& db, int typeId,
			const std::vector<PartTypeListColumn>& columns);

		// "Reset to default": drops this type's own rows. The type then falls back to its
		// ancestor's layout, or to the derived columns when there is none.
		static bool clearColumns(SQLiteWrapper::SQLite& db, int typeId);
#endif

	};

}
