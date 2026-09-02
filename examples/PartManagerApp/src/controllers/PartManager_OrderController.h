// @file PartManager_OrderController.h
// @brief Widget-free logic behind the order view (§4, §6, §12b).
//
// Same split as the other controllers: the pure parts (status wording, the
// summary line, the "can this be staged" verdict) are free functions unit-tested
// in TST_OrderController; the controller class is the thin OrderRepository +
// MouserCartClient wrapper the dialog talks to.
//
// **`stageToCart()` is the one call here that reaches the network**, and it is
// the one that has never run: `MOUSER_CART_API` was not set when this was
// written. Everything it depends on — the request body, the response parsing,
// the per-line rejection check — is covered offline in TST_MouserCartClient, and
// the manual checklist for the live path is in `.claude/PROJECT_STATUS.md`.
// It blocks the calling thread for up to the client's timeout, same ceiling as
// the Mouser search path already accepted.
// @see docs/design/ARCHITECTURE.md §4, §6, §12b
// @see PartManager_OrderRepository.h, PartManager_MouserCartClient.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "mouser/PartManager_MouserCartClient.h"
#include "persistence/PartManager_OrderRepository.h"
#include <QString>
#include <string>
#include <vector>

namespace PartManager
{

	// Display text for a `mouser_order.status`. App chrome, so tr()'d; an unknown/legacy string is
	// shown unchanged rather than dropped.
	QString orderStatusLabel(const std::string& status);
	// Display text for a `mouser_order_item.status`.
	QString orderItemStatusLabel(const std::string& status);

	// The one-line state of an order's lines, for the dialog's footer: how many arrived, how many
	// are still out, and how many cannot be staged for want of a Mouser article number.
	QString orderStatusSummary(const std::vector<OrderLine>& lines);

	// What actually gets sent to the Cart API, and why some lines did not make it. Splitting this
	// out of stageToCart() is what makes "the cart came back two lines short" explainable.
	struct StagingPlan
	{
		std::vector<MouserCartItemRequest> items;
		std::vector<std::string> skippedPartNames;   // no Mouser article number, so unorderable
	};
	StagingPlan planStaging(const std::vector<OrderLine>& lines);

	// OrderRepository wrapper shared by OrderManagerDialog and the partlist editor's
	// "Order shortfall" action. Holds the caller's DatabaseHandle without owning it.
	class OrderController
	{
	public:
		explicit OrderController(DatabaseHandle* handle);

		std::vector<MouserOrder> orders(bool openOnly = false) const;
		bool load(int orderId, MouserOrder& outOrder) const;
		std::vector<OrderLine> lines(int orderId) const;
		int itemCount(int orderId) const;
		bool save(const MouserOrder& order) const;
		bool remove(int orderId) const;

		// §4 checkout: what this partlist is short of, and the draft order that follows from it.
		OrderDraftPreview previewFromPartlist(int partlistId) const;
		int createDraftFromPartlist(int partlistId) const;

		// Editing a draft's quantities before staging — the per-line qty §4 calls editable.
		bool saveItems(int orderId, const std::vector<MouserOrderItem>& items) const;

		bool setStatus(int orderId, const std::string& status) const;
		// Confirms arrivals for one line. `totalReceived` is the running total, not an increment.
		bool receive(int orderItemId, int totalReceived, double unitCost,
			const std::string& currency) const;
		bool close(int orderId) const;

		// True when MOUSER_CART_API is set. The dialog disables staging and says why when it is
		// not, rather than letting the user press a button that can only fail.
		static bool canStage();

		// Builds the real Mouser cart from the order's lines and stores the CartKey (§6). Adds to
		// the order's existing cart when it already has one. Returns the raw result so the caller
		// can report a per-line rejection, which `ok` alone does not cover.
		MouserCartResult stageToCart(int orderId) const;

	private:
		DatabaseHandle* m_handle;
	};

}
