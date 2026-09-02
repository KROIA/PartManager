#pragma once

#include "UnitTest.h"
#include "backup/PartManager_BackupManager.h"
#include <filesystem>
#include <fstream>

// §9a's snapshots. This is the feature that stands in for an undo history, so the cases that
// matter are the destructive ones: pruning must never take the snapshot it just wrote, and a
// restore must leave the previous database recoverable.
class TST_BackupManager : public UnitTest::Test
{
	TEST_CLASS(TST_BackupManager)
public:
	TST_BackupManager()
		: Test("TST_BackupManager")
	{
		ADD_TEST(TST_BackupManager::snapshotNamesRoundTripAndRejectStrangers);
		ADD_TEST(TST_BackupManager::pruningKeepsTheNewestAndNeverEmptiesTheFolder);
		ADD_TEST(TST_BackupManager::restoreMovesTheCurrentDatabaseAsideRatherThanDeletingIt);
		ADD_TEST(TST_BackupManager::aFreshDatabaseIsAlwaysDueAndAJustTakenOneIsNot);
	}

private:

	// A throwaway database folder with a file that has recognisable contents, so a restore can be
	// checked by reading the bytes back rather than by trusting a return value.
	static std::filesystem::path makeDatabase(const std::string& name, const std::string& contents)
	{
		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / ("PartManager_TST_BackupManager_" + name);
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		const std::filesystem::path file = folder / "partmanager.db";
		std::ofstream out(file, std::ios::binary);
		out << contents;
		out.close();
		return file;
	}

	static std::string readAll(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	// A snapshot with a chosen timestamp, so retention can be tested without waiting minutes.
	static void writeSnapshot(const std::filesystem::path& folder, const std::string& stamp)
	{
		std::filesystem::create_directories(folder);
		std::ofstream out(folder / (stamp + ".db"), std::ios::binary);
		out << stamp;
	}

	// Tests

	TEST_FUNCTION(snapshotNamesRoundTripAndRejectStrangers)
	{
		TEST_START;

		TEST_COMPARE(PartManager::BackupManager::snapshotFileName("2026-09-02T06:30:00"),
			std::string("2026-09-02_0630.db"));
		TEST_COMPARE(PartManager::BackupManager::timestampFromFileName("2026-09-02_0630.db"),
			std::string("2026-09-02_0630"));

		// Anything that is not one of ours must not be recognised — prune() deletes what this
		// function accepts, so a user's own file in the backup folder has to be invisible to it.
		TEST_ASSERT(PartManager::BackupManager::timestampFromFileName("notes.txt").empty());
		TEST_ASSERT(PartManager::BackupManager::timestampFromFileName("partmanager.db").empty());
		TEST_ASSERT(PartManager::BackupManager::timestampFromFileName("2026-09-02_0630.db.bak").empty());
		TEST_ASSERT(PartManager::BackupManager::timestampFromFileName("xxxx-xx-xx_xxxx.db").empty());
		TEST_ASSERT_M(PartManager::BackupManager::timestampFromFileName(
			"2026-09-02_0630.pmdb").empty(), "a .pmdb must not be mistaken for a snapshot");
	}

	TEST_FUNCTION(pruningKeepsTheNewestAndNeverEmptiesTheFolder)
	{
		TEST_START;

		const std::filesystem::path database = makeDatabase("prune", "v1");
		const std::filesystem::path backups = database.parent_path() / "backups";
		for (const char* stamp : { "2026-09-01_0600", "2026-09-01_1200", "2026-09-02_0600" })
		{
			writeSnapshot(backups, stamp);
		}
		// A file that is not a snapshot. Pruning must walk straight past it.
		std::ofstream(backups / "why-i-kept-these.txt") << "notes";

		std::vector<PartManager::BackupEntry> snapshots =
			PartManager::BackupManager::listSnapshots(database.string(), std::string());
		TEST_COMPARE(snapshots.size(), static_cast<size_t>(3));
		// Newest first, and the fixed-width timestamp means a string sort is a chronological one.
		TEST_COMPARE(snapshots[0].timestamp, std::string("2026-09-02_0600"));

		TEST_COMPARE(PartManager::BackupManager::prune(database.string(), std::string(), 2), 1);
		snapshots = PartManager::BackupManager::listSnapshots(database.string(), std::string());
		TEST_COMPARE(snapshots.size(), static_cast<size_t>(2));
		TEST_ASSERT_M(std::filesystem::exists(backups / "why-i-kept-these.txt"),
			"pruning must not touch files that are not snapshots");

		// A retention of 0 would delete the snapshot taken a millisecond earlier, which is the
		// one way this feature could actively destroy data. Clamped to 1 instead.
		PartManager::BackupManager::prune(database.string(), std::string(), 0);
		TEST_COMPARE(PartManager::BackupManager::listSnapshots(database.string(), std::string()).size(),
			static_cast<size_t>(1));

		// A snapshot must survive its own retention pass even when a *future*-dated file is
		// sitting in the folder — a wrong clock or a file copied off another machine would
		// otherwise outrank the fresh one and have it deleted the instant it was taken.
		writeSnapshot(backups, "2099-01-01_0000");
		std::string error;
		const std::string written = PartManager::BackupManager::createSnapshot(database.string(),
			std::string(), 1, &error);
		TEST_ASSERT_M(!written.empty(), "createSnapshot failed: " + error);
		TEST_ASSERT_M(std::filesystem::exists(written),
			"the snapshot just written must survive its own retention pass");
	}

	TEST_FUNCTION(restoreMovesTheCurrentDatabaseAsideRatherThanDeletingIt)
	{
		TEST_START;

		const std::filesystem::path database = makeDatabase("restore", "current data");
		std::string error;
		const std::string snapshot = PartManager::BackupManager::createSnapshot(database.string(),
			std::string(), 20, &error);
		TEST_ASSERT_M(!snapshot.empty(), "createSnapshot failed: " + error);

		// The user carries on working, then wants the old state back.
		{
			std::ofstream out(database, std::ios::binary | std::ios::trunc);
			out << "edited into uselessness";
		}

		std::string movedAside;
		TEST_ASSERT_M(PartManager::BackupManager::restore(database.string(), snapshot,
			&movedAside, &error), "restore failed: " + error);
		TEST_COMPARE(readAll(database), std::string("current data"));

		// The whole point: restoring the wrong snapshot has to be undoable, so the file that was
		// replaced is renamed, never deleted.
		TEST_ASSERT_M(!movedAside.empty(), "restore must report where the old database went");
		TEST_ASSERT_M(std::filesystem::exists(movedAside),
			"the replaced database must still exist: " + movedAside);
		TEST_COMPARE(readAll(movedAside), std::string("edited into uselessness"));

		// A snapshot that is gone must fail cleanly rather than truncating the database.
		TEST_ASSERT(!PartManager::BackupManager::restore(database.string(),
			(database.parent_path() / "nope.db").string(), nullptr, &error));
		TEST_COMPARE(readAll(database), std::string("current data"));
	}

	TEST_FUNCTION(aFreshDatabaseIsAlwaysDueAndAJustTakenOneIsNot)
	{
		TEST_START;

		const std::filesystem::path database = makeDatabase("due", "v1");

		// Never backed up is always due — the first snapshot is the one that matters most.
		TEST_ASSERT(PartManager::BackupManager::isSnapshotDue(database.string(), std::string(), 6));

		std::string error;
		TEST_ASSERT(!PartManager::BackupManager::createSnapshot(database.string(), std::string(),
			20, &error).empty());
		TEST_ASSERT_M(!PartManager::BackupManager::isSnapshotDue(database.string(), std::string(), 6),
			"a snapshot taken seconds ago must not be due again");

		// An old one is. Written by hand so the test does not have to wait six hours.
		writeSnapshot(database.parent_path() / "backups", "2020-01-01_0000");
		std::filesystem::remove(database.parent_path() / "backups"
			/ PartManager::BackupManager::snapshotFileName("2026-01-01T00:00:00"));
		for (const PartManager::BackupEntry& entry :
			PartManager::BackupManager::listSnapshots(database.string(), std::string()))
		{
			if (entry.timestamp != "2020-01-01_0000")
			{
				std::filesystem::remove(entry.path);
			}
		}
		TEST_ASSERT_M(PartManager::BackupManager::isSnapshotDue(database.string(), std::string(), 6),
			"a snapshot from 2020 must be overdue");
	}
};

TEST_INSTANTIATE(TST_BackupManager);
