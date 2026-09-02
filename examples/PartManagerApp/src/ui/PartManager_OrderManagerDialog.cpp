#include "ui/PartManager_OrderManagerDialog.h"
#include "ui_PartManager_OrderManagerDialog.h"

#include <QColor>
#include <QDesktopServices>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QUrl>

namespace PartManager
{
	namespace
	{
		// Rows are filtered, so a row index is not an index into m_orders/m_lines and the id has
		// to travel with the item — same reasoning as PartlistManagerDialog.
		constexpr int OrderIdRole = Qt::UserRole;
		constexpr int LineIdRole = Qt::UserRole;

		// §4's orange/green. Muted enough to read black text on, unlike the named Qt colours.
		const QColor PendingRowColor(0xFB, 0xE1, 0x8F);
		const QColor ArrivedRowColor(0xC8, 0xE6, 0xC9);
		const QColor BackorderedRowColor(0xE0, 0xE0, 0xE0);
		// A line with no Mouser article number cannot be staged at all, which is a different
		// problem from "not here yet" and gets its own colour rather than sharing pending's.
		const QColor UnstageableRowColor(0xFF, 0xCD, 0xD2);

		enum OrderColumn
		{
			OrderColumnId = 0,
			OrderColumnStatus,
			OrderColumnLines,
			OrderColumnCart,
			OrderColumnNumber,
			OrderColumnCreated,
			OrderColumnCount
		};

		enum LineColumn
		{
			LineColumnPart = 0,
			LineColumnMouserNumber,
			LineColumnOrdered,
			LineColumnReceived,
			LineColumnStock,
			LineColumnStatus,
			LineColumnCount
		};
	}

	OrderManagerDialog::OrderManagerDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::OrderManagerDialog)
		, m_controller(handle)
	{
		m_ui->setupUi(this);

		m_ui->orderTable->setColumnCount(OrderColumnCount);
		m_ui->orderTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Order") << tr("Status") << tr("Lines") << tr("Mouser cart")
			<< tr("Order number") << tr("Created"));
		m_ui->orderTable->horizontalHeader()->setSectionResizeMode(OrderColumnStatus,
			QHeaderView::Stretch);

		m_ui->lineTable->setColumnCount(LineColumnCount);
		m_ui->lineTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Part") << tr("Mouser P/N") << tr("Ordered") << tr("Received")
			<< tr("In stock") << tr("Status"));
		m_ui->lineTable->horizontalHeader()->setSectionResizeMode(LineColumnPart,
			QHeaderView::Stretch);

		connect(m_ui->openOnlyCheck, &QCheckBox::toggled, this, &OrderManagerDialog::reload);
		connect(m_ui->orderTable, &QTableWidget::itemSelectionChanged,
			this, &OrderManagerDialog::showLines);
		connect(m_ui->lineTable, &QTableWidget::itemSelectionChanged,
			this, &OrderManagerDialog::updateButtons);
		connect(m_ui->lineTable, &QTableWidget::cellChanged, this, &OrderManagerDialog::onQuantityEdited);
		connect(m_ui->lineTable, &QTableWidget::itemDoubleClicked,
			this, &OrderManagerDialog::confirmArrival);
		connect(m_ui->stageButton, &QPushButton::clicked, this, &OrderManagerDialog::stageToCart);
		connect(m_ui->openCartButton, &QPushButton::clicked, this, &OrderManagerDialog::openCartOnMouser);
		connect(m_ui->submittedButton, &QPushButton::clicked, this, &OrderManagerDialog::markSubmitted);
		connect(m_ui->receiveButton, &QPushButton::clicked, this, &OrderManagerDialog::confirmArrival);
		connect(m_ui->closeOrderButton, &QPushButton::clicked, this, &OrderManagerDialog::closeOrder);
		connect(m_ui->deleteButton, &QPushButton::clicked, this, &OrderManagerDialog::deleteSelected);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &OrderManagerDialog::accept);

		reload();
	}

	OrderManagerDialog::~OrderManagerDialog()
	{
		delete m_ui;
	}

	void OrderManagerDialog::selectOrder(int orderId)
	{
		for (int row = 0; row < m_ui->orderTable->rowCount(); ++row)
		{
			QTableWidgetItem* item = m_ui->orderTable->item(row, OrderColumnId);
			if (item != nullptr && item->data(OrderIdRole).toInt() == orderId)
			{
				m_ui->orderTable->selectRow(row);
				return;
			}
		}
		// The order exists but is filtered out — showing "Orders" with nothing selected after the
		// user just created one would look like the creation failed.
		if (m_ui->openOnlyCheck->isChecked())
		{
			m_ui->openOnlyCheck->setChecked(false);
			selectOrder(orderId);
		}
	}

	void OrderManagerDialog::reload()
	{
		m_orders = m_controller.orders(m_ui->openOnlyCheck->isChecked());

		m_ui->orderTable->clearContents();
		m_ui->orderTable->setRowCount(0);
		int row = 0;
		for (const MouserOrder& order : m_orders)
		{
			m_ui->orderTable->insertRow(row);
			const QString cells[OrderColumnCount] = {
				tr("#%1").arg(order.id),
				orderStatusLabel(order.status),
				QString::number(m_controller.itemCount(order.id)),
				order.mouserCartId.empty() ? tr("—") : QString::fromStdString(order.mouserCartId),
				order.mouserOrderNumber.empty()
					? tr("—") : QString::fromStdString(order.mouserOrderNumber),
				QString::fromStdString(order.createdAt),
			};
			for (int column = 0; column < OrderColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setData(OrderIdRole, order.id);
				item->setToolTip(cells[column]);
				m_ui->orderTable->setItem(row, column, item);
			}
			++row;
		}
		m_ui->orderTable->resizeColumnsToContents();
		m_ui->orderTable->horizontalHeader()->setSectionResizeMode(OrderColumnStatus,
			QHeaderView::Stretch);

		if (m_orders.empty())
		{
			m_ui->statusLabel->setText(m_ui->openOnlyCheck->isChecked()
				? tr("No open orders. Raise one from a partlist's “Order Shortfall” button.")
				: tr("No orders yet. Raise one from a partlist's “Order Shortfall” button."));
		}
		showLines();
	}

	void OrderManagerDialog::showLines()
	{
		const int orderId = selectedOrderId();
		m_lines = orderId == NoOrderId ? std::vector<OrderLine>() : m_controller.lines(orderId);

		MouserOrder order;
		const bool isDraft = orderId != NoOrderId && m_controller.load(orderId, order)
			&& order.status == OrderStatus::Draft;

		// The cellChanged handler would otherwise fire on every setItem() below and write a
		// half-built row straight back to the database.
		m_populating = true;
		m_ui->lineTable->clearContents();
		m_ui->lineTable->setRowCount(0);
		int row = 0;
		for (const OrderLine& line : m_lines)
		{
			m_ui->lineTable->insertRow(row);
			const QString cells[LineColumnCount] = {
				line.partName.empty() ? tr("(deleted part)") : QString::fromStdString(line.partName),
				line.mouserPartNumber.empty()
					? tr("— not linked to Mouser") : QString::fromStdString(line.mouserPartNumber),
				QString::number(line.item.quantityOrdered),
				QString::number(line.item.quantityReceived),
				QString::number(line.stockQty),
				orderItemStatusLabel(line.item.status),
			};

			QColor background = PendingRowColor;
			if (line.item.status == OrderItemStatus::Arrived)        { background = ArrivedRowColor; }
			else if (line.item.status == OrderItemStatus::Backordered) { background = BackorderedRowColor; }
			else if (!line.stageable)                                { background = UnstageableRowColor; }

			for (int column = 0; column < LineColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setData(LineIdRole, line.item.id);
				item->setBackground(background);
				// Only the ordered quantity is editable, and only while the order is a draft —
				// changing it after staging would put PartManager's record out of step with
				// Mouser's without telling anyone.
				Qt::ItemFlags flags = item->flags() & ~Qt::ItemIsEditable;
				if (isDraft && column == LineColumnOrdered)
				{
					flags |= Qt::ItemIsEditable;
					item->setToolTip(tr("Editable while the order is a draft."));
				}
				else
				{
					item->setToolTip(cells[column]);
				}
				item->setFlags(flags);
				m_ui->lineTable->setItem(row, column, item);
			}
			if (line.partMpn.empty() == false)
			{
				m_ui->lineTable->item(row, LineColumnPart)->setToolTip(
					QString::fromStdString(line.partMpn));
			}
			++row;
		}
		m_populating = false;

		m_ui->lineTable->resizeColumnsToContents();
		m_ui->lineTable->horizontalHeader()->setSectionResizeMode(LineColumnPart, QHeaderView::Stretch);

		if (!m_lines.empty())
		{
			m_ui->statusLabel->setText(orderStatusSummary(m_lines));
		}
		updateButtons();
	}

	int OrderManagerDialog::selectedOrderId() const
	{
		QTableWidgetItem* item = m_ui->orderTable->item(m_ui->orderTable->currentRow(), OrderColumnId);
		return item == nullptr ? NoOrderId : item->data(OrderIdRole).toInt();
	}

	const OrderLine* OrderManagerDialog::selectedLine() const
	{
		QTableWidgetItem* item = m_ui->lineTable->item(m_ui->lineTable->currentRow(), LineColumnPart);
		if (item == nullptr)
		{
			return nullptr;
		}
		const int id = item->data(LineIdRole).toInt();
		for (const OrderLine& line : m_lines)
		{
			if (line.item.id == id)
			{
				return &line;
			}
		}
		return nullptr;
	}

	void OrderManagerDialog::updateButtons()
	{
		const int orderId = selectedOrderId();
		MouserOrder order;
		const bool hasOrder = orderId != NoOrderId && m_controller.load(orderId, order);
		const bool isClosed = hasOrder && order.status == OrderStatus::Closed;

		// Staging needs the Cart API key, which is a separate one from the search key. Saying so
		// on the button beats letting the user press it and read an error.
		const bool canStage = OrderController::canStage();
		m_ui->stageButton->setEnabled(hasOrder && !isClosed && canStage);
		m_ui->stageButton->setToolTip(canStage
			? tr("Builds the cart on mouser.com from this order's outstanding lines.")
			: tr("Set the MOUSER_API environment variable and restart to stage carts."));

		m_ui->openCartButton->setEnabled(hasOrder && !order.mouserCartId.empty());
		m_ui->submittedButton->setEnabled(hasOrder && !isClosed
			&& order.status != OrderStatus::Submitted);
		m_ui->receiveButton->setEnabled(selectedLine() != nullptr && !isClosed);
		m_ui->closeOrderButton->setEnabled(hasOrder && !isClosed);
		m_ui->deleteButton->setEnabled(hasOrder);
	}

	void OrderManagerDialog::onQuantityEdited(int row, int column)
	{
		if (m_populating || column != LineColumnOrdered)
		{
			return;
		}
		QTableWidgetItem* item = m_ui->lineTable->item(row, column);
		if (item == nullptr)
		{
			return;
		}

		bool parsed = false;
		const int quantity = item->text().toInt(&parsed);
		if (!parsed || quantity < 0)
		{
			// Put the old value back rather than storing nonsense — the cell is the only record
			// of what was there.
			showLines();
			return;
		}

		const int lineId = item->data(LineIdRole).toInt();
		std::vector<MouserOrderItem> items;
		for (const OrderLine& line : m_lines)
		{
			MouserOrderItem edited = line.item;
			if (edited.id == lineId)
			{
				edited.quantityOrdered = quantity;
			}
			items.push_back(edited);
		}
		// saveItems() replaces the rows wholesale, so the ids change — reload rather than trust
		// the in-memory copy.
		m_controller.saveItems(selectedOrderId(), items);
		showLines();
	}

	void OrderManagerDialog::stageToCart()
	{
		const int orderId = selectedOrderId();
		if (orderId == NoOrderId)
		{
			return;
		}

		const StagingPlan plan = planStaging(m_lines);
		if (!plan.skippedPartNames.empty())
		{
			QStringList names;
			for (const std::string& name : plan.skippedPartNames)
			{
				names.append(QString::fromStdString(name));
			}
			// Asked before the call, not reported after it: a cart that silently came back short
			// is the failure mode this whole confirmation exists to prevent.
			if (QMessageBox::question(this, tr("Some lines cannot be staged"),
				tr("These parts have no Mouser part number and will be left out of the cart:\n\n%1\n\n"
				   "Stage the rest anyway?").arg(names.join(QStringLiteral("\n"))),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}

		const MouserCartResult result = m_controller.stageToCart(orderId);
		if (!result.ok)
		{
			QMessageBox::warning(this, tr("Could not stage the cart"),
				QString::fromStdString(result.errorMessage));
			reload();
			return;
		}

		// The response lists the **whole cart**, not only what this call staged (verified live
		// 2026-09-02). So a rejected line is only ours if we actually asked for it — otherwise a
		// bad part somebody added on mouser.com would be reported as our failure every time.
		QStringList rejected;
		for (const MouserCartLine& line : result.lines)
		{
			if (line.errorMessage.empty())
			{
				continue;
			}
			for (const MouserCartItemRequest& asked : plan.items)
			{
				if (asked.mouserPartNumber == line.mouserPartNumber)
				{
					rejected.append(tr("%1 — %2")
						.arg(QString::fromStdString(line.mouserPartNumber))
						.arg(QString::fromStdString(line.errorMessage)));
					break;
				}
			}
		}

		if (!rejected.isEmpty())
		{
			// The call succeeded and the cart exists, but not everything asked for went in. `ok`
			// alone would have reported this as a clean success.
			QMessageBox::warning(this, tr("The cart was staged, but Mouser rejected some lines"),
				tr("The cart exists and the rest of the order is in it. Mouser rejected:\n\n%1")
					.arg(rejected.join(QStringLiteral("\n"))));
		}
		else
		{
			// What we asked for, not result.lines.size() — the response carries the whole cart,
			// so anything already in it would be counted as though this staging had put it there.
			QMessageBox::information(this, tr("Staged to your Mouser cart"),
				tr("%n line(s) staged. Open the cart on mouser.com to review and place the order — "
				   "PartManager never checks out for you.", "", static_cast<int>(plan.items.size())));
		}
		reload();
	}

	void OrderManagerDialog::openCartOnMouser()
	{
		MouserOrder order;
		if (selectedOrderId() == NoOrderId || !m_controller.load(selectedOrderId(), order))
		{
			return;
		}
		QDesktopServices::openUrl(QUrl(QString::fromStdString(
			MouserCartClient::cartUrl(order.mouserCartId))));
	}

	void OrderManagerDialog::markSubmitted()
	{
		const int orderId = selectedOrderId();
		if (orderId == NoOrderId)
		{
			return;
		}

		MouserOrder order;
		m_controller.load(orderId, order);
		bool accepted = false;
		const QString number = QInputDialog::getText(this, tr("Mark as submitted"),
			tr("Mouser order number (optional):"), QLineEdit::Normal,
			QString::fromStdString(order.mouserOrderNumber), &accepted);
		if (!accepted)
		{
			return;
		}
		order.mouserOrderNumber = number.trimmed().toStdString();
		m_controller.save(order);
		m_controller.setStatus(orderId, OrderStatus::Submitted);
		reload();
	}

	void OrderManagerDialog::confirmArrival()
	{
		const OrderLine* line = selectedLine();
		if (line == nullptr)
		{
			return;
		}

		bool accepted = false;
		// The running total, not an increment — confirming "10 arrived" twice must not restock
		// twice, and the only way to make that obvious is to ask for the total.
		const int received = QInputDialog::getInt(this, tr("Confirm arrival"),
			tr("How many of “%1” have arrived in total?\n"
			   "(%2 ordered, %3 already booked in)")
				.arg(QString::fromStdString(line->partName))
				.arg(line->item.quantityOrdered)
				.arg(line->item.quantityReceived),
			line->item.quantityOrdered, 0, 1000000, 1, &accepted);
		if (!accepted)
		{
			return;
		}

		double unitCost = line->item.unitPrice;
		if (received > line->item.quantityReceived)
		{
			// §3: what was actually paid is its own observation, and only the user knows it —
			// Mouser's list price and the price on the invoice are different numbers.
			unitCost = QInputDialog::getDouble(this, tr("Confirm arrival"),
				tr("Unit price actually paid (0 to skip):"), unitCost, 0.0, 1000000.0, 4, &accepted);
			if (!accepted)
			{
				return;
			}
		}

		if (!m_controller.receive(line->item.id, received, unitCost,
			line->item.currency.empty() ? std::string() : line->item.currency))
		{
			QMessageBox::warning(this, tr("Could not record the arrival"),
				tr("The database rejected the update."));
		}
		reload();
	}

	void OrderManagerDialog::closeOrder()
	{
		const int orderId = selectedOrderId();
		if (orderId == NoOrderId)
		{
			return;
		}

		int outstanding = 0;
		for (const OrderLine& line : m_lines)
		{
			if (line.item.quantityReceived < line.item.quantityOrdered)
			{
				++outstanding;
			}
		}
		if (outstanding > 0 && QMessageBox::question(this, tr("Close this order?"),
			tr("%n line(s) have not fully arrived and will be marked backordered. "
			   "Stock is not changed — only what you confirmed as arrived was ever booked in.",
			   "", outstanding),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		m_controller.close(orderId);
		reload();
	}

	void OrderManagerDialog::deleteSelected()
	{
		const int orderId = selectedOrderId();
		if (orderId == NoOrderId)
		{
			return;
		}
		if (QMessageBox::question(this, tr("Delete this order?"),
			tr("Order #%1 and its lines will be deleted. Parts that already arrived stay in "
			   "stock — their stock entries are not undone.").arg(orderId),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		m_controller.remove(orderId);
		reload();
	}

}
