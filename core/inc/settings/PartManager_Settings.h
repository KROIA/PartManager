// @file PartManager_Settings.h
// @brief Minimal app-wide settings facade over the vendored AppSettings library.
//
// Currently exposes only what core/database's DatabaseRegistry needs: a
// persisted list of known database entry-file paths and when each was last
// opened (docs/design/ARCHITECTURE.md §1b). Language/theme/API-key settings
// are a later backlog item (§9) and are intentionally not built here.
// @see PartManager_DatabaseRegistry.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One entry in the known-databases list (§1b) — the .pmdb path plus when it was last opened.
	struct PART_MANAGER_API KnownDatabaseEntry
	{
		std::string path;			// path to the .pmdb entry file
		std::string lastOpenedAt;	// ISO-8601 timestamp, empty if never opened
	};

	// Facade over AppSettings persisting app-wide state that must exist before any database is open.
	class PART_MANAGER_API Settings
	{
		Settings() = delete;
	public:
		// Absolute path of the settings file in the per-user application-data directory.
		// Empty if the AppSettings library is not available.
		static std::string getSettingsFilePath();

		// Returns every known database entry, in no particular guaranteed order.
		static std::vector<KnownDatabaseEntry> getKnownDatabases();
		// Overwrites the entire known-databases list.
		static void setKnownDatabases(const std::vector<KnownDatabaseEntry>& entries);

	};

}

