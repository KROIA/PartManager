#pragma once

#include "UnitTest.h"
#include "database/PartManager_DatabaseHandle.h"
#include "database/PartManager_DatabaseMetadata.h"
#include "database/PartManager_SchemaMigrator.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>
#include <fstream>

class TST_DatabaseCreate : public UnitTest::Test
{
	TEST_CLASS(TST_DatabaseCreate)
public:
	TST_DatabaseCreate()
		: Test("TST_DatabaseCreate")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_DatabaseCreate::createsLayoutAndSeedsTypes);
		ADD_TEST(TST_DatabaseCreate::reopenIsIdempotent);
		ADD_TEST(TST_DatabaseCreate::rejectsBadNamesAndNonEmptyFolders);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// A clean temp parent folder to create databases inside.
	static std::filesystem::path freshParent(const char* name)
	{
		std::filesystem::path parent = std::filesystem::temp_directory_path() / name;
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return parent;
	}

	// Tests
	TEST_FUNCTION(createsLayoutAndSeedsTypes)
	{
		TEST_START;

		std::filesystem::path parent = freshParent("PartManager_TST_DatabaseCreate_layout");
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "HomeLab", error);

		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT(handle->isOpen());

		std::filesystem::path folder = parent / "HomeLab";
		TEST_ASSERT_M(std::filesystem::exists(folder / "HomeLab.pmdb"), "missing .pmdb entry file");
		TEST_ASSERT_M(std::filesystem::exists(folder / "README.md"), "missing README.md");
		TEST_ASSERT_M(std::filesystem::exists(folder / "partmanager.db"), "missing partmanager.db");
		TEST_ASSERT_M(std::filesystem::is_directory(folder / "filestore"), "missing filestore/");
		TEST_ASSERT_M(std::filesystem::is_directory(folder / "kicad_libs"), "missing kicad_libs/");
		TEST_ASSERT_M(std::filesystem::is_directory(folder / "backups"), "missing backups/");

		// The .pmdb cache must carry the current schema version (§1a).
		PartManager::DatabaseMetadataValues values;
		TEST_ASSERT_M(PartManager::DatabaseMetadata::readPmdbFile((folder / "HomeLab.pmdb").string(), values),
			"readPmdbFile failed");
		TEST_COMPARE(values.schemaVersion, PartManager::CurrentSchemaVersion);

		// Fresh database is usable, not empty (§1b seeds the default type templates).
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(handle->connection()).size(),
			static_cast<size_t>(18));
	}

	TEST_FUNCTION(reopenIsIdempotent)
	{
		TEST_START;

		std::filesystem::path parent = freshParent("PartManager_TST_DatabaseCreate_reopen");
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Reopen", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);
		handle.reset();

		std::string pmdbPath = (parent / "Reopen" / "Reopen.pmdb").string();
		PartManager::DatabaseHandle reopened(pmdbPath);
		TEST_ASSERT_M(reopened.open(), "reopen failed: " + reopened.errorMessage());
		// No second migration, no duplicated seed rows.
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(reopened.connection()).size(),
			static_cast<size_t>(18));
	}

	TEST_FUNCTION(rejectsBadNamesAndNonEmptyFolders)
	{
		TEST_START;

		std::filesystem::path parent = freshParent("PartManager_TST_DatabaseCreate_reject");
		std::string error;

		TEST_ASSERT_M(PartManager::DatabaseHandle::createNew(parent.string(), "", error) == nullptr,
			"empty name must be rejected");
		TEST_ASSERT_M(PartManager::DatabaseHandle::createNew(parent.string(), "a/b", error) == nullptr,
			"forward slash in name must be rejected");
		TEST_ASSERT_M(PartManager::DatabaseHandle::createNew(parent.string(), "a\\b", error) == nullptr,
			"backslash in name must be rejected");
		TEST_ASSERT_M(!error.empty(), "a rejection must report why");

		std::filesystem::create_directories(parent / "Occupied");
		{
			std::ofstream squatter((parent / "Occupied" / "squatter.txt").string());
			squatter << "not empty";
		}
		TEST_ASSERT_M(PartManager::DatabaseHandle::createNew(parent.string(), "Occupied", error) == nullptr,
			"non-empty existing folder must be rejected");

		// An unwritable parent: a path whose parent component is a file, not a folder.
		std::filesystem::path blocker = parent / "blocker.txt";
		{
			std::ofstream file(blocker.string());
			file << "x";
		}
		TEST_ASSERT_M(PartManager::DatabaseHandle::createNew(blocker.string(), "Nested", error) == nullptr,
			"unwritable target must be rejected");
	}
#endif

};

TEST_INSTANTIATE(TST_DatabaseCreate);
