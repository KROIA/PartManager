// @file PartManager_OrderManagerDialog.h
// @brief The order view (`order-view.svg`, §4) — draft, stage, submit, confirm arrivals, close.
//
// One screen for the whole §4 checkout tail: the order list on top, the selected
// order's lines below, and the lifecycle buttons along the bottom. Arrival
// confirmation is per line and per quantity (orange -> green), which is what
// makes a partial shipment representable at all.
//
// **The quantity column is editable while an order is still a draft** and frozen
// afterwards: changing what you ordered once it is in a cart or submitted would
// make PartManager's record disagree with Mouser's, silently.
//
// **PartManager never checks out.** "Stage to Mouser Cart" builds the cart and
// stores its key; "Open Cart on Mouser" sends the user to the browser to pay.
// "Mark as Submitted" is the user telling PartManager they did.
// @see docs/design/ARCHITECTURE.md §4, §6, §12b
// @see PartManager_OrderController.h
#pragma once

#include "controllers/PartManager_OrderController.h"
#include <QDialog>
#include <vector>

namespace Ui { class OrderManagerDialog; }

namespace PartManager
{

	class OrderManagerDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit OrderManagerDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~OrderManagerDialog() override;

		// Selects one order on open — the draft the partlist editor just raised, so the user
		// lands on it instead of hunting for it in the list.
		void selectOrder(int orderId);

	private slots:
		void reload();
		void showLines();
		void updateButtons();
		void onQuantityEdited(int row, int column);
		void stageToCart();
		void openCartOnMouser();
		void markSubmitted();
		void confirmArrival();
		void closeOrder();
		void deleteSelected();

	private:
		int selectedOrderId() const;
		const OrderLine* selectedLine() const;

		Ui::OrderManagerDialog* m_ui;
		OrderController m_controller;
		std::vector<MouserOrder> m_orders;
		std::vector<OrderLine> m_lines;
		// Guards showLines()' own setItem() calls against the cellChanged handler, which would
		// otherwise read a half-built row and write it straight back to the database.
		bool m_populating = false;
	};

}
