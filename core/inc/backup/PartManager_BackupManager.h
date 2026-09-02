// @file PartManager_BackupManager.h
// @brief Rotating snapshots of the database file (§9a) — the stand-in for an undo history.
//
// There is no edit history in v1 (confirmed), and §10 autosave makes every edit
// instantly permanent. A snapshot of the `.pmdb` file is what stands between the
// user and a silent overwrite, so this is deliberately dumb and hard to break: a
// file copy into `backups/<yyyy-MM-dd_HHmm>.db`, oldest deleted past a retention
// count. No SQL, no incremental diffing, nothing that can half-succeed.
//
// **Only the database file is on the schedule.** `filestore/` and `kicad_libs/`
// are large, mostly additive and content-hashed, so they are lower-risk and
// belong to a manual full-folder backup rather than a 6-hourly copy (§9a).
//
// **restore() never deletes the current database.** It moves it aside to
// `<name>.pre-restore-<timestamp>.pmdb` first, so a restore of the wrong
// snapshot is itself undoable. The one thing that would make this useless is a
// restore that destroys the very state the user is trying to recover.
//
// The caller must **close the database before restoring** — this class does not
// own the connection and cannot close it. restore() reports the failure rather
// than corrupting an open file, but on Windows the copy would simply fail, which
// is the safe direction.
// @see docs/design/ARCHITECTURE.md §9a, §1a
// @see PartManager_Settings.h, PartManager_DatabaseHandle.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One snapshot on disk.
	struct PART_MANAGER_API BackupEntry
	{
		std::string path;           // absolute path of the snapshot file
		std::string timestamp;      // 'yyyy-MM-dd_HHmm', parsed back out of the filename
		long long sizeBytes = 0;
	};

	class PART_MANAGER_API BackupManager
	{
		BackupManager() = delete;
	public:
		// The folder snapshots go in: `backupFolder` when the user set one, otherwise `backups/`
		// beside the database file. Never creates anything — that is createSnapshot()'s job.
		static std::string backupFolderFor(const std::string& databasePath,
			const std::string& configuredFolder);

		// Copies the database file into the backup folder under a timestamped name, then prunes
		// to `retentionCount` newest. Returns the new snapshot's path, empty on failure with the
		// reason in outError.
		//
		// A snapshot taken in the same minute as an existing one **overwrites it** rather than
		// failing or accumulating: the timestamp is the identity, and two copies of the same
		// minute carry no more information than one.
		static std::string createSnapshot(const std::string& databasePath,
			const std::string& configuredFolder, int retentionCount,
			std::string* outError = nullptr);

		// Every snapshot in the folder, newest first. Non-snapshot files are ignored, so a user
		// dropping notes in there breaks nothing.
		static std::vector<BackupEntry> listSnapshots(const std::string& databasePath,
			const std::string& configuredFolder);

		// Deletes the oldest snapshots until at most `retentionCount` remain. Returns how many
		// were removed. A retentionCount below 1 is treated as 1 — "keep none" would mean the
		// snapshot just taken is deleted immediately.
		//
		// `alwaysKeep` is never deleted and counts as one of the kept. createSnapshot() passes
		// the file it just wrote: "oldest" is decided by the timestamp in the filename, and a
		// future-dated snapshot in the folder (a clock that was wrong, a file copied off another
		// machine) would otherwise outrank a fresh one and have it pruned away the moment it
		// was taken.
		static int prune(const std::string& databasePath, const std::string& configuredFolder,
			int retentionCount, const std::string& alwaysKeep = std::string());

		// Swaps `snapshotPath` in as the database, moving the current file aside first (see the
		// header note). Returns false with the reason in outError; the database must already be
		// closed. Returns the path the old database was moved to in outMovedAsidePath, so the
		// caller can tell the user where it went rather than leaving a mystery file behind.
		static bool restore(const std::string& databasePath, const std::string& snapshotPath,
			std::string* outMovedAsidePath = nullptr, std::string* outError = nullptr);

		// True when `intervalHours` have passed since the newest snapshot (or there is none).
		// Pure enough to test: it reads only the folder listing and the clock.
		static bool isSnapshotDue(const std::string& databasePath,
			const std::string& configuredFolder, int intervalHours);

		// The filename a snapshot taken at `isoTimestamp` ("yyyy-MM-ddTHH:mm:ss") would get.
		// Split out so the naming scheme is testable without touching the filesystem.
		static std::string snapshotFileName(const std::string& isoTimestamp);
		// The timestamp back out of a snapshot filename, empty when it is not one of ours.
		static std::string timestampFromFileName(const std::string& fileName);
	};

}
