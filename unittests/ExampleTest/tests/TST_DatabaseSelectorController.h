#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_DatabaseSelectorController.h"
#include "database/PartManager_DatabaseMetadata.h"
#include "database/PartManager_DatabaseRegistry.h"
#include "database/PartManager_SchemaMigrator.h"
#include "settings/PartManager_Settings.h"
#include <filesystem>
#include <fstream>

// What the database selector and the Manage Databases screen show, minus the widgets: the
// schema badge (§1c) each known database gets, the description read/written as README.md
// (§1b), and the promise behind "Remove from list" — that it forgets an entry and touches
// no file on disk.
//
// The badge cases matter because they are decided from the `.pmdb` cache alone (§1a): the
// list must be able to say "this one is too new to open" without opening it.
class TST_DatabaseSelectorController : public UnitTest::Test
{
	TEST_CLASS(TST_DatabaseSelectorController)
public:
	TST_DatabaseSelectorController()
		: Test("TST_DatabaseSelectorController")
	{
		ADD_TEST(TST_DatabaseSelectorController::badgeFollowsTheEntryFilesSchemaVersion);
		ADD_TEST(TST_DatabaseSelectorController::missingEntryFileIsStale);
		ADD_TEST(TST_DatabaseSelectorController::descriptionRoundTripsThroughReadme);
		ADD_TEST(TST_DatabaseSelectorController::removeFromListLeavesTheFilesAlone);
	}

private:

	// A database folder with nothing in it but the entry file — enough to badge, by design.
	static std::filesystem::path makePmdb(const std::string& name, int schemaVersion)
	{
		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / ("PartManager_TST_Selector_" + name);
		std::error_code errorCode;
		std::filesystem::remove_all(folder, errorCode);
		std::filesystem::create_directories(folder, errorCode);

		std::filesystem::path pmdbPath = folder / (name + ".pmdb");
		PartManager::DatabaseMetadataValues values;
		values.schemaVersion = schemaVersion;
		values.toolVersionLastSaved = "0.3.0";
		values.createdAt = "2026-01-10T12:00:00";
		values.lastOpenedAt = "2026-08-31T09:00:00";
		PartManager::DatabaseMetadata::writePmdbFile(pmdbPath.string(), values);
		return pmdbPath;
	}

	static PartManager::SchemaBadge badgeOf(const std::filesystem::path& pmdbPath)
	{
		int schemaVersion = 0;
		return PartManager::DatabaseSelectorController::badgeOf(
			QString::fromStdString(pmdbPath.string()), schemaVersion);
	}

	// Tests
	TEST_FUNCTION(badgeFollowsTheEntryFilesSchemaVersion)
	{
		TEST_START;

		std::filesystem::path older = makePmdb("older", PartManager::CurrentSchemaVersion - 1);
		std::filesystem::path current = makePmdb("current", PartManager::CurrentSchemaVersion);
		std::filesystem::path newer = makePmdb("newer", PartManager::CurrentSchemaVersion + 1);

		int schemaVersion = 0;
		TEST_ASSERT_M(badgeOf(older) == PartManager::SchemaBadge::needsUpdate,
			"an older schema must badge as needing a migration");
		TEST_ASSERT_M(badgeOf(current) == PartManager::SchemaBadge::current,
			"the current schema must badge as current");
		TEST_ASSERT_M(badgeOf(newer) == PartManager::SchemaBadge::tooNew,
			"a newer schema must badge as too new to open (§1c refuses it)");

		// The badge also reports the version it read, which is what the label prints.
		PartManager::DatabaseSelectorController::badgeOf(
			QString::fromStdString(newer.string()), schemaVersion);
		TEST_COMPARE(schemaVersion, PartManager::CurrentSchemaVersion + 1);

		std::error_code errorCode;
		std::filesystem::remove_all(older.parent_path(), errorCode);
		std::filesystem::remove_all(current.parent_path(), errorCode);
		std::filesystem::remove_all(newer.parent_path(), errorCode);
	}

	TEST_FUNCTION(missingEntryFileIsStale)
	{
		TEST_START;

		std::filesystem::path pmdbPath = makePmdb("stale", PartManager::CurrentSchemaVersion);
		TEST_ASSERT_M(badgeOf(pmdbPath) == PartManager::SchemaBadge::current, "setup badge is wrong");

		std::error_code errorCode;
		std::filesystem::remove_all(pmdbPath.parent_path(), errorCode);
		TEST_ASSERT_M(badgeOf(pmdbPath) == PartManager::SchemaBadge::stale,
			"a .pmdb that is gone from disk must badge as stale, not as unreadable");
	}

	TEST_FUNCTION(descriptionRoundTripsThroughReadme)
	{
		TEST_START;

		std::filesystem::path pmdbPath = makePmdb("described", PartManager::CurrentSchemaVersion);
		QString path = QString::fromStdString(pmdbPath.string());

		// No README yet — an empty description, not a failure.
		TEST_ASSERT_M(PartManager::DatabaseSelectorController::descriptionOf(path).isEmpty(),
			"a database without a README.md must read back an empty description");

		TEST_ASSERT_M(PartManager::DatabaseSelectorController::setDescription(path,
			"Archived - resistors/caps from a 2025 project."), "setDescription failed");
		TEST_COMPARE(PartManager::DatabaseSelectorController::descriptionOf(path).toStdString(),
			std::string("Archived - resistors/caps from a 2025 project."));

		// The stored file keeps the folder-name title line, so it still reads as a README
		// when opened outside the app — and that line is not part of the description.
		std::ifstream readme((pmdbPath.parent_path() / "README.md").string());
		std::string firstLine;
		std::getline(readme, firstLine);
		TEST_COMPARE(firstLine, std::string("# ") + pmdbPath.parent_path().filename().string());

		std::error_code errorCode;
		std::filesystem::remove_all(pmdbPath.parent_path(), errorCode);
	}

	TEST_FUNCTION(removeFromListLeavesTheFilesAlone)
	{
		TEST_START;

		// This writes the real user-level settings file, so put back whatever was there.
		std::vector<PartManager::KnownDatabaseEntry> original = PartManager::Settings::getKnownDatabases();

		std::filesystem::path pmdbPath = makePmdb("removable", PartManager::CurrentSchemaVersion);
		QString path = QString::fromStdString(pmdbPath.string());
		PartManager::DatabaseSelectorController::setDescription(path, "Keep me on disk.");

		PartManager::DatabaseSelectorController controller;
		controller.registerDatabase(path);
		bool listedBefore = false;
		for (const PartManager::DatabaseListEntry& entry : controller.knownDatabases())
		{
			listedBefore = listedBefore || entry.pmdbPath.compare(path, Qt::CaseInsensitive) == 0;
		}
		TEST_ASSERT_M(listedBefore, "the database was not registered, so removal proves nothing");

		controller.removeFromList(path);

		bool listedAfter = false;
		for (const PartManager::DatabaseListEntry& entry : controller.knownDatabases())
		{
			listedAfter = listedAfter || entry.pmdbPath.compare(path, Qt::CaseInsensitive) == 0;
		}
		TEST_ASSERT_M(!listedAfter, "the database is still in the known list after removeFromList");

		// The whole point of "Remove from list": it is not a delete.
		TEST_ASSERT_M(std::filesystem::exists(pmdbPath), "removeFromList deleted the .pmdb entry file");
		TEST_ASSERT_M(std::filesystem::exists(pmdbPath.parent_path() / "README.md"),
			"removeFromList deleted the database folder's README.md");
		TEST_COMPARE(PartManager::DatabaseSelectorController::descriptionOf(path).toStdString(),
			std::string("Keep me on disk."));

		std::error_code errorCode;
		std::filesystem::remove_all(pmdbPath.parent_path(), errorCode);
		PartManager::Settings::setKnownDatabases(original);
	}

};

TEST_INSTANTIATE(TST_DatabaseSelectorController);
