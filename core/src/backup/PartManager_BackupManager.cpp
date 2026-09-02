#include "backup/PartManager_BackupManager.h"
#include "PartManager_global.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace PartManager
{

	namespace
	{
		// Snapshots are `<yyyy-MM-dd_HHmm>.db`. Fixed width, so recognising one is a length and
		// shape check rather than a regex, and sorting the names sorts them chronologically.
		constexpr size_t TimestampLength = 15;   // 2026-09-02_0630
		const char* const SnapshotExtension = ".db";
		const char* const DefaultFolderName = "backups";

		bool isDigits(const std::string& text, size_t from, size_t count)
		{
			if (from + count > text.size())
			{
				return false;
			}
			for (size_t i = from; i < from + count; ++i)
			{
				if (text[i] < '0' || text[i] > '9')
				{
					return false;
				}
			}
			return true;
		}

		std::string nowIso()
		{
			const std::time_t now = std::time(nullptr);
			std::tm parts{};
#ifdef _WIN32
			localtime_s(&parts, &now);
#else
			localtime_r(&now, &parts);
#endif
			char buffer[32] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d",
				parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
				parts.tm_hour, parts.tm_min, parts.tm_sec);
			return buffer;
		}

		void report(std::string* outError, const std::string& message)
		{
			if (outError != nullptr)
			{
				*outError = message;
			}
		}
	}

	std::string BackupManager::snapshotFileName(const std::string& isoTimestamp)
	{
		// "2026-09-02T06:30:00" -> "2026-09-02_0630.db". Colons are illegal in Windows filenames
		// and the seconds are noise at a 6-hourly cadence, so both go.
		if (isoTimestamp.size() < 16)
		{
			return std::string();
		}
		std::string name = isoTimestamp.substr(0, 10);   // yyyy-MM-dd
		name += '_';
		name += isoTimestamp.substr(11, 2);              // HH
		name += isoTimestamp.substr(14, 2);              // mm
		name += SnapshotExtension;
		return name;
	}

	std::string BackupManager::timestampFromFileName(const std::string& fileName)
	{
		// Shape check, not a parse: yyyy-MM-dd_HHmm.db and nothing else. A user's own notes in
		// the backup folder must never be mistaken for a snapshot and pruned away.
		if (fileName.size() != TimestampLength + 3)
		{
			return std::string();
		}
		if (fileName.compare(TimestampLength, 3, SnapshotExtension) != 0)
		{
			return std::string();
		}
		if (fileName[4] != '-' || fileName[7] != '-' || fileName[10] != '_')
		{
			return std::string();
		}
		if (!isDigits(fileName, 0, 4) || !isDigits(fileName, 5, 2) || !isDigits(fileName, 8, 2)
			|| !isDigits(fileName, 11, 4))
		{
			return std::string();
		}
		return fileName.substr(0, TimestampLength);
	}

	std::string BackupManager::backupFolderFor(const std::string& databasePath,
		const std::string& configuredFolder)
	{
		if (!configuredFolder.empty())
		{
			return configuredFolder;
		}
		if (databasePath.empty())
		{
			return std::string();
		}
		std::error_code error;
		std::filesystem::path folder = std::filesystem::path(databasePath).parent_path()
			/ DefaultFolderName;
		PM_UNUSED(error);
		return folder.string();
	}

	std::vector<BackupEntry> BackupManager::listSnapshots(const std::string& databasePath,
		const std::string& configuredFolder)
	{
		std::vector<BackupEntry> result;
		const std::string folder = backupFolderFor(databasePath, configuredFolder);
		if (folder.empty())
		{
			return result;
		}

		std::error_code error;
		if (!std::filesystem::is_directory(folder, error))
		{
			// No folder yet is the normal state before the first snapshot, not an error.
			return result;
		}
		for (const std::filesystem::directory_entry& entry :
			std::filesystem::directory_iterator(folder, error))
		{
			if (!entry.is_regular_file(error))
			{
				continue;
			}
			const std::string timestamp = timestampFromFileName(entry.path().filename().string());
			if (timestamp.empty())
			{
				continue;
			}
			BackupEntry snapshot;
			snapshot.path = entry.path().string();
			snapshot.timestamp = timestamp;
			snapshot.sizeBytes = static_cast<long long>(std::filesystem::file_size(entry.path(), error));
			result.push_back(snapshot);
		}

		// The timestamp is fixed-width and big-endian by construction, so a plain string sort is
		// a chronological one — no date parsing anywhere in this class.
		std::sort(result.begin(), result.end(), [](const BackupEntry& a, const BackupEntry& b)
			{
				return a.timestamp > b.timestamp;
			});
		return result;
	}

	int BackupManager::prune(const std::string& databasePath, const std::string& configuredFolder,
		int retentionCount, const std::string& alwaysKeep)
	{
		// "Keep none" would delete the snapshot that was just taken, which is never what anyone
		// means by a retention setting.
		const int keep = retentionCount < 1 ? 1 : retentionCount;
		const std::vector<BackupEntry> snapshots = listSnapshots(databasePath, configuredFolder);

		std::error_code error;
		// Compared as paths, not strings: createSnapshot() builds its return value from
		// std::filesystem and listSnapshots() from directory_iterator, and the two can differ in
		// separator style for the same file.
		const std::filesystem::path protectedPath = alwaysKeep.empty()
			? std::filesystem::path() : std::filesystem::path(alwaysKeep);

		int kept = 0;
		int removed = 0;
		for (const BackupEntry& snapshot : snapshots)
		{
			const bool isProtected = !alwaysKeep.empty()
				&& std::filesystem::path(snapshot.path) == protectedPath;
			if (isProtected || kept < keep)
			{
				++kept;
				continue;
			}
			if (std::filesystem::remove(snapshot.path, error))
			{
				++removed;
			}
		}
		return removed;
	}

	std::string BackupManager::createSnapshot(const std::string& databasePath,
		const std::string& configuredFolder, int retentionCount, std::string* outError)
	{
		if (databasePath.empty())
		{
			report(outError, "No database is open.");
			return std::string();
		}
		std::error_code error;
		if (!std::filesystem::is_regular_file(databasePath, error))
		{
			report(outError, "The database file does not exist: " + databasePath);
			return std::string();
		}

		const std::string folder = backupFolderFor(databasePath, configuredFolder);
		std::filesystem::create_directories(folder, error);
		if (error)
		{
			report(outError, "Could not create the backup folder: " + error.message());
			return std::string();
		}

		const std::filesystem::path target = std::filesystem::path(folder)
			/ snapshotFileName(nowIso());
		// overwrite_existing: two snapshots in the same minute are the same snapshot. Failing
		// here instead would make a "Backup Now" pressed twice look broken.
		std::filesystem::copy_file(databasePath, target,
			std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			report(outError, "Could not write the snapshot: " + error.message());
			return std::string();
		}

		// The snapshot just written is protected from its own retention pass — see prune()'s note
		// on future-dated files.
		prune(databasePath, configuredFolder, retentionCount, target.string());
		return target.string();
	}

	bool BackupManager::isSnapshotDue(const std::string& databasePath,
		const std::string& configuredFolder, int intervalHours)
	{
		const std::vector<BackupEntry> snapshots = listSnapshots(databasePath, configuredFolder);
		if (snapshots.empty())
		{
			// Never backed up is always due — the first snapshot is the one that matters most.
			return true;
		}

		// Both sides are the same fixed-width string, so "is the newest older than now minus the
		// interval" is a string comparison against a synthesised cutoff. No date arithmetic, and
		// no timezone to get wrong.
		const std::time_t cutoff = std::time(nullptr)
			- static_cast<std::time_t>(intervalHours < 1 ? 1 : intervalHours) * 3600;
		std::tm parts{};
#ifdef _WIN32
		localtime_s(&parts, &cutoff);
#else
		localtime_r(&cutoff, &parts);
#endif
		char buffer[32] = { 0 };
		std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d_%02d%02d",
			parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
		return snapshots.front().timestamp < std::string(buffer);
	}

	bool BackupManager::restore(const std::string& databasePath, const std::string& snapshotPath,
		std::string* outMovedAsidePath, std::string* outError)
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(snapshotPath, error))
		{
			report(outError, "That snapshot no longer exists: " + snapshotPath);
			return false;
		}

		// The current database is moved aside, never deleted — a restore of the wrong snapshot
		// has to be undoable, or this feature is a way to lose data rather than recover it.
		if (std::filesystem::is_regular_file(databasePath, error))
		{
			const std::filesystem::path current(databasePath);
			std::string stamp = nowIso();
			// Colons again: illegal in a Windows filename.
			stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
			const std::filesystem::path asideName = current.parent_path()
				/ (current.stem().string() + ".pre-restore-" + stamp + current.extension().string());
			std::filesystem::rename(databasePath, asideName, error);
			if (error)
			{
				// Almost always "the file is still open". Saying so beats a bare error code,
				// because closing the database is exactly what the caller forgot to do.
				report(outError, "Could not move the current database aside — is it still open? ("
					+ error.message() + ")");
				return false;
			}
			if (outMovedAsidePath != nullptr)
			{
				*outMovedAsidePath = asideName.string();
			}
		}

		std::filesystem::copy_file(snapshotPath, databasePath,
			std::filesystem::copy_options::overwrite_existing, error);
		if (error)
		{
			report(outError, "Could not put the snapshot in place: " + error.message());
			return false;
		}
		return true;
	}

}
