// @file PartManager_Transaction.h
// @brief RAII `BEGIN IMMEDIATE` / `COMMIT` / `ROLLBACK` guard over one SQLiteWrapper connection.
//
// **This is new to the codebase.** Until now nothing here ran inside an explicit
// transaction — PartRepository::deletePart() says so in as many words, and lives
// with it because the worst a half-finished delete leaves behind is a part row
// with fewer children that the next delete finishes off.
//
// The category merge (PartManager_PartTypeTransfer.h) cannot live with it. One
// merged category writes five tables — `part_type`, `part_type_attribute`,
// `part_type_file_slot`, `part_type_list_column` and `tag`/`part_type_tag` — and
// a re-parent rewrites a `parent_type_id` on top of that. A failure halfway
// through does not leave "a bit less than was asked for": it leaves a category
// tree whose attributes belong to one database and whose parents belong to
// another, which is corruption nothing later can untangle. The whole apply is
// therefore one transaction, and a partial apply is impossible by construction.
//
// IMMEDIATE rather than plain BEGIN: the write lock is taken up front, so the
// merge fails at the start when another connection holds the database rather
// than halfway through when the first write finally upgrades the lock.
//
// Failure is reported, never thrown — `active()` is false when BEGIN did not
// take (most commonly because a transaction is already open on this connection,
// SQLite allowing no nesting). An inactive guard then does nothing at all on
// scope exit: it did not open the outer transaction and must not close it.
// The destructor never throws, because it runs while an error is already being
// handled and a throw from there would terminate the process.
// @see docs/design/ARCHITECTURE.md §1c, §9a
// @see PartManager_PartTypeTransfer.h, PartManager_DatabaseHandle.h
#pragma once

#include "PartManager_global.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// Scope guard around one SQLite transaction: commit on request, roll back otherwise.
	class PART_MANAGER_API Transaction
	{
	public:
		// Issues BEGIN IMMEDIATE. Check active() — a false there means no transaction was
		// started and this guard will neither commit nor roll anything back.
		explicit Transaction(SQLiteWrapper::SQLite& db);
		// Rolls back unless commit() already ran. Never throws.
		~Transaction();

		// Non-copyable and non-movable: two objects owning one transaction would mean two
		// ROLLBACKs, the second of them against whatever transaction happened to be open next.
		Transaction(const Transaction&) = delete;
		Transaction& operator=(const Transaction&) = delete;
		Transaction(Transaction&&) = delete;
		Transaction& operator=(Transaction&&) = delete;

		// True while this guard owns an open transaction — false when BEGIN failed, and false
		// again once commit() or rollback() has run.
		bool active() const;

		// COMMITs. Returns false if there was nothing to commit or the COMMIT itself failed;
		// in the latter case the guard stays active so the destructor still rolls back.
		bool commit();

		// Rolls back early, before scope exit. The destructor calls this itself, so it is only
		// needed when the caller wants the rollback to happen at a specific point.
		bool rollback();

	private:
		SQLiteWrapper::SQLite& m_db;
		bool m_active = false;
	};

#endif

}
