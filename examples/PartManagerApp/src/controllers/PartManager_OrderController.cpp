#include "controllers/PartManager_OrderController.h"

#include <QObject>
#include <QStringList>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	QString orderStatusLabel(const std::string& status)
	{
		if (status == OrderStatus::Draft)            { return QObject::tr("Draft"); }
		if (status == OrderStatus::StagedInCart)     { return QObject::tr("Staged in Mouser cart"); }
		if (status == OrderStatus::Submitted)        { return QObject::tr("Submitted"); }
		if (status == OrderStatus::PartiallyArrived) { return QObject::tr("Partially arrived"); }
		if (status == OrderStatus::Closed)           { return QObject::tr("Closed"); }
		// `status` is free-form TEXT, so a row written by an older version still reads.
		return QString::fromStdString(status);
	}

	QString orderItemStatusLabel(const std::string& status)
	{
		if (status == OrderItemStatus::Pending)     { return QObject::tr("Pending"); }
		if (status == OrderItemStatus::Arrived)     { return QObject::tr("Arrived"); }
		if (status == OrderItemStatus::Backordered) { return QObject::tr("Backordered"); }
		return QString::fromStdString(status);
	}

	QString orderStatusSummary(const std::vector<OrderLine>& lines)
	{
		int arrived = 0;
		int unstageable = 0;
		for (const OrderLine& line : lines)
		{
			if (line.item.status == OrderItemStatus::Arrived)
			{
				++arrived;
			}
			if (!line.stageable)
			{
				++unstageable;
			}
		}

		QStringList parts;
		parts.append(QObject::tr("%1 of %2 line(s) arrived")
			.arg(arrived).arg(static_cast<int>(lines.size())));
		if (unstageable > 0)
		{
			// Naming the count is the whole point: without it a short cart looks like a Mouser
			// problem rather than a part nobody ever linked.
			parts.append(QObject::tr("%n line(s) have no Mouser part number and cannot be staged",
				"", unstageable));
		}
		return parts.join(QObject::tr(", "));
	}

	StagingPlan planStaging(const std::vector<OrderLine>& lines)
	{
		StagingPlan plan;
		for (const OrderLine& line : lines)
		{
			if (!line.stageable)
			{
				plan.skippedPartNames.push_back(line.partName.empty()
					? line.partMpn : line.partName);
				continue;
			}
			// Already-arrived lines are not re-ordered. Staging a closed line again is how you
			// end up with twice the parts and no idea why.
			if (line.item.status == OrderItemStatus::Arrived)
			{
				continue;
			}
			MouserCartItemRequest item;
			item.mouserPartNumber = line.mouserPartNumber;
			// Only what is still outstanding, so re-staging a partially arrived order asks for
			// the remainder rather than the original quantity.
			item.quantity = line.item.quantityOrdered - line.item.quantityReceived;
			if (item.quantity > 0)
			{
				plan.items.push_back(item);
			}
		}
		return plan;
	}

	OrderController::OrderController(DatabaseHandle* handle)
		: m_handle(handle)
	{
	}

	bool OrderController::canStage()
	{
		return MouserCartClient::hasApiKey();
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// Same shape the other controllers use: null when there is no open database, so every
		// method below degrades to "nothing" instead of dereferencing a closed handle.
		SQLiteWrapper::SQLite* connectionOf(DatabaseHandle* handle)
		{
			return (handle != nullptr && handle->isOpen()) ? &handle->connection() : nullptr;
		}
	}

	std::vector<MouserOrder> OrderController::orders(bool openOnly) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db)
		{
			return std::vector<MouserOrder>();
		}
		return openOnly ? OrderRepository::listOpenOrders(*db) : OrderRepository::listOrders(*db);
	}

	bool OrderController::load(int orderId, MouserOrder& outOrder) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::findOrder(*db, orderId, outOrder) : false;
	}

	std::vector<OrderLine> OrderController::lines(int orderId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::lines(*db, orderId) : std::vector<OrderLine>();
	}

	int OrderController::itemCount(int orderId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::itemCount(*db, orderId) : 0;
	}

	bool OrderController::save(const MouserOrder& order) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::updateOrder(*db, order) : false;
	}

	bool OrderController::remove(int orderId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::deleteOrder(*db, orderId) : false;
	}

	OrderDraftPreview OrderController::previewFromPartlist(int partlistId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::previewFromPartlist(*db, partlistId) : OrderDraftPreview();
	}

	int OrderController::createDraftFromPartlist(int partlistId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::createDraftFromPartlist(*db, partlistId) : NoOrderId;
	}

	bool OrderController::saveItems(int orderId, const std::vector<MouserOrderItem>& items) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::saveItems(*db, orderId, items) : false;
	}

	bool OrderController::setStatus(int orderId, const std::string& status) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::setStatus(*db, orderId, status) : false;
	}

	bool OrderController::receive(int orderItemId, int totalReceived, double unitCost,
		const std::string& currency) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::receiveItem(*db, orderItemId, totalReceived, unitCost, currency)
			: false;
	}

	bool OrderController::close(int orderId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? OrderRepository::closeOrder(*db, orderId) : false;
	}

	MouserCartResult OrderController::stageToCart(int orderId) const
	{
		MouserCartResult result;
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db)
		{
			result.errorMessage = QObject::tr("No database is open.").toStdString();
			return result;
		}

		MouserOrder order;
		if (!OrderRepository::findOrder(*db, orderId, order))
		{
			result.errorMessage = QObject::tr("That order no longer exists.").toStdString();
			return result;
		}

		const StagingPlan plan = planStaging(OrderRepository::lines(*db, orderId));
		if (plan.items.empty())
		{
			// A request with no lines would come back as a successful empty cart, which reads as
			// "staged" when in fact nothing was.
			result.errorMessage = QObject::tr("Nothing on this order can be staged: every line is "
				"either already in or has no Mouser part number.").toStdString();
			return result;
		}

		MouserCartClient client;
		result = client.insertItems(order.mouserCartId, plan.items);
		if (result.ok && !result.cartKey.empty())
		{
			OrderRepository::setCartId(*db, orderId, result.cartKey);
		}
		return result;
	}

#else

	// No SQLiteWrapper: the dialog still builds and every action reports nothing rather than
	// pretending to have written something.
	std::vector<MouserOrder> OrderController::orders(bool) const { return std::vector<MouserOrder>(); }
	bool OrderController::load(int, MouserOrder&) const { return false; }
	std::vector<OrderLine> OrderController::lines(int) const { return std::vector<OrderLine>(); }
	int OrderController::itemCount(int) const { return 0; }
	bool OrderController::save(const MouserOrder&) const { return false; }
	bool OrderController::remove(int) const { return false; }
	OrderDraftPreview OrderController::previewFromPartlist(int) const { return OrderDraftPreview(); }
	int OrderController::createDraftFromPartlist(int) const { return NoOrderId; }
	bool OrderController::saveItems(int, const std::vector<MouserOrderItem>&) const { return false; }
	bool OrderController::setStatus(int, const std::string&) const { return false; }
	bool OrderController::receive(int, int, double, const std::string&) const { return false; }
	bool OrderController::close(int) const { return false; }
	MouserCartResult OrderController::stageToCart(int) const { return MouserCartResult(); }

#endif

}
