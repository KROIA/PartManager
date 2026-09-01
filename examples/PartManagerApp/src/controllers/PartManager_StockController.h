// @file PartManager_StockController.h
// @brief Widget-free logic behind the ribbon's Restock/Take Out and the editor's stock field (§3, §12b).
//
// Same split as the other controllers: the pure parts (the running quantity a
// history table shows, the reason vocabulary's display labels) are free
// functions unit-tested in TST_StockController; the class itself is the thin
// StockRepository wrapper the dialogs talk to.
//
// Everything that changes a quantity goes through here, so there is exactly one
// path from the UI into `stock_transaction` — the editor's quantity field is a
// correction (§3 `manual_adjust`), not a write to the cached `part.stock_qty`.
// Over-draw is not blocked (see StockRepository's header note); the resulting
// negative quantity is shown, not prevented.
// @see docs/design/ARCHITECTURE.md §3, §7, §12b
// @see PartManager_StockRepository.h, PartManager_StockDialog.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_StockTransaction.h"
#include <QString>
#include <vector>

namespace PartManager
{

	// The quantity standing after each transaction of an oldest-first history — what the
	// history table's "Result" column shows. One entry per input transaction.
	std::vector<int> runningQuantities(const std::vector<StockTransaction>& history);

	// Display text for a `stock_transaction.reason`. The StockReason vocabulary is app
	// chrome and gets tr()'d; an unknown/legacy string is shown unchanged rather than dropped.
	QString stockReasonLabel(const std::string& reason);

	// StockRepository wrapper shared by StockDialog (ribbon) and PartEditorDialog.
	// Holds the caller's DatabaseHandle without owning it — MainWindow's controller does.
	class StockController
	{
	public:
		explicit StockController(DatabaseHandle* handle);

		// The authoritative quantity — SUM(delta_qty) from the log, not the cached column.
		int quantity(int partId) const;

		// +quantity / -quantity with the user's own note (never tr()'d — it is their text).
		// False when nothing was written: a non-positive quantity, or no open database.
		bool restock(int partId, int quantity, const QString& note = QString()) const;
		bool takeOut(int partId, int quantity, const QString& note = QString()) const;

		// "The shelf actually holds newQuantity": logs the difference as `manual_adjust`.
		// True when the quantity now matches, including the no-op case where it already did.
		bool setQuantity(int partId, int newQuantity, const QString& note = QString()) const;

		// One part's transactions, oldest first.
		std::vector<StockTransaction> history(int partId) const;

	private:
		DatabaseHandle* m_handle;
	};

}
