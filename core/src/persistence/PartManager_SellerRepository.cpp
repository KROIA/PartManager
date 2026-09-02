#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_SqlLiteral.h"
#include "PartManager_global.h"

#include <cstdlib>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		const char* const SellerColumns = "id,name,api_type";
		const char* const LinkColumns = "id,part_id,seller_id,seller_part_number,url,is_primary";
		const char* const PriceColumns =
			"id,part_seller_link_id,observed_at,quantity_break,unit_price,currency,source";

		Seller rowToSeller(const std::vector<std::string>& row)
		{
			Seller seller;
			seller.id = std::atoi(row[0].c_str());
			seller.name = row[1];
			seller.apiType = row[2];
			return seller;
		}

		PartSellerLink rowToLink(const std::vector<std::string>& row)
		{
			PartSellerLink link;
			link.id = std::atoi(row[0].c_str());
			link.partId = std::atoi(row[1].c_str());
			link.sellerId = std::atoi(row[2].c_str());
			link.sellerPartNumber = row[3];
			link.url = row[4];
			link.isPrimary = std::atoi(row[5].c_str()) != 0;
			return link;
		}

		PriceObservation rowToObservation(const std::vector<std::string>& row)
		{
			PriceObservation observation;
			observation.id = std::atoi(row[0].c_str());
			observation.partSellerLinkId = std::atoi(row[1].c_str());
			observation.observedAt = row[2];
			observation.quantityBreak = std::atoi(row[3].c_str());
			observation.unitPrice = std::atof(row[4].c_str());
			observation.currency = row[5];
			observation.source = row[6];
			return observation;
		}
	}

	bool SellerRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = db.execute(
			"CREATE TABLE IF NOT EXISTS seller ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL,"
			"api_type TEXT NOT NULL DEFAULT 'manual'"
			");");
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_seller_link ("
			"id INTEGER PRIMARY KEY,"
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"seller_id INTEGER NOT NULL REFERENCES seller(id),"
			"seller_part_number TEXT,"
			"url TEXT,"
			"is_primary INTEGER NOT NULL DEFAULT 0"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS price_history ("
			"id INTEGER PRIMARY KEY,"
			"part_seller_link_id INTEGER NOT NULL REFERENCES part_seller_link(id),"
			"observed_at TEXT NOT NULL DEFAULT (datetime('now')),"
			"quantity_break INTEGER NOT NULL,"
			"unit_price REAL NOT NULL,"
			"currency TEXT NOT NULL DEFAULT 'EUR',"
			"source TEXT NOT NULL"
			");") && ok;
		// Every read of a link or a price starts from a part or a link id, and both tables grow
		// one row per part per search — without these the order view degrades into a table scan
		// per line.
		ok = db.execute("CREATE INDEX IF NOT EXISTS idx_part_seller_link_part "
			"ON part_seller_link(part_id);") && ok;
		ok = db.execute("CREATE INDEX IF NOT EXISTS idx_price_history_link "
			"ON price_history(part_seller_link_id);") && ok;
		return ok;
	}

	int SellerRepository::ensureSeller(SQLiteWrapper::SQLite& db, const std::string& name,
		const std::string& apiType)
	{
		if (name.empty())
		{
			return NoSellerId;
		}
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id FROM seller WHERE name=" + sqlLiteral(name) + ";");
		if (!rows.empty() && !rows.front().empty())
		{
			return std::atoi(rows.front().front().c_str());
		}
		const bool ok = db.executeWithParams("INSERT INTO seller (name, api_type) VALUES (?, ?);",
			{ name, apiType.empty() ? std::string(SellerApiType::Manual) : apiType });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoSellerId;
	}

	int SellerRepository::ensureMouserSeller(SQLiteWrapper::SQLite& db)
	{
		return ensureSeller(db, MouserSellerName, SellerApiType::Mouser);
	}

	std::vector<Seller> SellerRepository::listSellers(SQLiteWrapper::SQLite& db)
	{
		std::vector<Seller> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + SellerColumns + " FROM seller ORDER BY name;"))
		{
			result.push_back(rowToSeller(row));
		}
		return result;
	}

	bool SellerRepository::findSeller(SQLiteWrapper::SQLite& db, int sellerId, Seller& outSeller)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			std::string("SELECT ") + SellerColumns + " FROM seller WHERE id="
			+ std::to_string(sellerId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outSeller = rowToSeller(rows.front());
		return true;
	}

	int SellerRepository::linkPart(SQLiteWrapper::SQLite& db, const PartSellerLink& link)
	{
		if (link.partId == 0 || link.sellerId == NoSellerId)
		{
			return NoPartSellerLinkId;
		}

		// Upsert, not insert — see the header note. The triple is the natural key: the same part
		// at the same seller under the same article number is one fact, however often it is seen.
		int linkId = NoPartSellerLinkId;
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id FROM part_seller_link WHERE part_id=" + std::to_string(link.partId)
			+ " AND seller_id=" + std::to_string(link.sellerId)
			+ " AND IFNULL(seller_part_number,'')=" + sqlLiteral(link.sellerPartNumber) + ";");
		if (!rows.empty() && !rows.front().empty())
		{
			linkId = std::atoi(rows.front().front().c_str());
			if (!db.executeWithParams("UPDATE part_seller_link SET url=?, is_primary=? WHERE id=?;",
				{ link.url, link.isPrimary ? "1" : "0", std::to_string(linkId) }))
			{
				return NoPartSellerLinkId;
			}
		}
		else
		{
			if (!db.executeWithParams(
				"INSERT INTO part_seller_link (part_id, seller_id, seller_part_number, url, is_primary) "
				"VALUES (?, ?, ?, ?, ?);",
				{ std::to_string(link.partId), std::to_string(link.sellerId), link.sellerPartNumber,
				  link.url, link.isPrimary ? "1" : "0" }))
			{
				return NoPartSellerLinkId;
			}
			linkId = static_cast<int>(db.getLastInsertRowId());
		}

		if (link.isPrimary)
		{
			// "Primary" has to stay a single answer, so promoting one link demotes the rest.
			db.executeWithParams("UPDATE part_seller_link SET is_primary=0 WHERE part_id=? AND id<>?;",
				{ std::to_string(link.partId), std::to_string(linkId) });
		}
		return linkId;
	}

	bool SellerRepository::removeLink(SQLiteWrapper::SQLite& db, int linkId)
	{
		// The price observations go with it: PRAGMA foreign_keys is never on, so nothing else
		// would clean them up and they would point at an id that no longer resolves.
		const std::string id = std::to_string(linkId);
		db.executeWithParams("DELETE FROM price_history WHERE part_seller_link_id=?;", { id });
		return db.executeWithParams("DELETE FROM part_seller_link WHERE id=?;", { id });
	}

	std::vector<PartSellerLink> SellerRepository::linksForPart(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<PartSellerLink> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + LinkColumns + " FROM part_seller_link WHERE part_id="
			+ std::to_string(partId) + " ORDER BY is_primary DESC, id;"))
		{
			result.push_back(rowToLink(row));
		}
		return result;
	}

	std::vector<PartSellerLink> SellerRepository::allLinks(SQLiteWrapper::SQLite& db)
	{
		std::vector<PartSellerLink> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + LinkColumns + " FROM part_seller_link ORDER BY id;"))
		{
			result.push_back(rowToLink(row));
		}
		return result;
	}

	bool SellerRepository::primaryLink(SQLiteWrapper::SQLite& db, int partId, PartSellerLink& outLink)
	{
		// linksForPart() already sorts is_primary first, so the front row is the answer whether
		// or not anything is flagged — which is what "its only link" means for a part with one.
		const std::vector<PartSellerLink> links = linksForPart(db, partId);
		if (links.empty())
		{
			return false;
		}
		outLink = links.front();
		return true;
	}

	std::string SellerRepository::mouserPartNumber(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT l.seller_part_number FROM part_seller_link l "
			"JOIN seller s ON s.id=l.seller_id "
			"WHERE l.part_id=" + std::to_string(partId) + " AND s.api_type='mouser' "
			"ORDER BY l.is_primary DESC, l.id LIMIT 1;");
		if (rows.empty() || rows.front().empty())
		{
			return std::string();
		}
		return rows.front().front();
	}

	int SellerRepository::recordQuote(SQLiteWrapper::SQLite& db, int linkId,
		const std::vector<PriceObservation>& observations)
	{
		if (linkId == NoPartSellerLinkId)
		{
			return 0;
		}
		int written = 0;
		for (const PriceObservation& observation : observations)
		{
			if (db.executeWithParams(
				"INSERT INTO price_history (part_seller_link_id, quantity_break, unit_price, "
				"currency, source) VALUES (?, ?, ?, ?, ?);",
				{ std::to_string(linkId), std::to_string(observation.quantityBreak),
				  std::to_string(observation.unitPrice),
				  observation.currency.empty() ? std::string("EUR") : observation.currency,
				  observation.source.empty() ? std::string(PriceSource::MouserApiQuote) : observation.source }))
			{
				++written;
			}
		}
		return written;
	}

	int SellerRepository::recordPaidPrice(SQLiteWrapper::SQLite& db, int linkId, double unitPrice,
		const std::string& currency, int quantityBreak)
	{
		PriceObservation observation;
		observation.quantityBreak = quantityBreak < 1 ? 1 : quantityBreak;
		observation.unitPrice = unitPrice;
		observation.currency = currency;
		observation.source = PriceSource::Paid;
		return recordQuote(db, linkId, { observation });
	}

	std::vector<PriceObservation> SellerRepository::priceHistory(SQLiteWrapper::SQLite& db, int linkId)
	{
		std::vector<PriceObservation> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + PriceColumns + " FROM price_history WHERE part_seller_link_id="
			+ std::to_string(linkId) + " ORDER BY observed_at DESC, id DESC;"))
		{
			result.push_back(rowToObservation(row));
		}
		return result;
	}

	std::vector<PriceObservation> SellerRepository::priceHistoryForPart(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<PriceObservation> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT h.id,h.part_seller_link_id,h.observed_at,h.quantity_break,h.unit_price,"
			"h.currency,h.source FROM price_history h "
			"JOIN part_seller_link l ON l.id=h.part_seller_link_id "
			"WHERE l.part_id=" + std::to_string(partId) + " ORDER BY h.observed_at DESC, h.id DESC;"))
		{
			result.push_back(rowToObservation(row));
		}
		return result;
	}

	bool SellerRepository::lastPrice(SQLiteWrapper::SQLite& db, int partId, const std::string& source,
		double& outUnitPrice, std::string& outCurrency)
	{
		std::string sql =
			"SELECT h.unit_price,h.currency FROM price_history h "
			"JOIN part_seller_link l ON l.id=h.part_seller_link_id "
			"WHERE l.part_id=" + std::to_string(partId);
		if (!source.empty())
		{
			sql += " AND h.source=" + sqlLiteral(source);
		}
		sql += " ORDER BY h.observed_at DESC, h.id DESC LIMIT 1;";

		std::vector<std::vector<std::string>> rows = db.fetchAll(sql);
		if (rows.empty() || rows.front().size() < 2)
		{
			return false;
		}
		outUnitPrice = std::atof(rows.front()[0].c_str());
		outCurrency = rows.front()[1];
		return true;
	}

#endif

}
