#pragma once

#include "UnitTest.h"
#include "database/PartManager_SchemaMigrator.h"

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
