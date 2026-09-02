// @file PartManager_Seller.h
// @brief Plain data classes for `seller` / `part_seller_link` / `price_history` (§3).
//
// Zero Qt, zero SQL, zero business logic. These three rows are what lets a part
// remember where it came from: item 7 could prefill a part from a Mouser hit but
// then threw the `ProductDetailUrl` away, so nothing on the part pointed back at
// the page it was created from. A `PartSellerLink` is that pointer, and it is
// also the only place the **Mouser part number** lives — `part.mpn` is the
// *manufacturer's* number, which the Cart API does not accept.
//
// `PriceObservation` separates the two prices §3 insists are different things:
// `mouser_api_quote` is a list price we saw, `paid` is what the user actually
// handed over. Averaging them together would quietly corrupt the "value of
// wealth" figure, so the `source` column stays and every reader filters on it.
// @see docs/design/ARCHITECTURE.md §3, §6
// @see PartManager_SellerRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinels meaning "not yet inserted" / "none".
	constexpr int NoSellerId = 0;
	constexpr int NoPartSellerLinkId = 0;

	// `seller.api_type` vocabulary. TEXT in the schema; these catch typos at the call site the
	// same way StockReason and PartFileRole do.
	namespace SellerApiType
	{
		constexpr const char* Mouser = "mouser";
		constexpr const char* Manual = "manual";
		constexpr const char* Other = "other";
	}

	// The one seller PartManager talks to over an API. Its row is created on demand.
	constexpr const char* MouserSellerName = "Mouser";

	// `price_history.source` vocabulary (§3).
	namespace PriceSource
	{
		constexpr const char* MouserApiQuote = "mouser_api_quote";  // list price we observed
		constexpr const char* Paid = "paid";                        // what was actually paid
	}

	// One `seller` row.
	struct PART_MANAGER_API Seller
	{
		int id = NoSellerId;
		std::string name;                              // 'Mouser', 'Digikey', 'Local'
		std::string apiType = SellerApiType::Manual;
	};

	// One `part_seller_link` row — "this part is that seller's article number, at that URL".
	struct PART_MANAGER_API PartSellerLink
	{
		int id = NoPartSellerLinkId;
		int partId = 0;
		int sellerId = NoSellerId;
		std::string sellerPartNumber;   // Mouser P/N — NOT part.mpn; the Cart API needs this one
		std::string url;                // ProductDetailUrl, what "Open on Mouser" opens (§6)
		bool isPrimary = false;         // the link the order/cart path uses when a part has several
	};

	// One `price_history` row. A quote is per price break, so a single search result normally
	// appends several of these at once.
	struct PART_MANAGER_API PriceObservation
	{
		int id = 0;
		int partSellerLinkId = NoPartSellerLinkId;
		std::string observedAt;         // ISO-8601, DB-assigned on insert
		int quantityBreak = 1;          // 1 / 10 / 100 ...
		double unitPrice = 0.0;
		std::string currency = "EUR";
		std::string source = PriceSource::MouserApiQuote;
	};

}
