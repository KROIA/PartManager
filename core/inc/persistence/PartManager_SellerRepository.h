// @file PartManager_SellerRepository.h
// @brief CRUD for `seller` / `part_seller_link` / `price_history` (§3).
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartRepository/PartlistRepository.
//
// `linkPart()` is an **upsert on (part_id, seller_id, seller_part_number)**, not
// a plain insert: creating a part from a Mouser hit and then re-running the same
// search must not accumulate duplicate links for one article number. Re-linking
// the same triple refreshes the URL and the primary flag instead.
//
// `recordQuote()` appends one `price_history` row per price break. It does not
// deduplicate against the previous observation on purpose — "the price was the
// same on both days" is itself history, and a chart wants both points.
// @see docs/design/ARCHITECTURE.md §3, §6
// @see PartManager_Seller.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Seller.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API SellerRepository
	{
		SellerRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates seller/part_seller_link/price_history if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// Find-or-insert by name. Returns the seller id, NoSellerId on failure. `apiType` is only
		// used when the row has to be created — renaming an existing seller's API type is a
		// separate, deliberate act, not a side effect of looking it up.
		static int ensureSeller(SQLiteWrapper::SQLite& db, const std::string& name,
			const std::string& apiType);
		// Shorthand for the one seller with an API behind it (§6).
		static int ensureMouserSeller(SQLiteWrapper::SQLite& db);

		static std::vector<Seller> listSellers(SQLiteWrapper::SQLite& db);
		static bool findSeller(SQLiteWrapper::SQLite& db, int sellerId, Seller& outSeller);

		// Upsert on (part_id, seller_id, seller_part_number) — see the header note. Returns the
		// link id, NoPartSellerLinkId on failure. Setting isPrimary clears the flag on the part's
		// other links, so "primary" stays a single answer.
		static int linkPart(SQLiteWrapper::SQLite& db, const PartSellerLink& link);
		static bool removeLink(SQLiteWrapper::SQLite& db, int linkId);
		static std::vector<PartSellerLink> linksForPart(SQLiteWrapper::SQLite& db, int partId);
		// The part's primary link, or its only link when nothing is flagged. False when it has none.
		static bool primaryLink(SQLiteWrapper::SQLite& db, int partId, PartSellerLink& outLink);
		// The Mouser part number the Cart API needs (§6). Empty when the part has no Mouser link.
		static std::string mouserPartNumber(SQLiteWrapper::SQLite& db, int partId);

		// Appends one price_history row per break. Returns how many rows were written.
		static int recordQuote(SQLiteWrapper::SQLite& db, int linkId,
			const std::vector<PriceObservation>& observations);
		// The convenience shape the restock/order-close path uses: one `paid` row at qty 1.
		static int recordPaidPrice(SQLiteWrapper::SQLite& db, int linkId, double unitPrice,
			const std::string& currency, int quantityBreak = 1);

		// Newest first — the order a price chart or a "last seen" label wants.
		static std::vector<PriceObservation> priceHistory(SQLiteWrapper::SQLite& db, int linkId);
		// Every observation for a part across all its links, newest first.
		static std::vector<PriceObservation> priceHistoryForPart(SQLiteWrapper::SQLite& db, int partId);
		// The most recent unit price for a part, filtered by source ("" = any). Returns false when
		// there is none, which is different from a price of 0.
		static bool lastPrice(SQLiteWrapper::SQLite& db, int partId, const std::string& source,
			double& outUnitPrice, std::string& outCurrency);
#endif

	};

}
