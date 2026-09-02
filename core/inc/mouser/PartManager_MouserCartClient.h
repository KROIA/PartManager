// @file PartManager_MouserCartClient.h
// @brief Mouser Cart API v1 client (§6) — stages an order's lines into a real mouser.com cart.
//
// Grounded on `.claude/MouserAPI/MouserAPI_V1.json`: `POST /api/v1/cart/items/insert`
// (`MouserCart_AddCartItems`) with a `CartItemRequestRoot`
// (`{ CartKey?, CartItems:[{ MouserPartNumber, Quantity }] }`) and a
// `CartResponseRoot` back (`CartKey`, `CartItems[OrderLine]`, `MerchandiseTotal`,
// `CurrencyCode`, `Errors[]`). Same as the Search API, failures arrive inside a
// 200 body, so an HTTP 200 alone never means success — and here it is worse,
// because a per-line `Errors[]` can reject one part while the cart itself
// succeeds. `parseCartResponse()` surfaces both.
//
// **This is a separate key from the Search API.** It is read at runtime from
// `MOUSER_CART_API` and from nowhere else — never a literal, never a settings
// file, never logged. When it is unset every request fails immediately with a
// message that does not contain the key or any part of it.
//
// **PartManager never checks out.** The cart is built and its `CartKey` stored;
// the user goes to mouser.com to review and pay. Nothing here touches the Order
// API's submit endpoint, and nothing here can spend money.
//
// **Not yet live-tested.** `MOUSER_CART_API` was not present in the environment
// when this was written, so only `parseCartResponse()` has been exercised (with
// fixture JSON, offline). See `.claude/PROJECT_STATUS.md` for the checklist to
// run once the key is set.
// @see docs/design/ARCHITECTURE.md §6, §4
// @see PartManager_MouserClient.h, PartManager_OrderRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

#if QT_ENABLED
class QNetworkAccessManager;
#endif

namespace PartManager
{

	// One line to put in the cart. Quantity is what Mouser will actually price; it may be raised
	// by Mouser to a sales multiple, which is why the response carries its own quantity back.
	struct PART_MANAGER_API MouserCartItemRequest
	{
		std::string mouserPartNumber;   // seller article number — NOT part.mpn
		int quantity = 0;
	};

	// One line as Mouser priced it (`OrderLine` in the spec). `errorMessage` is that line's own
	// `Errors[]`: the cart can succeed while one part in it was rejected.
	struct PART_MANAGER_API MouserCartLine
	{
		std::string mouserPartNumber;
		std::string manufacturerPartNumber;
		std::string description;
		int quantity = 0;
		double unitPrice = 0.0;
		double extendedPrice = 0.0;
		std::string errorMessage;       // empty when the line was accepted
	};

	// Outcome of one cart call. `ok == false` covers the same four failure modes as the Search
	// API: missing key, network/timeout, non-200 status, and in-body `Errors[]` inside a 200.
	struct PART_MANAGER_API MouserCartResult
	{
		bool ok = false;
		std::string errorMessage;       // never contains the API key
		std::string cartKey;            // the UUID stored as mouser_order.mouser_cart_id
		std::string currencyCode;
		double merchandiseTotal = 0.0;
		int totalItemCount = 0;
		std::vector<MouserCartLine> lines;

		// True when the cart came back fine but at least one line inside it did not — the case a
		// plain `ok` check would sail straight past.
		bool hasRejectedLines() const;
	};

	class PART_MANAGER_API MouserCartClient
	{
	public:
		// The only place the key ever comes from. Deliberately not the Search API's key.
		static const char* const ApiKeyEnvVar;

		// True when MOUSER_CART_API is set and non-empty. Does not expose the value.
		static bool hasApiKey();

		// Pure response handling, no network, no key — the offline-testable half, and currently
		// the only half that has been tested at all.
		static MouserCartResult parseCartResponse(const std::string& json);
		// Request-body construction, split out for the same reason. An empty `cartKey` asks
		// Mouser to create a new cart; passing an existing one adds to it.
		static std::string buildInsertBody(const std::string& cartKey,
			const std::vector<MouserCartItemRequest>& items);

		MouserCartClient();
		~MouserCartClient();
		MouserCartClient(const MouserCartClient&) = delete;
		MouserCartClient& operator=(const MouserCartClient&) = delete;

		void setTimeoutMs(int timeoutMs);
		int timeoutMs() const;

		// POST /api/v1/cart/items/insert — **adds to** whatever quantity is already in the cart.
		// Verified live 2026-09-02: inserting qty 5 twice leaves qty 10 on one line. Use this
		// only to fill a cart that PartManager did not already stage into, or re-staging an
		// order doubles it.
		MouserCartResult insertItems(const std::string& cartKey,
			const std::vector<MouserCartItemRequest>& items);

		// POST /api/v1/cart/items/update — **sets** the quantity outright. Verified live
		// 2026-09-02: updating a line holding 10 to 3 leaves 3, not 13. This is what makes
		// re-staging an order idempotent, which is the behaviour the order view promises.
		MouserCartResult updateItems(const std::string& cartKey,
			const std::vector<MouserCartItemRequest>& items);
		// GET /api/v1/cart?cartKey=... — reads a cart back, for refreshing an order's prices.
		MouserCartResult readCart(const std::string& cartKey);

		// The mouser.com page for a staged cart, for the "Open cart on Mouser" button. Empty for
		// an empty key.
		static std::string cartUrl(const std::string& cartKey);

	private:
		MouserCartResult send(const std::string& endpointPath, const std::string& jsonBody,
			bool isPost);

		int m_timeoutMs = 15000;
#if QT_ENABLED
		QNetworkAccessManager* m_network = nullptr;
#endif
	};

}
