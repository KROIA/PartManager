#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_StockRepository : public UnitTest::Test
{
	TEST_CLASS(TST_StockRepository)
public:
	TST_StockRepository()
		: Test("TST_StockRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_StockRepository::restockThenTakeOutDerivesQuantity);
		ADD_TEST(TST_StockRepository::historyIsOldestFirst);
		ADD_TEST(TST_StockRepository::takeOutBeyondStockGoesNegative);
		ADD_TEST(TST_StockRepository::backfillIsIdempotent);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// A fresh temp database with part/part_type/stock_transaction laid down.
	static SQLiteWrapper::SQLite* freshDb(const char* name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite* db = new SQLiteWrapper::SQLite(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		PartManager::PartRepository::createSchema(*db);
		PartManager::StockRepository::createSchema(*db);
		return db;
	}

	static int makePart(SQLiteWrapper::SQLite& db, const std::string& name, int stockQty)
	{
		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		part.stockQty = stockQty;
		return PartManager::PartRepository::insertPart(db, part);
	}

	static int cachedStockQty(SQLiteWrapper::SQLite& db, int partId)
	{
		PartManager::Part part;
		if (!PartManager::PartRepository::findPart(db, partId, part))
		{
			return -999999;
		}
		return part.stockQty;
	}

	// Tests
	TEST_FUNCTION(restockThenTakeOutDerivesQuantity)
	{
		TEST_START;

		std::unique_ptr<SQLiteWrapper::SQLite> db(freshDb("PartManager_TST_StockRepository_derive.db"));
		int partId = makePart(*db, "10k 0603 resistor", 0);
		TEST_ASSERT(partId != 0);

		TEST_ASSERT(PartManager::StockRepository::restock(*db, partId, 100, "first reel") != 0);
		TEST_ASSERT(PartManager::StockRepository::takeOut(*db, partId, 30, "PCB build") != 0);

		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, partId), 70);
		// The whole point of the cache: a reader that never heard of StockRepository still sees 70.
		TEST_COMPARE(cachedStockQty(*db, partId), 70);

		// A correction targets an absolute count, not a delta.
		TEST_ASSERT(PartManager::StockRepository::correct(*db, partId, 68, "recount") != 0);
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, partId), 68);
		TEST_COMPARE(cachedStockQty(*db, partId), 68);
		// Correcting to the count it already has is not an event, so nothing is written.
		TEST_COMPARE(PartManager::StockRepository::correct(*db, partId, 68, "recount again"), 0);
		TEST_COMPARE(PartManager::StockRepository::history(*db, partId).size(), static_cast<size_t>(3));

		// Zero-delta and non-positive quantities are rejected outright.
		TEST_COMPARE(PartManager::StockRepository::restock(*db, partId, 0, ""), 0);
		TEST_COMPARE(PartManager::StockRepository::takeOut(*db, partId, -5, ""), 0);
		TEST_COMPARE(PartManager::StockRepository::history(*db, partId).size(), static_cast<size_t>(3));
	}

	TEST_FUNCTION(historyIsOldestFirst)
	{
		TEST_START;

		std::unique_ptr<SQLiteWrapper::SQLite> db(freshDb("PartManager_TST_StockRepository_history.db"));
		int partId = makePart(*db, "100n 0603 ceramic", 0);

		PartManager::StockRepository::restock(*db, partId, 500, "reel");
		PartManager::StockRepository::takeOut(*db, partId, 12, "prototype");
		PartManager::StockRepository::correct(*db, partId, 480, "recount");

		std::vector<PartManager::StockTransaction> log = PartManager::StockRepository::history(*db, partId);
		TEST_COMPARE(log.size(), static_cast<size_t>(3));
		TEST_COMPARE(log[0].deltaQty, 500);
		TEST_COMPARE(log[0].reason, std::string(PartManager::StockReason::Restock));
		TEST_COMPARE(log[0].note, std::string("reel"));
		TEST_COMPARE(log[1].deltaQty, -12);
		TEST_COMPARE(log[1].reason, std::string(PartManager::StockReason::CheckoutPartlist));
		TEST_COMPARE(log[2].deltaQty, -8);
		TEST_COMPARE(log[2].reason, std::string(PartManager::StockReason::ManualAdjust));
		TEST_ASSERT_M(log[0].id < log[1].id && log[1].id < log[2].id, "history must be oldest first");
		TEST_ASSERT_M(!log[0].createdAt.empty(), "every transaction must carry a timestamp");
	}

	// Decision: a take-out larger than stock is ALLOWED and drives the quantity negative, rather than
	// being rejected. The log records what physically happened; refusing the entry would leave the DB
	// further from reality than it already is. A negative quantity is the "recount me" signal.
	TEST_FUNCTION(takeOutBeyondStockGoesNegative)
	{
		TEST_START;

		std::unique_ptr<SQLiteWrapper::SQLite> db(freshDb("PartManager_TST_StockRepository_negative.db"));
		int partId = makePart(*db, "M3x10 screw", 0);

		PartManager::StockRepository::restock(*db, partId, 5, "");
		TEST_ASSERT_M(PartManager::StockRepository::takeOut(*db, partId, 8, "took the lot") != 0,
			"an over-draw must be recorded, not rejected");
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, partId), -3);
		TEST_COMPARE(cachedStockQty(*db, partId), -3);

		// And a correction is what puts it right again.
		PartManager::StockRepository::correct(*db, partId, 0, "found none on the shelf");
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, partId), 0);
	}

	TEST_FUNCTION(backfillIsIdempotent)
	{
		TEST_START;

		std::unique_ptr<SQLiteWrapper::SQLite> db(freshDb("PartManager_TST_StockRepository_backfill.db"));
		// Two parts the CSV import path would leave behind: a standing stock_qty, no log at all.
		int importedA = makePart(*db, "imported A", 42);
		int importedB = makePart(*db, "imported B", 7);
		int emptyPart = makePart(*db, "never stocked", 0);
		TEST_COMPARE(PartManager::StockRepository::history(*db, importedA).size(), static_cast<size_t>(0));

		TEST_COMPARE(PartManager::StockRepository::backfillOpeningBalances(*db), 2);
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, importedA), 42);
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, importedB), 7);
		TEST_COMPARE(PartManager::StockRepository::history(*db, emptyPart).size(), static_cast<size_t>(0));

		std::vector<PartManager::StockTransaction> log = PartManager::StockRepository::history(*db, importedA);
		TEST_COMPARE(log.size(), static_cast<size_t>(1));
		TEST_COMPARE(log[0].reason, std::string(PartManager::StockReason::Initial));
		TEST_ASSERT_M(!log[0].note.empty(), "an opening balance must say where it came from");

		// Second run: nothing left to do, and above all nothing counted twice.
		TEST_COMPARE(PartManager::StockRepository::backfillOpeningBalances(*db), 0);
		TEST_COMPARE(PartManager::StockRepository::history(*db, importedA).size(), static_cast<size_t>(1));
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, importedA), 42);

		// A part that has since traded is not a candidate either, however its balance moved.
		PartManager::StockRepository::takeOut(*db, importedB, 3, "used some");
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, importedB), 4);
		TEST_COMPARE(PartManager::StockRepository::backfillOpeningBalances(*db), 0);
		TEST_COMPARE(PartManager::StockRepository::currentQuantity(*db, importedB), 4);
	}
#endif

};

TEST_INSTANTIATE(TST_StockRepository);
