// @file PartManager_OrderRepository.h
// @brief CRUD and lifecycle for `mouser_order` / `mouser_order_item` (§4).
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartlistRepository.
//
// The lifecycle transitions live here rather than in a separate service for the
// same reason PartlistRepository owns the shortfall arithmetic: every one of
// them is a multi-table write that must stay consistent (an arrival is one
// `mouser_order_item` update *and* one `stock_transaction` *and* a refreshed
// `part.stock_qty`), and splitting them across layers is how half of them
// eventually get skipped. `core/mouser` keeps only the part that actually
// speaks HTTP (MouserCartClient).
//
// **receiveItem() is the only way stock ever moves for an order**, and it is
// idempotent in the sense that matters: it records the *delta* against what was
// already received, so confirming "10 of 40 arrived" twice does not restock 20.
// Marking an already-fully-received line again writes nothing.
// @see docs/design/ARCHITECTURE.md §4, §6
// @see PartManager_MouserOrder.h, PartManager_PartlistRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_MouserOrder.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// One order line with everything the order view shows, resolved in one query so the grid
	// does not issue a lookup per row.
	struct PART_MANAGER_API OrderLine
	{
		MouserOrderItem item;
		std::string partName;
		std::string partMpn;             // manufacturer part number, for the human
		std::string mouserPartNumber;    // seller article number, what the Cart API needs (§6)
		int stockQty = 0;                // the part's stock right now, for context
		// True when the line can be staged: the Cart API refuses a line without a Mouser P/N,
		// so the UI has to say so before the call rather than after it fails.
		bool stageable = false;
	};

	// What a partlist would need ordering, before an order exists. `skippedUnresolved` counts
	// partlist rows that point at no part at all — they cannot be ordered and the user has to
	// resolve them first, so silently dropping them would be a lie.
	struct PART_MANAGER_API OrderDraftPreview
	{
		std::vector<OrderLine> lines;
		int skippedUnresolved = 0;
	};

	class PART_MANAGER_API OrderRepository
	{
		OrderRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates mouser_order/mouser_order_item if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// order CRUD. insertOrder() returns the new id, NoOrderId on failure.
		static int insertOrder(SQLiteWrapper::SQLite& db, const MouserOrder& order);
		static bool updateOrder(SQLiteWrapper::SQLite& db, const MouserOrder& order);
		// Also removes the order's items — foreign keys are declared but never enforced.
		static bool deleteOrder(SQLiteWrapper::SQLite& db, int orderId);
		static bool findOrder(SQLiteWrapper::SQLite& db, int orderId, MouserOrder& outOrder);
		// Newest first, which is the order the order screen lists them in.
		static std::vector<MouserOrder> listOrders(SQLiteWrapper::SQLite& db);
		// Only the ones still worth looking at (everything but `closed`).
		static std::vector<MouserOrder> listOpenOrders(SQLiteWrapper::SQLite& db);

		// The order's rows joined against part and part_seller_link.
		static std::vector<OrderLine> lines(SQLiteWrapper::SQLite& db, int orderId);
		static int itemCount(SQLiteWrapper::SQLite& db, int orderId);
		// Replaces the order's items in one shot, same reasoning as PartlistRepository::saveItems().
		static bool saveItems(SQLiteWrapper::SQLite& db, int orderId,
			const std::vector<MouserOrderItem>& items);

		// What a partlist is short of, as order lines, without writing anything. The quantities
		// are PartlistRepository's shortfall numbers (`quantity_per_unit * multiplier - stock_qty`),
		// summed per part so a BOM that lists the same resistor on three lines orders it once.
		static OrderDraftPreview previewFromPartlist(SQLiteWrapper::SQLite& db, int partlistId);
		// The same thing, persisted as a `draft` order. Returns NoOrderId when there is nothing
		// to order — an order with no lines is not a useful thing to create.
		static int createDraftFromPartlist(SQLiteWrapper::SQLite& db, int partlistId);

		// Sets `status` and stamps submitted_at/closed_at when moving into those states.
		static bool setStatus(SQLiteWrapper::SQLite& db, int orderId, const std::string& status);
		// Records the CartKey the Cart API handed back and moves the order to `staged_in_cart`.
		static bool setCartId(SQLiteWrapper::SQLite& db, int orderId, const std::string& cartId);

		// Confirms arrivals for one line. `totalReceived` is the running total for that line, not
		// an increment; the stock transaction posted is the difference against what was already
		// booked (see header note). `unitCost`/`currency` are what was actually paid and are
		// written both onto the transaction and, when the part has a seller link, as a `paid`
		// price observation (§3). Returns false when the line does not exist.
		static bool receiveItem(SQLiteWrapper::SQLite& db, int orderItemId, int totalReceived,
			double unitCost = 0.0, const std::string& currency = std::string());

		// Closes an order. Any line short of its ordered quantity is marked `backordered` rather
		// than silently completed — the remainder is a fact, not a rounding error. Stock is NOT
		// touched here: every arrival already posted its own transaction through receiveItem().
		static bool closeOrder(SQLiteWrapper::SQLite& db, int orderId);
#endif

	};

}
