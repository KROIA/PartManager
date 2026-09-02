#pragma once

#include "UnitTest.h"
#include "database/PartManager_SchemaMigrator.h"
#include "persistence/PartManager_TagRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_SchemaMigrator : public UnitTest::Test
{
	TEST_CLASS(TST_SchemaMigrator)
public:
	TST_SchemaMigrator()
		: Test("TST_SchemaMigrator")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_SchemaMigrator::equalVersionOpensNormally);
		ADD_TEST(TST_SchemaMigrator::lowerVersionMigrates);
		ADD_TEST(TST_SchemaMigrator::higherVersionRefuses);
		ADD_TEST(TST_SchemaMigrator::migratingAnOldDatabaseFillsTheNewTables);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Tests
	TEST_FUNCTION(equalVersionOpensNormally)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, PartManager::CurrentSchemaVersion, "0.0.0", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::equal);
		TEST_ASSERT(error.empty());
	}

	// A migration has to leave the database in the state the current version expects, not merely
	// with the right tables. Tag categories (v8) are the case that caught this: creating the
	// tables and leaving them empty means an existing database shows no families at all, and the
	// whole feature is invisible on every database that predates it — which is all of them.
	TEST_FUNCTION(migratingAnOldDatabaseFillsTheNewTables)
	{
		TEST_START;

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "PartManager_TST_Migrator_v8.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();

		// A database as it looked before categories existed: the tag schema of v2, carrying the
		// six tags the old seed wrote.
		TEST_ASSERT(db.execute(
			"CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,"
			" color TEXT NOT NULL, sort_order INTEGER NOT NULL DEFAULT 0);"));
		for (const char* name : { "SMD", "THT", "Favourite", "Obsolete", "Do not use",
			"Needs datasheet" })
		{
			db.executeWithParams("INSERT INTO tag (name,color,sort_order) VALUES (?,?,0);",
				{ name, "#1E88E5" });
		}

		std::string error;
		const PartManager::SchemaCompatibility result =
			PartManager::SchemaMigrator::migrate(db, 7, "0.0.0", error);
		TEST_ASSERT(result == PartManager::SchemaCompatibility::migrated);
		TEST_ASSERT(error.empty());

		// The families exist...
		const std::vector<PartManager::TagCategory> categories =
			PartManager::TagRepository::listCategories(db);
		TEST_ASSERT_M(!categories.empty(),
			"migrating to v8 must create the tag families, not just the table");

		// ...and the tags that were already there joined them rather than being duplicated.
		int smdCount = 0;
		int loose = 0;
		for (const PartManager::Tag& tag : PartManager::TagRepository::listTags(db))
		{
			smdCount += (tag.name == "SMD") ? 1 : 0;
			loose += (tag.categoryId == PartManager::NoTagCategoryId) ? 1 : 0;
		}
		TEST_ASSERT_M(smdCount == 1, "the existing tag is adopted, not duplicated");
		TEST_ASSERT_M(loose == 1, "only Favourite stays outside a family");

		db.close();
		std::filesystem::remove(path);
	}

	TEST_FUNCTION(lowerVersionMigrates)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, 0, "0.0.0", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::migrated);
	}

	TEST_FUNCTION(higherVersionRefuses)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, PartManager::CurrentSchemaVersion + 1, "9.9.9", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::refused);
		TEST_ASSERT_M(!error.empty(), "refusal must set an error message");
	}
#endif

};

TEST_INSTANTIATE(TST_SchemaMigrator);
