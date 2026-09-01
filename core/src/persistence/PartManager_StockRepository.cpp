#include "persistence/PartManager_StockRepository.h"
#include "PartManager_global.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		StockTransaction rowToTransaction(const std::vector<std::string>& row)
		{
			StockTransaction transaction;
			transaction.id = std::atoi(row[0].c_str());
			transaction.partId = std::atoi(row[1].c_str());
			transaction.deltaQty = std::atoi(row[2].c_str());
			transaction.reason = row[3];
			transaction.refPartlistId = std::atoi(row[4].c_str());
			transaction.refOrderId = std::atoi(row[5].c_str());
			transaction.unitCost = std::atof(row[6].c_str());
			transaction.currency = row[7];
			transaction.note = row[8];
			transaction.createdAt = row[9];
			return transaction;
		}

		// Same reason as PartRepository's copy: std::to_string(double) is fixed 6-decimal notation and
		// would round a sub-cent unit cost to 0.000000. 17 significant digits round-trips any IEEE double.
		std::string toSqlNumber(double value)
		{
			char buffer[64] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%.17g", value);
			return buffer;
		}

		// First cell of a single-value query, or fallback when the query returned nothing.
		std::string scalar(SQLiteWrapper::SQLite& db, const std::string& query, const char* fallback)
		{
			std::vector<std::vector<std::string>> rows = db.fetchAll(query);
			if (rows.empty() || rows.front().empty() || rows.front().front().empty())
			{
				return fallback;
			}
			return rows.front().front();
		}
	}

	bool StockRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		// ponytail: ref_partlist_id/ref_order_id are plain INTEGERs, not REFERENCES partlist(id)/
		// mouser_order(id) as §3 writes them — those tables do not exist yet (§4 is a later backlog
		// item). Harmless today (this codebase never sets PRAGMA foreign_keys=ON) but it would become
		// a real "no such table" the moment anyone does. Add the REFERENCES clauses in the migration
		// step that creates partlist/mouser_order.
		return db.execute(
			"CREATE TABLE IF NOT EXISTS stock_transaction ("
			"id INTEGER PRIMARY KEY,"
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"delta_qty INTEGER NOT NULL,"
			"reason TEXT NOT NULL,"
			"ref_partlist_id INTEGER,"
			"ref_order_id INTEGER,"
			"unit_cost REAL,"
			"currency TEXT,"
			"note TEXT,"
			"created_at TEXT NOT NULL DEFAULT (datetime('now'))"
			");")
			&& db.execute(
			"CREATE INDEX IF NOT EXISTS idx_stock_transaction_part ON stock_transaction(part_id);");
	}

	int StockRepository::recordTransaction(SQLiteWrapper::SQLite& db, const StockTransaction& transaction)
	{
		if (transaction.partId == 0 || transaction.deltaQty == 0)
		{
			return NoStockTransactionId;
		}
		bool ok = db.executeWithParams(
			"INSERT INTO stock_transaction (part_id, delta_qty, reason, ref_partlist_id, ref_order_id, "
			"unit_cost, currency, note) VALUES (?, ?, ?, ?, ?, ?, ?, ?);",
			{ std::to_string(transaction.partId), std::to_string(transaction.deltaQty), transaction.reason,
			  std::to_string(transaction.refPartlistId), std::to_string(transaction.refOrderId),
			  toSqlNumber(transaction.unitCost), transaction.currency, transaction.note });
		if (!ok)
		{
			return NoStockTransactionId;
		}
		int newId = static_cast<int>(db.getLastInsertRowId());
		// The cache is refreshed here, not at the call site, so no caller can forget it and leave
		// part.stock_qty disagreeing with the log.
		refreshCachedQuantity(db, transaction.partId);
		return newId;
	}

	int StockRepository::restock(SQLiteWrapper::SQLite& db, int partId, int quantity, const std::string& note)
	{
		if (quantity <= 0)
		{
			return NoStockTransactionId;
		}
		StockTransaction transaction;
		transaction.partId = partId;
		transaction.deltaQty = quantity;
		transaction.reason = StockReason::Restock;
		transaction.note = note;
		return recordTransaction(db, transaction);
	}

	int StockRepository::takeOut(SQLiteWrapper::SQLite& db, int partId, int quantity, const std::string& note)
	{
		if (quantity <= 0)
		{
			return NoStockTransactionId;
		}
		// No "is there enough on the shelf" check on purpose — see the header note on negative stock.
		StockTransaction transaction;
		transaction.partId = partId;
		transaction.deltaQty = -quantity;
		transaction.reason = StockReason::CheckoutPartlist;
		transaction.note = note;
		return recordTransaction(db, transaction);
	}

	int StockRepository::correct(SQLiteWrapper::SQLite& db, int partId, int newQuantity, const std::string& note)
	{
		int delta = newQuantity - currentQuantity(db, partId);
		if (delta == 0)
		{
			return NoStockTransactionId;
		}
		StockTransaction transaction;
		transaction.partId = partId;
		transaction.deltaQty = delta;
		transaction.reason = StockReason::ManualAdjust;
		transaction.note = note;
		return recordTransaction(db, transaction);
	}

	std::vector<StockTransaction> StockRepository::history(SQLiteWrapper::SQLite& db, int partId)
	{
		// ORDER BY id, not created_at: created_at only has second resolution, so several transactions
		// entered in one burst would tie and come back in an arbitrary order.
		std::vector<StockTransaction> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_id,delta_qty,reason,ref_partlist_id,ref_order_id,unit_cost,currency,note,created_at "
			"FROM stock_transaction WHERE part_id=" + std::to_string(partId) + " ORDER BY id;"))
		{
			result.push_back(rowToTransaction(row));
		}
		return result;
	}

	int StockRepository::currentQuantity(SQLiteWrapper::SQLite& db, int partId)
	{
		return std::atoi(scalar(db,
			"SELECT COALESCE(SUM(delta_qty),0) FROM stock_transaction WHERE part_id="
			+ std::to_string(partId) + ";", "0").c_str());
	}

	bool StockRepository::refreshCachedQuantity(SQLiteWrapper::SQLite& db, int partId)
	{
		return db.executeWithParams(
			"UPDATE part SET stock_qty=(SELECT COALESCE(SUM(delta_qty),0) FROM stock_transaction "
			"WHERE part_id=part.id) WHERE id=?;", { std::to_string(partId) });
	}

	int StockRepository::backfillOpeningBalances(SQLiteWrapper::SQLite& db)
	{
		// "Non-zero stock_qty and not one transaction to its name" is both what makes a part a
		// backfill candidate and what makes this idempotent: the row written below disqualifies the
		// part from every later run, so the opening balance can never be counted twice.
		const std::string candidates =
			"FROM part WHERE stock_qty!=0 AND NOT EXISTS "
			"(SELECT 1 FROM stock_transaction s WHERE s.part_id=part.id)";

		int count = std::atoi(scalar(db, "SELECT COUNT(*) " + candidates + ";", "0").c_str());
		if (count == 0)
		{
			return 0;
		}
		bool ok = db.execute(
			"INSERT INTO stock_transaction (part_id, delta_qty, reason, note) "
			"SELECT id, stock_qty, '" + std::string(StockReason::Initial) + "', "
			"'Opening balance backfilled from part.stock_qty' " + candidates + ";");
		return ok ? count : 0;
	}

#endif

}
