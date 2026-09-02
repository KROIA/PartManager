#include "persistence/PartManager_OrderRepository.h"
#include "persistence/PartManager_PartlistRepository.h"
#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_SqlLiteral.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		const char* const OrderColumns =
			"id,partlist_id,status,mouser_cart_id,mouser_order_number,created_at,submitted_at,closed_at";
		const char* const ItemColumns =
			"id,mouser_order_id,part_id,quantity_ordered,unit_price,currency,quantity_received,status";

		MouserOrder rowToOrder(const std::vector<std::string>& row)
		{
			MouserOrder order;
			order.id = std::atoi(row[0].c_str());
			// A NULL partlist_id comes back as an empty string, which atoi() reads as 0 — the same
			// value "standalone order" already means, so the state survives the round trip.
			order.partlistId = std::atoi(row[1].c_str());
			order.status = row[2];
			order.mouserCartId = row[3];
			order.mouserOrderNumber = row[4];
			order.createdAt = row[5];
			order.submittedAt = row[6];
			order.closedAt = row[7];
			return order;
		}

		MouserOrderItem rowToItem(const std::vector<std::string>& row)
		{
			MouserOrderItem item;
			item.id = std::atoi(row[0].c_str());
			item.orderId = std::atoi(row[1].c_str());
			item.partId = std::atoi(row[2].c_str());
			item.quantityOrdered = std::atoi(row[3].c_str());
			item.unitPrice = std::atof(row[4].c_str());
			item.currency = row[5];
			item.quantityReceived = std::atoi(row[6].c_str());
			item.status = row[7];
			return item;
		}

		// The status a line has once `received` of `ordered` have turned up. Kept in one place so
		// receiveItem() and closeOrder() can never disagree about what "arrived" means.
		const char* itemStatusFor(int received, int ordered)
		{
			if (received >= ordered && ordered > 0)
			{
				return OrderItemStatus::Arrived;
			}
			return OrderItemStatus::Pending;
		}
	}

	bool OrderRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = db.execute(
			"CREATE TABLE IF NOT EXISTS mouser_order ("
			"id INTEGER PRIMARY KEY,"
			"partlist_id INTEGER REFERENCES partlist(id),"
			"status TEXT NOT NULL DEFAULT 'draft',"
			"mouser_cart_id TEXT,"
			"mouser_order_number TEXT,"
			"created_at TEXT NOT NULL DEFAULT (datetime('now')),"
			"submitted_at TEXT,"
			"closed_at TEXT"
			");");
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS mouser_order_item ("
			"id INTEGER PRIMARY KEY,"
			"mouser_order_id INTEGER NOT NULL REFERENCES mouser_order(id),"
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"quantity_ordered INTEGER NOT NULL,"
			"unit_price REAL,"
			"currency TEXT,"
			"quantity_received INTEGER NOT NULL DEFAULT 0,"
			"status TEXT NOT NULL DEFAULT 'pending'"
			");") && ok;
		ok = db.execute("CREATE INDEX IF NOT EXISTS idx_mouser_order_item_order "
			"ON mouser_order_item(mouser_order_id);") && ok;
		return ok;
	}

	int OrderRepository::insertOrder(SQLiteWrapper::SQLite& db, const MouserOrder& order)
	{
		// partlist_id has to go in as a real NULL for a standalone order, or a later join would
		// match partlist id 0 instead of failing to match — same reasoning as PartlistRepository.
		const std::string status = order.status.empty() ? std::string(OrderStatus::Draft) : order.status;
		const bool ok = order.partlistId == NoPartlistId
			? db.executeWithParams(
				"INSERT INTO mouser_order (partlist_id, status, mouser_cart_id, mouser_order_number) "
				"VALUES (NULL, ?, ?, ?);",
				{ status, order.mouserCartId, order.mouserOrderNumber })
			: db.executeWithParams(
				"INSERT INTO mouser_order (partlist_id, status, mouser_cart_id, mouser_order_number) "
				"VALUES (?, ?, ?, ?);",
				{ std::to_string(order.partlistId), status, order.mouserCartId, order.mouserOrderNumber });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoOrderId;
	}

	bool OrderRepository::updateOrder(SQLiteWrapper::SQLite& db, const MouserOrder& order)
	{
		// `partlist_id` and `created_at` are deliberately not updatable: which BOM raised an order
		// and when is a fact about its history, not a field. Status moves through setStatus(),
		// which owns the timestamps.
		return db.executeWithParams(
			"UPDATE mouser_order SET mouser_cart_id=?, mouser_order_number=? WHERE id=?;",
			{ order.mouserCartId, order.mouserOrderNumber, std::to_string(order.id) });
	}

	bool OrderRepository::deleteOrder(SQLiteWrapper::SQLite& db, int orderId)
	{
		// The stock transactions an arrival already posted are deliberately left alone: they
		// record parts that physically turned up, and deleting the paperwork does not send them
		// back. They keep their now-dangling ref_order_id, which is what "orphaned history" looks
		// like everywhere else in this schema.
		const std::string id = std::to_string(orderId);
		db.executeWithParams("DELETE FROM mouser_order_item WHERE mouser_order_id=?;", { id });
		return db.executeWithParams("DELETE FROM mouser_order WHERE id=?;", { id });
	}

	bool OrderRepository::findOrder(SQLiteWrapper::SQLite& db, int orderId, MouserOrder& outOrder)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			std::string("SELECT ") + OrderColumns + " FROM mouser_order WHERE id="
			+ std::to_string(orderId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outOrder = rowToOrder(rows.front());
		return true;
	}

	std::vector<MouserOrder> OrderRepository::listOrders(SQLiteWrapper::SQLite& db)
	{
		std::vector<MouserOrder> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + OrderColumns + " FROM mouser_order ORDER BY id DESC;"))
		{
			result.push_back(rowToOrder(row));
		}
		return result;
	}

	std::vector<MouserOrder> OrderRepository::listOpenOrders(SQLiteWrapper::SQLite& db)
	{
		std::vector<MouserOrder> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + OrderColumns + " FROM mouser_order WHERE status<>"
			+ sqlLiteral(OrderStatus::Closed) + " ORDER BY id DESC;"))
		{
			result.push_back(rowToOrder(row));
		}
		return result;
	}

	std::vector<OrderLine> OrderRepository::lines(SQLiteWrapper::SQLite& db, int orderId)
	{
		std::vector<OrderLine> result;
		// LEFT JOIN on the seller link so a line whose part has no Mouser number still shows up —
		// it is exactly the line the user has to fix before staging, and hiding it would leave
		// them wondering why the cart came back short.
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT i.id,i.mouser_order_id,i.part_id,i.quantity_ordered,i.unit_price,i.currency,"
			"i.quantity_received,i.status,p.name,p.mpn,p.stock_qty,"
			"(SELECT l.seller_part_number FROM part_seller_link l JOIN seller s ON s.id=l.seller_id "
			" WHERE l.part_id=i.part_id AND s.api_type='mouser' "
			" ORDER BY l.is_primary DESC, l.id LIMIT 1) "
			"FROM mouser_order_item i LEFT JOIN part p ON p.id=i.part_id "
			"WHERE i.mouser_order_id=" + std::to_string(orderId) + " ORDER BY i.id;"))
		{
			OrderLine line;
			line.item = rowToItem(row);
			line.partName = row[8];
			line.partMpn = row[9];
			line.stockQty = std::atoi(row[10].c_str());
			line.mouserPartNumber = row[11];
			line.stageable = !line.mouserPartNumber.empty() && line.item.quantityOrdered > 0;
			result.push_back(line);
		}
		return result;
	}

	int OrderRepository::itemCount(SQLiteWrapper::SQLite& db, int orderId)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT COUNT(*) FROM mouser_order_item WHERE mouser_order_id="
			+ std::to_string(orderId) + ";");
		if (rows.empty() || rows.front().empty())
		{
			return 0;
		}
		return std::atoi(rows.front().front().c_str());
	}

	bool OrderRepository::saveItems(SQLiteWrapper::SQLite& db, int orderId,
		const std::vector<MouserOrderItem>& items)
	{
		bool ok = db.executeWithParams("DELETE FROM mouser_order_item WHERE mouser_order_id=?;",
			{ std::to_string(orderId) });
		for (const MouserOrderItem& item : items)
		{
			ok = db.executeWithParams(
				"INSERT INTO mouser_order_item (mouser_order_id, part_id, quantity_ordered, "
				"unit_price, currency, quantity_received, status) VALUES (?, ?, ?, ?, ?, ?, ?);",
				{ std::to_string(orderId), std::to_string(item.partId),
				  std::to_string(item.quantityOrdered), std::to_string(item.unitPrice),
				  item.currency, std::to_string(item.quantityReceived),
				  item.status.empty() ? std::string(OrderItemStatus::Pending) : item.status }) && ok;
		}
		return ok;
	}

	OrderDraftPreview OrderRepository::previewFromPartlist(SQLiteWrapper::SQLite& db, int partlistId)
	{
		OrderDraftPreview preview;

		// **Needed is summed per part, and stock is subtracted once at the end** — not
		// `sum(line.shortfallQty)`. A BOM that lists the same resistor on three lines needing 10
		// each, with 20 on the shelf, reports a per-line shortfall of 0 three times (each line
		// alone is covered) while the build is really 10 short. Every line's shortfall was
		// computed against the same untouched stock figure, so those numbers cannot be added.
		//
		// std::map, not unordered_map: the lines come out in part-id order, which is stable across
		// runs. A hash order would reshuffle the draft every time the user reopened it.
		std::map<int, int> neededByPart;
		std::map<int, int> stockByPart;
		for (const PartlistLine& line : PartlistRepository::lines(db, partlistId))
		{
			if (!line.resolved)
			{
				++preview.skippedUnresolved;
				continue;
			}
			neededByPart[line.item.partId] += line.neededQty;
			stockByPart[line.item.partId] = line.stockQty;
		}

		for (const std::pair<const int, int>& entry : neededByPart)
		{
			const int shortfall = entry.second - stockByPart[entry.first];
			if (shortfall <= 0)
			{
				continue;
			}
			MouserOrderItem item;
			item.partId = entry.first;
			item.quantityOrdered = shortfall;
			OrderLine line;
			line.item = item;
			preview.lines.push_back(line);
		}
		if (preview.lines.empty())
		{
			return preview;
		}

		// Fill in the display/staging columns from the same query shape lines() uses, so the
		// preview and a saved order show identical information.
		std::string ids;
		for (const OrderLine& line : preview.lines)
		{
			if (!ids.empty())
			{
				ids += ",";
			}
			ids += std::to_string(line.item.partId);
		}
		std::map<int, std::pair<std::string, std::string>> names;   // partId -> (name, mpn)
		std::map<int, int> stock;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,name,mpn,stock_qty FROM part WHERE id IN (" + ids + ");"))
		{
			const int partId = std::atoi(row[0].c_str());
			names[partId] = { row[1], row[2] };
			stock[partId] = std::atoi(row[3].c_str());
		}
		for (OrderLine& line : preview.lines)
		{
			line.partName = names[line.item.partId].first;
			line.partMpn = names[line.item.partId].second;
			line.stockQty = stock[line.item.partId];
			line.mouserPartNumber = SellerRepository::mouserPartNumber(db, line.item.partId);
			line.stageable = !line.mouserPartNumber.empty();
		}
		return preview;
	}

	int OrderRepository::createDraftFromPartlist(SQLiteWrapper::SQLite& db, int partlistId)
	{
		const OrderDraftPreview preview = previewFromPartlist(db, partlistId);
		if (preview.lines.empty())
		{
			return NoOrderId;
		}

		MouserOrder order;
		order.partlistId = partlistId;
		order.status = OrderStatus::Draft;
		const int orderId = insertOrder(db, order);
		if (orderId == NoOrderId)
		{
			return NoOrderId;
		}

		std::vector<MouserOrderItem> items;
		for (const OrderLine& line : preview.lines)
		{
			items.push_back(line.item);
		}
		if (!saveItems(db, orderId, items))
		{
			// A half-written order is worse than none: the user would stage a cart missing lines
			// and never know which.
			deleteOrder(db, orderId);
			return NoOrderId;
		}
		return orderId;
	}

	bool OrderRepository::setStatus(SQLiteWrapper::SQLite& db, int orderId, const std::string& status)
	{
		if (status.empty())
		{
			return false;
		}
		std::string sql = "UPDATE mouser_order SET status=?";
		// The timestamps are set once, when the order first reaches the state — re-marking an
		// order as submitted must not rewrite the day it was submitted.
		if (status == OrderStatus::Submitted)
		{
			sql += ", submitted_at=IFNULL(submitted_at, datetime('now'))";
		}
		else if (status == OrderStatus::Closed)
		{
			sql += ", closed_at=IFNULL(closed_at, datetime('now'))";
		}
		sql += " WHERE id=?;";
		return db.executeWithParams(sql, { status, std::to_string(orderId) });
	}

	bool OrderRepository::setCartId(SQLiteWrapper::SQLite& db, int orderId, const std::string& cartId)
	{
		if (!db.executeWithParams("UPDATE mouser_order SET mouser_cart_id=? WHERE id=?;",
			{ cartId, std::to_string(orderId) }))
		{
			return false;
		}
		// An empty CartKey means the staging failed, so the order stays where it was.
		return cartId.empty() ? true : setStatus(db, orderId, OrderStatus::StagedInCart);
	}

	bool OrderRepository::receiveItem(SQLiteWrapper::SQLite& db, int orderItemId, int totalReceived,
		double unitCost, const std::string& currency)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			std::string("SELECT ") + ItemColumns + " FROM mouser_order_item WHERE id="
			+ std::to_string(orderItemId) + ";");
		if (rows.empty())
		{
			return false;
		}
		const MouserOrderItem item = rowToItem(rows.front());

		// Clamped, not trusted: a negative total is meaningless, and "more arrived than ordered"
		// is a real thing that happens (Mouser ships full reels), so only the lower bound is fixed.
		const int received = std::max(0, totalReceived);
		const int delta = received - item.quantityReceived;

		if (!db.executeWithParams(
			"UPDATE mouser_order_item SET quantity_received=?, status=?, unit_price=?, currency=? "
			"WHERE id=?;",
			{ std::to_string(received), itemStatusFor(received, item.quantityOrdered),
			  std::to_string(unitCost > 0.0 ? unitCost : item.unitPrice),
			  currency.empty() ? item.currency : currency, std::to_string(orderItemId) }))
		{
			return false;
		}

		if (delta == 0)
		{
			// Nothing physically moved, so nothing goes in the stock log. Confirming the same
			// arrival twice is a no-op rather than a double restock (see header note).
			return true;
		}

		StockTransaction transaction;
		transaction.partId = item.partId;
		transaction.deltaQty = delta;
		// A negative delta is a correction to an over-confirmed arrival, not a restock — logging
		// it as one would make the history claim parts arrived that never did.
		transaction.reason = delta > 0 ? StockReason::Restock : StockReason::ManualAdjust;
		transaction.refOrderId = item.orderId;
		transaction.unitCost = unitCost;
		transaction.currency = currency;
		transaction.note = delta > 0
			? "arrived, order #" + std::to_string(item.orderId)
			: "arrival correction, order #" + std::to_string(item.orderId);
		StockRepository::recordTransaction(db, transaction);

		if (delta > 0 && unitCost > 0.0)
		{
			// §3: what was actually paid is a separate observation from Mouser's list price.
			PartSellerLink link;
			if (SellerRepository::primaryLink(db, item.partId, link))
			{
				SellerRepository::recordPaidPrice(db, link.id, unitCost, currency);
			}
		}

		// An order with some but not all lines in is `partially_arrived`; the last line to land
		// leaves it there rather than auto-closing, because closing is the user's decision (they
		// may still be waiting on a backorder they want visible).
		MouserOrder order;
		if (findOrder(db, item.orderId, order) && order.status != OrderStatus::Closed)
		{
			setStatus(db, item.orderId, OrderStatus::PartiallyArrived);
		}
		return true;
	}

	bool OrderRepository::closeOrder(SQLiteWrapper::SQLite& db, int orderId)
	{
		// Anything not fully in is backordered, not quietly completed — see the header note.
		db.executeWithParams(
			"UPDATE mouser_order_item SET status=? "
			"WHERE mouser_order_id=? AND quantity_received<quantity_ordered;",
			{ OrderItemStatus::Backordered, std::to_string(orderId) });
		return setStatus(db, orderId, OrderStatus::Closed);
	}

#endif

}
