#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_SqlLiteral.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// §3's seller/price tables. Two things here are easy to get wrong and expensive to notice late:
// re-linking the same article number must not pile up duplicate rows, and the Mouser part number
// must never be confused with `part.mpn` — the Cart API rejects the manufacturer's number.
class TST_SellerRepository : public UnitTest::Test
{
	TEST_CLASS(TST_SellerRepository)
public:
	TST_SellerRepository()
		: Test("TST_SellerRepository")
	{
		ADD_TEST(TST_SellerRepository::sqlLiteralDoublesEmbeddedQuotes);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_SellerRepository::relinkingTheSameArticleUpdatesInsteadOfDuplicating);
		ADD_TEST(TST_SellerRepository::primaryIsASingleAnswer);
		ADD_TEST(TST_SellerRepository::mouserNumberComesFromTheLinkNotTheMpn);
		ADD_TEST(TST_SellerRepository::quotedAndPaidPricesStaySeparate);
#endif
	}

private:

	// A name with an apostrophe is the case that turns a SELECT into a syntax error, and the
	// reason every text predicate in these repositories goes through sqlLiteral().
	TEST_FUNCTION(sqlLiteralDoublesEmbeddedQuotes)
	{
		TEST_START;

		TEST_COMPARE(PartManager::sqlLiteral("Mouser"), std::string("'Mouser'"));
		TEST_COMPARE(PartManager::sqlLiteral("O'Brien"), std::string("'O''Brien'"));
		TEST_COMPARE(PartManager::sqlLiteral(""), std::string("''"));
		// The classic injection payload has to end up inert inside one literal, not close it.
		TEST_COMPARE(PartManager::sqlLiteral("'; DROP TABLE part;--"),
			std::string("'''; DROP TABLE part;--'"));
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	static void createSchemas(SQLiteWrapper::SQLite& db)
	{
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::SellerRepository::createSchema(db);
	}

	static std::filesystem::path databasePath(const std::string& name)
	{
		std::filesystem::path path =
			std::filesystem::temp_directory_path() / ("PartManager_TST_SellerRepository_" + name + ".db");
		std::filesystem::remove(path);
		return path;
	}

	static int makePart(SQLiteWrapper::SQLite& db, const std::string& name, const std::string& mpn)
	{
		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		part.mpn = mpn;
		return PartManager::PartRepository::insertPart(db, part);
	}

	// Tests

	TEST_FUNCTION(relinkingTheSameArticleUpdatesInsteadOfDuplicating)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("relink").string());
		db.open();
		createSchemas(db);
		const int partId = makePart(db, "RC0603-4K7", "RC0603FR-074K7L");

		// ensureSeller is find-or-insert, so calling it twice must not make two Mousers.
		const int sellerId = PartManager::SellerRepository::ensureMouserSeller(db);
		TEST_ASSERT(sellerId != PartManager::NoSellerId);
		TEST_COMPARE(PartManager::SellerRepository::ensureMouserSeller(db), sellerId);
		TEST_COMPARE(PartManager::SellerRepository::listSellers(db).size(), static_cast<size_t>(1));

		PartManager::PartSellerLink link;
		link.partId = partId;
		link.sellerId = sellerId;
		link.sellerPartNumber = "603-RC0603FR-074K7L";
		link.url = "https://www.mouser.ch/ProductDetail/first";
		const int linkId = PartManager::SellerRepository::linkPart(db, link);
		TEST_ASSERT(linkId != PartManager::NoPartSellerLinkId);

		// Re-running the same search finds the same article again. That is one fact, not two.
		link.url = "https://www.mouser.ch/ProductDetail/second";
		TEST_COMPARE(PartManager::SellerRepository::linkPart(db, link), linkId);
		const std::vector<PartManager::PartSellerLink> links =
			PartManager::SellerRepository::linksForPart(db, partId);
		TEST_COMPARE(links.size(), static_cast<size_t>(1));
		TEST_COMPARE(links[0].url, std::string("https://www.mouser.ch/ProductDetail/second"));

		// A genuinely different article number for the same part is a second link, though.
		link.sellerPartNumber = "603-RC0603FR-074K7L-CUT";
		TEST_ASSERT(PartManager::SellerRepository::linkPart(db, link) != linkId);
		TEST_COMPARE(PartManager::SellerRepository::linksForPart(db, partId).size(),
			static_cast<size_t>(2));
	}

	TEST_FUNCTION(primaryIsASingleAnswer)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("primary").string());
		db.open();
		createSchemas(db);
		const int partId = makePart(db, "RC0603-4K7", "RC0603FR-074K7L");
		const int sellerId = PartManager::SellerRepository::ensureMouserSeller(db);

		PartManager::PartSellerLink first;
		first.partId = partId;
		first.sellerId = sellerId;
		first.sellerPartNumber = "AAA";
		first.isPrimary = true;
		const int firstId = PartManager::SellerRepository::linkPart(db, first);

		PartManager::PartSellerLink second = first;
		second.sellerPartNumber = "BBB";
		const int secondId = PartManager::SellerRepository::linkPart(db, second);

		// Promoting the second must demote the first, or "primary" stops being one answer.
		const std::vector<PartManager::PartSellerLink> links =
			PartManager::SellerRepository::linksForPart(db, partId);
		TEST_COMPARE(links.size(), static_cast<size_t>(2));
		int primaries = 0;
		for (const PartManager::PartSellerLink& link : links)
		{
			if (link.isPrimary)
			{
				++primaries;
			}
		}
		TEST_COMPARE(primaries, 1);

		PartManager::PartSellerLink chosen;
		TEST_ASSERT(PartManager::SellerRepository::primaryLink(db, partId, chosen));
		TEST_COMPARE(chosen.id, secondId);
		PM_UNUSED(firstId);

		// A part with no link at all must say so rather than hand back a default-constructed one.
		PartManager::PartSellerLink none;
		TEST_ASSERT_M(!PartManager::SellerRepository::primaryLink(db, 9999, none),
			"a part with no links must report none");
	}

	TEST_FUNCTION(mouserNumberComesFromTheLinkNotTheMpn)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("mouserpn").string());
		db.open();
		createSchemas(db);
		const int partId = makePart(db, "RC0603-4K7", "RC0603FR-074K7L");

		// No link yet: the answer is "none", not the manufacturer's number. Handing back the mpn
		// here would build a cart Mouser rejects, one line at a time, with no obvious cause.
		TEST_ASSERT_M(PartManager::SellerRepository::mouserPartNumber(db, partId).empty(),
			"an unlinked part must have no Mouser part number");

		// A non-Mouser seller must not answer for Mouser either.
		const int localId = PartManager::SellerRepository::ensureSeller(db, "Local drawer",
			PartManager::SellerApiType::Manual);
		PartManager::PartSellerLink localLink;
		localLink.partId = partId;
		localLink.sellerId = localId;
		localLink.sellerPartNumber = "drawer-7";
		PartManager::SellerRepository::linkPart(db, localLink);
		TEST_ASSERT_M(PartManager::SellerRepository::mouserPartNumber(db, partId).empty(),
			"a manual seller link must not answer as a Mouser one");

		PartManager::PartSellerLink mouserLink;
		mouserLink.partId = partId;
		mouserLink.sellerId = PartManager::SellerRepository::ensureMouserSeller(db);
		mouserLink.sellerPartNumber = "603-RC0603FR-074K7L";
		PartManager::SellerRepository::linkPart(db, mouserLink);
		TEST_COMPARE(PartManager::SellerRepository::mouserPartNumber(db, partId),
			std::string("603-RC0603FR-074K7L"));
	}

	TEST_FUNCTION(quotedAndPaidPricesStaySeparate)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("prices").string());
		db.open();
		createSchemas(db);
		const int partId = makePart(db, "RC0603-4K7", "RC0603FR-074K7L");

		PartManager::PartSellerLink link;
		link.partId = partId;
		link.sellerId = PartManager::SellerRepository::ensureMouserSeller(db);
		link.sellerPartNumber = "603-RC0603FR-074K7L";
		const int linkId = PartManager::SellerRepository::linkPart(db, link);

		// One search result is several price breaks, and all of them are one observation.
		std::vector<PartManager::PriceObservation> quote;
		for (int quantity : { 1, 10, 100 })
		{
			PartManager::PriceObservation observation;
			observation.quantityBreak = quantity;
			observation.unitPrice = 1.0 / quantity;
			observation.currency = "CHF";
			quote.push_back(observation);
		}
		TEST_COMPARE(PartManager::SellerRepository::recordQuote(db, linkId, quote), 3);
		TEST_COMPARE(PartManager::SellerRepository::priceHistory(db, linkId).size(),
			static_cast<size_t>(3));

		PartManager::SellerRepository::recordPaidPrice(db, linkId, 0.31, "CHF");

		// §3's whole point: what Mouser lists and what the user paid are different facts, and a
		// reader has to be able to ask for one without averaging in the other.
		double price = 0.0;
		std::string currency;
		TEST_ASSERT(PartManager::SellerRepository::lastPrice(db, partId,
			PartManager::PriceSource::Paid, price, currency));
		TEST_COMPARE(currency, std::string("CHF"));
		TEST_ASSERT_M(price > 0.30 && price < 0.32, "the paid price must come back, not a quote");

		TEST_COMPARE(PartManager::SellerRepository::priceHistoryForPart(db, partId).size(),
			static_cast<size_t>(4));

		// A part nobody ever priced reports no price — which is not the same as a price of 0.
		double none = 0.0;
		std::string noCurrency;
		TEST_ASSERT_M(!PartManager::SellerRepository::lastPrice(db, 9999, "", none, noCurrency),
			"an unpriced part must report no price rather than zero");
	}

#endif
};

TEST_INSTANTIATE(TST_SellerRepository);
