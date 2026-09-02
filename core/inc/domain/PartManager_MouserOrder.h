// @file PartManager_MouserOrder.h
// @brief Plain data classes for `mouser_order` / `mouser_order_item` (§4).
//
// Zero Qt, zero SQL, zero business logic. The order is the bridge between "this
// partlist is short 40 resistors" and "40 resistors arrived": it is created as a
// `draft` from a partlist's shortfall, staged into a real Mouser cart (§6), and
// then lives until every line has been confirmed as arrived, at which point each
// arrival has already written its own `stock_transaction`.
//
// **PartManager never places the order.** `staged_in_cart` means the cart exists
// on mouser.com and the user goes there to check out; `submitted` is the user
// telling PartManager they did. Nothing here touches payment.
// @see docs/design/ARCHITECTURE.md §4, §6
// @see PartManager_OrderRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinels meaning "not yet inserted".
	constexpr int NoOrderId = 0;
	constexpr int NoOrderItemId = 0;

	// `mouser_order.status` vocabulary (§4). The order is linear: draft -> staged_in_cart ->
	// submitted -> partially_arrived -> closed, but a user may jump straight to submitted
	// (they ordered by hand) or close early, so nothing enforces the sequence.
	namespace OrderStatus
	{
		constexpr const char* Draft = "draft";
		constexpr const char* StagedInCart = "staged_in_cart";
		constexpr const char* Submitted = "submitted";
		constexpr const char* PartiallyArrived = "partially_arrived";
		constexpr const char* Closed = "closed";
	}

	// `mouser_order_item.status` vocabulary (§4) — orange / green / grey in the order view.
	namespace OrderItemStatus
	{
		constexpr const char* Pending = "pending";
		constexpr const char* Arrived = "arrived";
		constexpr const char* Backordered = "backordered";
	}

	// One `mouser_order` row.
	struct PART_MANAGER_API MouserOrder
	{
		int id = NoOrderId;
		int partlistId = 0;                     // 0 = a standalone order, not raised from a BOM
		std::string status = OrderStatus::Draft;
		std::string mouserCartId;               // CartKey returned by the Cart API (§6)
		std::string mouserOrderNumber;          // typed in by the user after checking out
		std::string createdAt;                  // ISO-8601, DB-assigned
		std::string submittedAt;
		std::string closedAt;
	};

	// One `mouser_order_item` row.
	struct PART_MANAGER_API MouserOrderItem
	{
		int id = NoOrderItemId;
		int orderId = NoOrderId;
		int partId = 0;
		int quantityOrdered = 0;
		double unitPrice = 0.0;                 // 0 = unknown; filled from the cart quote or by hand
		std::string currency;
		int quantityReceived = 0;
		std::string status = OrderItemStatus::Pending;
	};

}
