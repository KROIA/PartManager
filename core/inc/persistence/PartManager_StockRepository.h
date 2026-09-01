// @file PartManager_StockRepository.h
// @brief The `stock_transaction` log (§3) + the `part.stock_qty` cache derived from it.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartRepository/TagRepository. The log is the source
// of truth; `part.stock_qty` is a cache of SUM(delta_qty) that every write here
// refreshes, so existing readers (Home table, "check stock", PartRepository)
// keep reading the plain column and need no change.
//
// **Negative stock is allowed, deliberately.** takeOut() never rejects a
// quantity larger than what is on the shelf: the log records what actually
// happened, and refusing the entry would only leave the DB *more* wrong than
// the physical world already is (someone took parts without logging them).
// A negative quantity is a visible "your book-keeping is off, recount me"
// signal; the fix is a correction, not a blocked entry. Callers that want to
// warn first read currentQuantity() before writing.
//
// backfillOpeningBalances() turns a standing `part.stock_qty` with no log
// behind it (CSV import wrote the column directly) into one `initial` row per
// part. It is idempotent by construction — "has no transactions at all" is both
// the selection rule and the guard, so a second run selects nothing.
// @see docs/design/ARCHITECTURE.md §3
// @see PartManager_StockTransaction.h, PartManager_PartRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_StockTransaction.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API StockRepository
	{
		StockRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates stock_transaction if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// Appends one transaction and refreshes part.stock_qty. Returns the new id
		// (NoStockTransactionId on failure). A zero deltaQty is rejected — it is never a real event.
		static int recordTransaction(SQLiteWrapper::SQLite& db, const StockTransaction& transaction);

		// +quantity, reason 'restock'. quantity must be > 0.
		static int restock(SQLiteWrapper::SQLite& db, int partId, int quantity, const std::string& note = std::string());
		// -quantity. quantity must be > 0; may drive stock negative (see header note). `reason` takes a
		// StockReason constant — a manual take-out is usually a build, but 'loss' is just as real, and
		// logging a broken part as a checkout makes the history lie. Empty keeps the partlist default.
		static int takeOut(SQLiteWrapper::SQLite& db, int partId, int quantity,
			const std::string& note = std::string(),
			const std::string& reason = std::string());
		// "I counted the shelf, it is actually newQuantity": writes the delta as 'manual_adjust'.
		// Returns NoStockTransactionId and writes nothing when the count already matches.
		static int correct(SQLiteWrapper::SQLite& db, int partId, int newQuantity, const std::string& note = std::string());

		// A part's transactions, oldest first (insertion order).
		static std::vector<StockTransaction> history(SQLiteWrapper::SQLite& db, int partId);
		// SUM(delta_qty) straight from the log — the authoritative quantity, not the cached column.
		static int currentQuantity(SQLiteWrapper::SQLite& db, int partId);
		// Rewrites part.stock_qty from the log. Only needed after a hand-edited DB or a bulk import.
		static bool refreshCachedQuantity(SQLiteWrapper::SQLite& db, int partId);

		// §3 opening balance: one 'initial' transaction per part that has a non-zero stock_qty and no
		// transactions behind it. Returns how many parts were backfilled (0 on a second run).
		static int backfillOpeningBalances(SQLiteWrapper::SQLite& db);
#endif

	};

}
