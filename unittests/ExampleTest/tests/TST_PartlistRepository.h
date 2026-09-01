#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_PartlistRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// §4's partlist storage. The interesting part is not the CRUD — it is that an unresolved line
// (`part_id IS NULL`) has to survive every round trip, keep appearing in lines(), and report no
// shortfall rather than a confident zero.
class TST_PartlistRepository : public UnitTest::Test
{
	TEST_CLASS(TST_PartlistRepository)
public:
	TST_PartlistRepository()
		: Test("TST_PartlistRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_PartlistRepository::crudRoundTrip);
		ADD_TEST(TST_PartlistRepository::multiplierDrivesNeededAndShortfall);
		ADD_TEST(TST_PartlistRepository::unresolvedLinesSurviveAndReportNoShortfall);
		ADD_TEST(TST_PartlistRepository::deleteTakesTheItemsWithIt);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// A database with one part type, seeded so the parts below have something to hang off.
	static void createSchemas(SQLiteWrapper::SQLite& db)
	{
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::StockRepository::createSchema(db);
		PartManager::PartlistRepository::createSchema(db);
	}

	static std::filesystem::path databasePath(const std::string& name)
	{
		std::filesystem::path path =
			std::filesystem::temp_directory_path() / ("PartManager_TST_PartlistRepository_" + name + ".db");
		std::filesystem::remove(path);
		return path;
	}

	// A part with a known stock quantity, written through the log so stock_qty is authoritative.
	static int makePart(SQLiteWrapper::SQLite& db, int typeId, const std::string& name, int stock)
	{
		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		part.mpn = name;
		const int id = PartManager::PartRepository::insertPart(db, part);
		if (id != 0 && stock > 0)
		{
			PartManager::StockRepository::restock(db, id, stock, "test fixture");
		}
		return id;
	}

	static int makeType(SQLiteWrapper::SQLite& db)
	{
		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		return PartManager::PartTypeRepository::insertType(db, type);
	}

	// Tests

	TEST_FUNCTION(crudRoundTrip)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("crud").string());
		db.open();
		createSchemas(db);

		PartManager::Partlist partlist;
		partlist.name = "PSU Rev C";
		partlist.description = "the bench supply";
		partlist.projectLinkUrl = "github.com/alex/psu-revc";
		partlist.multiplier = 5;
		partlist.source = PartManager::PartlistSource::Manual;

		const int id = PartManager::PartlistRepository::insertPartlist(db, partlist);
		TEST_ASSERT_M(id != PartManager::NoPartlistId, "insertPartlist failed");

		PartManager::Partlist readBack;
		TEST_ASSERT_M(PartManager::PartlistRepository::findPartlist(db, id, readBack), "findPartlist failed");
		TEST_COMPARE(readBack.name, partlist.name);
		TEST_COMPARE(readBack.projectLinkUrl, partlist.projectLinkUrl);
		TEST_COMPARE(readBack.multiplier, 5);
		TEST_COMPARE(readBack.source, std::string(PartManager::PartlistSource::Manual));

		readBack.name = "PSU Rev D";
		readBack.multiplier = 2;
		TEST_ASSERT_M(PartManager::PartlistRepository::updatePartlist(db, readBack), "updatePartlist failed");
		PartManager::Partlist afterUpdate;
		TEST_ASSERT(PartManager::PartlistRepository::findPartlist(db, id, afterUpdate));
		TEST_COMPARE(afterUpdate.name, std::string("PSU Rev D"));
		TEST_COMPARE(afterUpdate.multiplier, 2);
		// `source` is history, not a field — updatePartlist must leave it alone.
		TEST_COMPARE(afterUpdate.source, std::string(PartManager::PartlistSource::Manual));

		TEST_COMPARE(PartManager::PartlistRepository::listPartlists(db).size(), static_cast<size_t>(1));

		// A multiplier of 0 would silently zero every needed quantity, so it is clamped, not stored.
		afterUpdate.multiplier = 0;
		PartManager::PartlistRepository::updatePartlist(db, afterUpdate);
		PartManager::Partlist clamped;
		TEST_ASSERT(PartManager::PartlistRepository::findPartlist(db, id, clamped));
		TEST_COMPARE(clamped.multiplier, 1);
	}

	TEST_FUNCTION(multiplierDrivesNeededAndShortfall)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("shortfall").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);

		// 12 on the shelf, 3 needed per board — enough for four boards, not for five.
		const int resistorId = makePart(db, typeId, "RC0603-4K7", 12);
		const int capacitorId = makePart(db, typeId, "CAP-0805-100n", 45);

		PartManager::Partlist partlist;
		partlist.name = "PSU Rev C";
		partlist.multiplier = 5;
		const int listId = PartManager::PartlistRepository::insertPartlist(db, partlist);

		std::vector<PartManager::PartlistItem> items;
		PartManager::PartlistItem resistorLine;
		resistorLine.partId = resistorId;
		resistorLine.designators = "R1,R2,R5";
		resistorLine.quantityPerUnit = 3;
		items.push_back(resistorLine);
		PartManager::PartlistItem capacitorLine;
		capacitorLine.partId = capacitorId;
		capacitorLine.designators = "C1,C2";
		capacitorLine.quantityPerUnit = 2;
		items.push_back(capacitorLine);
		TEST_ASSERT_M(PartManager::PartlistRepository::saveItems(db, listId, items), "saveItems failed");

		std::vector<PartManager::PartlistLine> lines = PartManager::PartlistRepository::lines(db, listId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(2));

		TEST_ASSERT(lines[0].resolved);
		TEST_COMPARE(lines[0].partName, std::string("RC0603-4K7"));
		TEST_COMPARE(lines[0].item.designators, std::string("R1,R2,R5"));
		TEST_COMPARE(lines[0].neededQty, 15);          // 3 per unit x 5 boards
		TEST_COMPARE(lines[0].stockQty, 12);
		TEST_COMPARE(lines[0].shortfallQty, 3);

		// 10 needed, 45 on the shelf — a surplus is not a negative shortfall.
		TEST_COMPARE(lines[1].neededQty, 10);
		TEST_COMPARE(lines[1].shortfallQty, 0);

		// One board's worth is covered by what is already there.
		PartManager::Partlist single;
		PartManager::PartlistRepository::findPartlist(db, listId, single);
		single.multiplier = 1;
		PartManager::PartlistRepository::updatePartlist(db, single);
		lines = PartManager::PartlistRepository::lines(db, listId);
		TEST_COMPARE(lines[0].neededQty, 3);
		TEST_COMPARE(lines[0].shortfallQty, 0);
	}

	TEST_FUNCTION(unresolvedLinesSurviveAndReportNoShortfall)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("unresolved").string());
		db.open();
		createSchemas(db);
		const int typeId = makeType(db);
		const int knownId = makePart(db, typeId, "RC0603-4K7", 1);

		PartManager::Partlist partlist;
		partlist.name = "imported BOM";
		partlist.multiplier = 5;
		partlist.source = PartManager::PartlistSource::CsvImport;
		const int listId = PartManager::PartlistRepository::insertPartlist(db, partlist);

		std::vector<PartManager::PartlistItem> items;
		PartManager::PartlistItem resolved;
		resolved.partId = knownId;
		resolved.quantityPerUnit = 1;
		items.push_back(resolved);
		PartManager::PartlistItem unresolved;   // partId stays NoPartId
		unresolved.designators = "U1";
		unresolved.quantityPerUnit = 1;
		unresolved.rawImportData = "{\"mpn\":\"LM317-SOT23\"}";
		items.push_back(unresolved);
		TEST_ASSERT(PartManager::PartlistRepository::saveItems(db, listId, items));

		// A LEFT JOIN is the whole point: an inner join would drop exactly the row the user has
		// to act on.
		const std::vector<PartManager::PartlistLine> lines =
			PartManager::PartlistRepository::lines(db, listId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(2));

		TEST_ASSERT_M(!lines[1].resolved, "the unmatched line must come back unresolved");
		TEST_COMPARE(lines[1].item.partId, PartManager::NoPartId);
		TEST_COMPARE(lines[1].item.rawImportData, std::string("{\"mpn\":\"LM317-SOT23\"}"));
		TEST_COMPARE(lines[1].neededQty, 5);
		// No part means no stock to compare against — reporting 0 short would read as "fine".
		TEST_COMPARE(lines[1].shortfallQty, 0);
		TEST_ASSERT_M(lines[1].partName.empty(), "an unresolved line must carry no part name");

		// Resolving it is just writing the id back.
		std::vector<PartManager::PartlistItem> fixed = PartManager::PartlistRepository::listItems(db, listId);
		TEST_COMPARE(fixed.size(), static_cast<size_t>(2));
		fixed[1].partId = knownId;
		TEST_ASSERT(PartManager::PartlistRepository::saveItems(db, listId, fixed));
		TEST_ASSERT(PartManager::PartlistRepository::lines(db, listId)[1].resolved);
	}

	TEST_FUNCTION(deleteTakesTheItemsWithIt)
	{
		TEST_START;

		SQLiteWrapper::SQLite db(databasePath("delete").string());
		db.open();
		createSchemas(db);

		PartManager::Partlist partlist;
		partlist.name = "throwaway";
		const int listId = PartManager::PartlistRepository::insertPartlist(db, partlist);

		std::vector<PartManager::PartlistItem> items;
		PartManager::PartlistItem item;
		item.quantityPerUnit = 4;
		items.push_back(item);
		TEST_ASSERT(PartManager::PartlistRepository::saveItems(db, listId, items));
		TEST_COMPARE(PartManager::PartlistRepository::itemCount(db, listId), 1);

		TEST_ASSERT(PartManager::PartlistRepository::deletePartlist(db, listId));

		PartManager::Partlist gone;
		TEST_ASSERT_M(!PartManager::PartlistRepository::findPartlist(db, listId, gone),
			"the partlist must be gone");
		// Foreign keys are declared but never enforced, so the items only go if deletePartlist
		// takes them — the same trap PartRepository::deletePart() fell into.
		TEST_COMPARE(PartManager::PartlistRepository::itemCount(db, listId), 0);
	}

#endif
};

TEST_INSTANTIATE(TST_PartlistRepository);
