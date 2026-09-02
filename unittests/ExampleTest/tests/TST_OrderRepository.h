#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_OrderRepository.h"
#include "persistence/PartManager_PartlistRepository.h"
#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// §4's order lifecycle. The two cases worth the most here are the ones a plain CRUD test would
// miss: a BOM that lists the same part on several lines must be ordered **once** for the combined
// shortfall (per-line shortfalls cannot be added), and confirming the same arrival twice must not
// restock twice.
class TST_OrderRepository : public UnitTest::Test
{
	TEST_CLASS(TST_OrderRepository)
public:
	TST_OrderRepository()
		: Test("TST_OrderRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_OrderRepository::draftTakesOnlyTheShortfall);
		ADD_TEST(TST_OrderRepository::onePartOnSeveralBomLinesIsOrderedOnce);
		ADD_TEST(TST_OrderRepository::arrivalsRestockExactlyOnce);
		ADD_TEST(TST_OrderRepository::closingBackordersWhatNeverArrived);
		ADD_TEST(TST_OrderRepository::linesWithoutAMouserNumberAreNotStageable);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	static void createSchemas(SQLiteWrapper::SQLite& db)
	{
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::StockRepository::createSchema(db);
		PartManager::PartlistRepository::createSchema(db);
		PartManager::SellerRepository::createSchema(db);
		PartManager::OrderRepository::createSchema(db);
	}

	static std::filesystem::path databasePath(const std::string& name)
	{
		std::filesystem::path path =
			std::filesystem::temp_directory_path() / ("PartManager_TST_OrderRepository_" + name + ".db");
		std::filesystem::remove(path);
		return path;
	}

	static int makeType(SQLiteWrapper::SQLite& db)
	{
		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		return PartManager::PartTypeRepository::insertType(db, type);
	}

	// A part with stock written through the log, plus a Mouser link so it is stageable.
	static int makePart(SQLiteWrapper::SQLite& db, int typeId, const std::string& name, int stock,
		bool withMouserLink = true)
	{
		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		part.mpn = name;
		const int id = PartManager::PartRepository::insertPart(db, part);
		if (stock > 0)
		{
			PartManager::StockRepository::restock(db, id, stock, "test fixture");
		}
		if (withMouserLink)
		{
			PartManager::PartSellerLink link;
			link.partId = id;
			link.sellerId = PartManager::SellerRepository::ensureMouserSeller(db);
			link.sellerPartNumber = "603-" + name;
			link.isPrimary = true;
			PartManager::SellerRepository::linkPart(db, link);
		}
		return id;
	}

	static int makeList(SQLiteWrapper::SQLite& db, int multiplier)
	{
		PartManager::Partlist partlist;
		partlist.name = "PSU Rev C";
		partlist.multiplier = multiplier;
		return PartManager::PartlistRepository::insertPartlist(db, partlist);
	}

	// Tests

	TEST_FUNCTION(draftTakesOnlyTheShortfall)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("draft").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);

		const int shortId = makePart(db, typeId, "RC0603-4K7", 12);       // 15 needed, 3 short
		const int plentyId = makePart(db, typeId, "CAP-0805-100n", 45);   // 10 needed, none short
		const int listId = makeList(db, 5);

		std::vector<PartManager::PartlistItem> items;
		PartManager::PartlistItem resistor;
		resistor.partId = shortId;
		resistor.quantityPerUnit = 3;
		items.push_back(resistor);
		PartManager::PartlistItem capacitor;
		capacitor.partId = plentyId;
		capacitor.quantityPerUnit = 2;
		items.push_back(capacitor);
		PartManager::PartlistItem unresolved;   // partId stays NoPartId
		unresolved.quantityPerUnit = 1;
		items.push_back(unresolved);
		PartManager::PartlistRepository::saveItems(db, listId, items);

		const PartManager::OrderDraftPreview preview =
			PartManager::OrderRepository::previewFromPartlist(db, listId);
		// Only the part that is actually short, and the unresolved row counted rather than
		// silently dropped — it is the one the user has to fix before ordering.
		TEST_COMPARE(preview.lines.size(), static_cast<size_t>(1));
		TEST_COMPARE(preview.skippedUnresolved, 1);
		TEST_COMPARE(preview.lines[0].item.partId, shortId);
		TEST_COMPARE(preview.lines[0].item.quantityOrdered, 3);
		TEST_COMPARE(preview.lines[0].partName, std::string("RC0603-4K7"));
		TEST_COMPARE(preview.lines[0].mouserPartNumber, std::string("603-RC0603-4K7"));
		TEST_ASSERT(preview.lines[0].stageable);

		const int orderId = PartManager::OrderRepository::createDraftFromPartlist(db, listId);
		TEST_ASSERT_M(orderId != PartManager::NoOrderId, "createDraftFromPartlist failed");

		PartManager::MouserOrder order;
		TEST_ASSERT(PartManager::OrderRepository::findOrder(db, orderId, order));
		TEST_COMPARE(order.status, std::string(PartManager::OrderStatus::Draft));
		TEST_COMPARE(order.partlistId, listId);
		TEST_COMPARE(PartManager::OrderRepository::itemCount(db, orderId), 1);

		// A list with nothing short must not create an empty order to click through.
		const int fullListId = makeList(db, 1);
		std::vector<PartManager::PartlistItem> covered;
		PartManager::PartlistItem one;
		one.partId = plentyId;
		one.quantityPerUnit = 1;
		covered.push_back(one);
		PartManager::PartlistRepository::saveItems(db, fullListId, covered);
		TEST_COMPARE(PartManager::OrderRepository::createDraftFromPartlist(db, fullListId),
			PartManager::NoOrderId);
	}

	TEST_FUNCTION(onePartOnSeveralBomLinesIsOrderedOnce)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("dedupe").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);

		// 20 on the shelf; three positions need 10 each for one board, so the build needs 30.
		// Each line on its own looks covered (10 <= 20), which is exactly why per-line shortfalls
		// cannot be summed — doing so would order nothing and the build would come up 10 short.
		const int partId = makePart(db, typeId, "RC0603-4K7", 20);
		const int listId = makeList(db, 1);

		std::vector<PartManager::PartlistItem> items;
		for (const char* designator : { "R1", "R2", "R3" })
		{
			PartManager::PartlistItem item;
			item.partId = partId;
			item.designators = designator;
			item.quantityPerUnit = 10;
			items.push_back(item);
		}
		PartManager::PartlistRepository::saveItems(db, listId, items);

		// Every individual line reports no shortfall...
		for (const PartManager::PartlistLine& line : PartManager::PartlistRepository::lines(db, listId))
		{
			TEST_COMPARE(line.shortfallQty, 0);
		}

		// ...but the order still has to be for 10, on one line.
		const PartManager::OrderDraftPreview preview =
			PartManager::OrderRepository::previewFromPartlist(db, listId);
		TEST_COMPARE(preview.lines.size(), static_cast<size_t>(1));
		TEST_COMPARE(preview.lines[0].item.quantityOrdered, 10);
	}

	TEST_FUNCTION(arrivalsRestockExactlyOnce)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("arrivals").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);
		const int partId = makePart(db, typeId, "RC0603-4K7", 5);

		PartManager::MouserOrder order;
		const int orderId = PartManager::OrderRepository::insertOrder(db, order);
		std::vector<PartManager::MouserOrderItem> items;
		PartManager::MouserOrderItem item;
		item.partId = partId;
		item.quantityOrdered = 40;
		items.push_back(item);
		PartManager::OrderRepository::saveItems(db, orderId, items);

		std::vector<PartManager::OrderLine> lines = PartManager::OrderRepository::lines(db, orderId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(1));
		const int lineId = lines[0].item.id;

		// Half the shipment turns up.
		TEST_ASSERT(PartManager::OrderRepository::receiveItem(db, lineId, 10, 0.21, "CHF"));
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(db, partId), 15);
		lines = PartManager::OrderRepository::lines(db, orderId);
		TEST_COMPARE(lines[0].item.quantityReceived, 10);
		TEST_COMPARE(lines[0].item.status, std::string(PartManager::OrderItemStatus::Pending));

		// The user clicks confirm again on the same line. `totalReceived` is a running total, so
		// this is a no-op — restocking another 10 here is the bug this test exists for.
		TEST_ASSERT(PartManager::OrderRepository::receiveItem(db, lineId, 10, 0.21, "CHF"));
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(db, partId), 15);

		// The rest arrives.
		TEST_ASSERT(PartManager::OrderRepository::receiveItem(db, lineId, 40, 0.21, "CHF"));
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(db, partId), 45);
		lines = PartManager::OrderRepository::lines(db, orderId);
		TEST_COMPARE(lines[0].item.status, std::string(PartManager::OrderItemStatus::Arrived));

		PartManager::MouserOrder afterArrival;
		PartManager::OrderRepository::findOrder(db, orderId, afterArrival);
		TEST_COMPARE(afterArrival.status, std::string(PartManager::OrderStatus::PartiallyArrived));

		// A miscount corrected downwards gives the parts back rather than logging a negative
		// restock, which would claim parts arrived that never did.
		TEST_ASSERT(PartManager::OrderRepository::receiveItem(db, lineId, 35, 0.21, "CHF"));
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(db, partId), 40);
		bool sawCorrection = false;
		for (const PartManager::StockTransaction& transaction :
			PartManager::StockRepository::history(db, partId))
		{
			if (transaction.deltaQty < 0)
			{
				TEST_COMPARE(transaction.reason, std::string(PartManager::StockReason::ManualAdjust));
				sawCorrection = true;
			}
		}
		TEST_ASSERT_M(sawCorrection, "the downward correction must be in the stock log");

		// What was paid is recorded as a `paid` observation, distinct from any Mouser quote (§3).
		double price = 0.0;
		std::string currency;
		TEST_ASSERT(PartManager::SellerRepository::lastPrice(db, partId,
			PartManager::PriceSource::Paid, price, currency));
		TEST_COMPARE(currency, std::string("CHF"));
	}

	TEST_FUNCTION(closingBackordersWhatNeverArrived)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("close").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);
		const int arrivedPart = makePart(db, typeId, "RC0603-4K7", 0);
		const int missingPart = makePart(db, typeId, "CAP-0805-100n", 0);

		PartManager::MouserOrder order;
		const int orderId = PartManager::OrderRepository::insertOrder(db, order);
		std::vector<PartManager::MouserOrderItem> items;
		PartManager::MouserOrderItem full;
		full.partId = arrivedPart;
		full.quantityOrdered = 10;
		items.push_back(full);
		PartManager::MouserOrderItem partial;
		partial.partId = missingPart;
		partial.quantityOrdered = 10;
		items.push_back(partial);
		PartManager::OrderRepository::saveItems(db, orderId, items);

		std::vector<PartManager::OrderLine> lines = PartManager::OrderRepository::lines(db, orderId);
		PartManager::OrderRepository::receiveItem(db, lines[0].item.id, 10);
		PartManager::OrderRepository::receiveItem(db, lines[1].item.id, 4);

		TEST_ASSERT(PartManager::OrderRepository::closeOrder(db, orderId));

		PartManager::MouserOrder closed;
		TEST_ASSERT(PartManager::OrderRepository::findOrder(db, orderId, closed));
		TEST_COMPARE(closed.status, std::string(PartManager::OrderStatus::Closed));
		TEST_ASSERT_M(!closed.closedAt.empty(), "closing must stamp closed_at");

		lines = PartManager::OrderRepository::lines(db, orderId);
		TEST_COMPARE(lines[0].item.status, std::string(PartManager::OrderItemStatus::Arrived));
		// The six that never came are a fact, not a rounding error.
		TEST_COMPARE(lines[1].item.status, std::string(PartManager::OrderItemStatus::Backordered));
		// Closing must not move stock: every arrival already posted its own transaction.
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(db, missingPart), 4);

		// A closed order drops out of the open list but stays in the full one.
		TEST_COMPARE(PartManager::OrderRepository::listOpenOrders(db).size(), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::OrderRepository::listOrders(db).size(), static_cast<size_t>(1));
	}

	TEST_FUNCTION(linesWithoutAMouserNumberAreNotStageable)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("stageable").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);
		// A part created by hand, never seen on Mouser — it has an mpn but no seller link.
		const int partId = makePart(db, typeId, "HOMEBREW-1", 0, false);

		PartManager::MouserOrder order;
		const int orderId = PartManager::OrderRepository::insertOrder(db, order);
		std::vector<PartManager::MouserOrderItem> items;
		PartManager::MouserOrderItem item;
		item.partId = partId;
		item.quantityOrdered = 3;
		items.push_back(item);
		PartManager::OrderRepository::saveItems(db, orderId, items);

		const std::vector<PartManager::OrderLine> lines =
			PartManager::OrderRepository::lines(db, orderId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(1));
		// The line still appears — it is the one the user has to fix — but it cannot be staged.
		// Falling back to part.mpn here would build a cart Mouser rejects line by line.
		TEST_ASSERT_M(!lines[0].stageable, "a part with no Mouser link must not be stageable");
		TEST_ASSERT(lines[0].mouserPartNumber.empty());
		TEST_COMPARE(lines[0].partMpn, std::string("HOMEBREW-1"));
	}

#endif
};

TEST_INSTANTIATE(TST_OrderRepository);
