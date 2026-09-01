#pragma once

#include "UnitTest.h"
#include "database/PartManager_SchemaMigrator.h"
#include "persistence/PartManager_ListColumnRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// The §7b `part_type_list_column` table: round-trip, §2b inheritance, "reset to default",
// and the v3 -> v4 migration that introduces it.
class TST_ListColumnRepository : public UnitTest::Test
{
	TEST_CLASS(TST_ListColumnRepository)
public:
	TST_ListColumnRepository()
		: Test("TST_ListColumnRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_ListColumnRepository::savedLayoutRoundTrips);
		ADD_TEST(TST_ListColumnRepository::untouchedTypeHasNoRows);
		ADD_TEST(TST_ListColumnRepository::subtypeInheritsAndOverrides);
		ADD_TEST(TST_ListColumnRepository::migrationFromV3KeepsData);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Opens a fresh temp SQLite db with the part_type + part_type_list_column schema.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		PartManager::ListColumnRepository::createSchema(*db);
		return db;
	}

	static int addType(SQLiteWrapper::SQLite& db, const std::string& name, int parentId)
	{
		PartManager::PartType type;
		type.name = name;
		type.domain = "electronic";
		type.parentTypeId = parentId;
		return PartManager::PartTypeRepository::insertType(db, type);
	}

	static PartManager::PartTypeListColumn makeColumn(const std::string& key, bool visible, int widthPx)
	{
		PartManager::PartTypeListColumn column;
		column.columnKey = key;
		column.visible = visible;
		column.widthPx = widthPx;
		return column;
	}

	// Tests
	TEST_FUNCTION(savedLayoutRoundTrips)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_ListColumnRepository_roundtrip.db");
		int resistorId = addType(*db, "Resistor", PartManager::NoParentType);
		TEST_ASSERT(resistorId != PartManager::NoParentType);

		std::vector<PartManager::PartTypeListColumn> saved{
			makeColumn("name", true, 0),
			makeColumn("stock_qty", true, 80),
			makeColumn("resistance", true, 120),
			makeColumn("mpn", false, 0)
		};
		TEST_ASSERT_M(PartManager::ListColumnRepository::saveColumns(*db, resistorId, saved),
			"saveColumns failed");

		std::vector<PartManager::PartTypeListColumn> loaded =
			PartManager::ListColumnRepository::listOwnColumns(*db, resistorId);
		TEST_COMPARE(loaded.size(), static_cast<size_t>(4));
		// sort_order comes from the vector's order, so the layout comes back exactly as written.
		TEST_COMPARE(loaded[1].columnKey, std::string("stock_qty"));
		TEST_COMPARE(loaded[1].widthPx, 80);
		TEST_COMPARE(loaded[2].columnKey, std::string("resistance"));
		TEST_COMPARE(loaded[3].columnKey, std::string("mpn"));
		TEST_ASSERT_M(!loaded[3].visible, "a hidden column must come back hidden");
		TEST_COMPARE(loaded[0].sortOrder, 0);
		TEST_COMPARE(loaded[3].sortOrder, 3);

		// Saving again replaces the layout rather than appending to it — a dropped key disappears.
		saved.pop_back();
		TEST_ASSERT(PartManager::ListColumnRepository::saveColumns(*db, resistorId, saved));
		TEST_COMPARE(PartManager::ListColumnRepository::listOwnColumns(*db, resistorId).size(),
			static_cast<size_t>(3));
	}

	TEST_FUNCTION(untouchedTypeHasNoRows)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_ListColumnRepository_untouched.db");
		int screwId = addType(*db, "Screw", PartManager::NoParentType);

		// The fallback the whole feature rests on: no rows means "derive the columns as before".
		TEST_ASSERT_M(PartManager::ListColumnRepository::effectiveColumns(*db, screwId).empty(),
			"a never-customized type must report no saved layout");

		TEST_ASSERT(PartManager::ListColumnRepository::saveColumns(*db, screwId,
			{ makeColumn("name", true, 0) }));
		TEST_COMPARE(PartManager::ListColumnRepository::effectiveColumns(*db, screwId).size(),
			static_cast<size_t>(1));

		// "Reset to default" puts it back into the derived state.
		TEST_ASSERT(PartManager::ListColumnRepository::clearColumns(*db, screwId));
		TEST_ASSERT_M(PartManager::ListColumnRepository::effectiveColumns(*db, screwId).empty(),
			"clearColumns must return the type to the derived state");
	}

	TEST_FUNCTION(subtypeInheritsAndOverrides)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_ListColumnRepository_inherit.db");
		int capacitorId = addType(*db, "Capacitor", PartManager::NoParentType);
		int ceramicId = addType(*db, "Ceramic Capacitor", capacitorId);

		TEST_ASSERT(PartManager::ListColumnRepository::saveColumns(*db, capacitorId,
			{ makeColumn("name", true, 0), makeColumn("mpn", false, 0), makeColumn("capacitance", true, 90) }));

		// §2b: a subtype with no rows of its own shows its ancestor's layout.
		std::vector<PartManager::PartTypeListColumn> inherited =
			PartManager::ListColumnRepository::effectiveColumns(*db, ceramicId);
		TEST_COMPARE(inherited.size(), static_cast<size_t>(3));
		TEST_COMPARE(inherited[1].columnKey, std::string("mpn"));
		TEST_ASSERT_M(!inherited[1].visible, "the inherited hidden column stays hidden");

		// Its own row for the same key overrides the ancestor's, keeping the ancestor's position.
		TEST_ASSERT(PartManager::ListColumnRepository::saveColumns(*db, ceramicId,
			{ makeColumn("mpn", true, 55) }));
		std::vector<PartManager::PartTypeListColumn> resolved =
			PartManager::ListColumnRepository::effectiveColumns(*db, ceramicId);
		TEST_COMPARE(resolved.size(), static_cast<size_t>(3));
		TEST_COMPARE(resolved[2].columnKey, std::string("mpn"));
		TEST_ASSERT_M(resolved[2].visible, "the subtype's own row must win");
		TEST_COMPARE(resolved[2].widthPx, 55);
		// The ancestor is untouched by the subtype's override.
		TEST_ASSERT(!PartManager::ListColumnRepository::effectiveColumns(*db, capacitorId)[1].visible);
	}

	TEST_FUNCTION(migrationFromV3KeepsData)
	{
		TEST_START;

		std::filesystem::path path =
			std::filesystem::temp_directory_path() / "PartManager_TST_ListColumnRepository_v3.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		TEST_ASSERT_M(db.open(), "could not open the temp database");

		// A database exactly as v3 left it: everything the first three migration steps create.
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::TagRepository::createSchema(db);
		PartManager::StockRepository::createSchema(db);

		int resistorId = addType(db, "Resistor", PartManager::NoParentType);
		PartManager::Part part;
		part.partTypeId = resistorId;
		part.name = "R 4k7 0603";
		part.stockQty = 42;
		int partId = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT(partId != 0);

		std::string error;
		TEST_ASSERT(PartManager::SchemaMigrator::migrate(db, 3, "0.0.0", error)
			== PartManager::SchemaCompatibility::migrated);
		TEST_ASSERT_M(error.empty(), "a forward migration must not report an error");

		// Nothing the v3 database held was touched...
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(db).size(), static_cast<size_t>(1));
		std::vector<PartManager::Part> parts = PartManager::PartRepository::listParts(db, resistorId);
		TEST_COMPARE(parts.size(), static_cast<size_t>(1));
		TEST_COMPARE(parts[0].name, std::string("R 4k7 0603"));
		TEST_COMPARE(parts[0].stockQty, 42);

		// ...and the new table is there, empty, so the table still derives its columns.
		TEST_ASSERT_M(PartManager::ListColumnRepository::effectiveColumns(db, resistorId).empty(),
			"a migrated database must start with no saved layout");
		TEST_ASSERT_M(PartManager::ListColumnRepository::saveColumns(db, resistorId,
			{ makeColumn("name", true, 0) }), "part_type_list_column must be writable after migrating");
	}
#endif

};

TEST_INSTANTIATE(TST_ListColumnRepository);
