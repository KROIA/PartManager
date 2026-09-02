// @file PartManager_CartStagingDialog.h
// @brief Review what goes into the Mouser cart before anything is sent (§4, §6).
//
// The cart is on the user's real Mouser account and may already hold parts —
// from an earlier staging of this order, or from something they added on
// mouser.com themselves. Sending blindly is how someone ends up with double the
// resistors and no idea which step did it. So every staging goes through this:
// what the order needs, what the cart already holds, and one decision per line.
//
// Three actions per line, and the difference matters because the two Mouser
// endpoints genuinely behave differently (verified live 2026-09-02):
//   * **Set to N** — `/cart/items/update`, the quantity becomes N. Idempotent,
//     and the right default: re-staging an order should not accumulate.
//   * **Add N** — `/cart/items/insert`, N is added to what is there.
//   * **Skip** — the line is not sent at all, and whatever is in the cart stays.
//
// A mixed selection is sent as **two calls**, sets first then adds, because one
// request cannot carry both semantics.
// @see docs/design/ARCHITECTURE.md §4, §6
// @see PartManager_OrderController.h, PartManager_MouserCartClient.h
#pragma once

#include "controllers/PartManager_OrderController.h"
#include <QDialog>
#include <vector>

class QLabel;
class QTableWidget;

namespace PartManager
{

	// One reviewed line: what the order wants, what the cart holds, what the user decided.
	struct CartStagingRow
	{
		std::string mouserPartNumber;
		std::string partName;
		int inCart = 0;          // quantity already in the Mouser cart, 0 when it is not there
		int outstanding = 0;     // what the order still needs (ordered minus received)
		int quantity = 0;        // what the user settled on
		bool add = false;        // true = add to the cart, false = set the cart to `quantity`
		bool skip = false;
	};

	class CartStagingDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `plan` is what the order wants; `cart` is the cart as it stands right now (an empty
		// result simply means nothing is in it yet).
		CartStagingDialog(const StagingPlan& plan, const MouserCartResult& cart,
			const std::vector<OrderLine>& lines, QWidget* parent = nullptr);

		// The lines to send with `/cart/items/update` (set) and `/cart/items/insert` (add).
		// Either may be empty; both empty means the user chose to send nothing.
		std::vector<MouserCartItemRequest> itemsToSet() const;
		std::vector<MouserCartItemRequest> itemsToAdd() const;

		// True when the user asked to abandon the current cart and start a fresh one.
		bool wantsNewCart() const;

	private slots:
		void onCellChanged(int row, int column);
		void setAllTo(int actionIndex);
		void requestNewCart();
		void updateSummary();

	private:
		void fill();

		std::vector<CartStagingRow> m_rows;
		QTableWidget* m_table;
		QLabel* m_summary;
		bool m_newCart = false;
		bool m_populating = false;
	};

}
