#pragma once

#include "UnitTest.h"
#include "database/PartManager_DatabaseMetadata.h"
#include <filesystem>

class TST_DatabaseMetadata : public UnitTest::Test
{
	TEST_CLASS(TST_DatabaseMetadata)
public:
	TST_DatabaseMetadata()
		: Test("TST_DatabaseMetadata")
	{
		ADD_TEST(TST_DatabaseMetadata::pmdbRoundTrip);
	}

private:

	// Tests
	TEST_FUNCTION(pmdbRoundTrip)
	{
		TEST_START;

		std::filesystem::path path = std::filesystem::temp_directory_path() / "PartManager_TST_DatabaseMetadata.pmdb";

		PartManager::DatabaseMetadataValues written;
		written.schemaVersion = 3;
		written.toolVersionLastSaved = "0.3.0";
		written.createdAt = "2026-01-10T12:00:00";
		written.lastOpenedAt = "2026-08-31T09:00:00";

		TEST_ASSERT_M(PartManager::DatabaseMetadata::writePmdbFile(path.string(), written), "writePmdbFile failed");

		PartManager::DatabaseMetadataValues readBack;
		TEST_ASSERT_M(PartManager::DatabaseMetadata::readPmdbFile(path.string(), readBack), "readPmdbFile failed");

		TEST_COMPARE(readBack.schemaVersion, written.schemaVersion);
		TEST_COMPARE(readBack.toolVersionLastSaved, written.toolVersionLastSaved);
		TEST_COMPARE(readBack.createdAt, written.createdAt);
		TEST_COMPARE(readBack.lastOpenedAt, written.lastOpenedAt);

		std::filesystem::remove(path);
	}

};

TEST_INSTANTIATE(TST_DatabaseMetadata);
