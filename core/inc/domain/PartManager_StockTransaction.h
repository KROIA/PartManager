// @file PartManager_StockTransaction.h
// @brief Plain data class for a `stock_transaction` row (§3) — the source of truth for stock.
//
// Zero Qt, zero SQL, zero business logic. `part.stock_qty` is only a cache of
// SUM(delta_qty) over these rows, so every quantity change in the app is one
// row here plus a cache refresh (StockRepository does both in one call).
//
// `reason` stays a free-form TEXT column; the StockReason constants below exist
// only to catch typos at the C++ call site, the same way PartFileRole does for
// `part_file.role`. Unknown/legacy strings still round-trip untouched.
// @see docs/design/ARCHITECTURE.md §3
// @see PartManager_StockRepository.h, PartManager_Part.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for StockTransaction::id meaning "not yet inserted" / "no transaction".
	constexpr int NoStockTransactionId = 0;

	// The `stock_transaction.reason` vocabulary from §3.
	namespace StockReason
	{
		constexpr const char* Restock = "restock";                     // +n arrived (order close, manual restock)
		constexpr const char* CheckoutPartlist = "checkout_partlist";  // -n taken for a partlist build
		constexpr const char* ManualAdjust = "manual_adjust";          // +/-n after a physical recount
		constexpr const char* Loss = "loss";                           // -n broken/lost
		constexpr const char* Initial = "initial";                     // opening balance, see StockRepository::backfillOpeningBalances()
	}

	// One `stock_transaction` row.
	struct PART_MANAGER_API StockTransaction
	{
		int id = NoStockTransactionId;  // SQLite primary key; NoStockTransactionId (0) = not yet inserted
		int partId = 0;
		int deltaQty = 0;               // signed: + restock, - checkout/loss
		std::string reason;             // one of the StockReason constants above
		int refPartlistId = 0;          // 0 = not tied to a partlist
		int refOrderId = 0;             // 0 = not tied to an order
		double unitCost = 0.0;          // what was actually paid, restock only (0 = unknown)
		std::string currency;
		std::string note;               // free text, e.g. "recount 2026-09-01"
		std::string createdAt;          // set by SQLite on insert, read-only for callers
	};

}
