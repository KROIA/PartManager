// @file PartManager_DatabaseRegistry.h
// @brief The known-databases list (§1b) — .pmdb paths + last-opened time, persisted via core/settings.
//
// This is app-wide state that must exist before any database is open, so it
// lives in core/settings rather than inside any single database. "Remove from
// list" un-registers an entry without touching any file; actual folder
// deletion is a separate, later, UI-level destructive action and is
// deliberately not implemented here.
// @see PartManager_Settings.h
// @see docs/design/ARCHITECTURE.md §1b
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One entry in the registry — a .pmdb path and when it was last opened (ISO-8601, empty = never).
	struct PART_MANAGER_API RegisteredDatabase
	{
		std::string pmdbPath;
		std::string lastOpenedAt;
	};

	// Known-databases list, persisted app-wide via core/settings (AppSettings).
	class PART_MANAGER_API DatabaseRegistry
	{
		DatabaseRegistry() = delete;
	public:
		// Returns every registered database.
		static std::vector<RegisteredDatabase> list();
		// Registers pmdbPath if not already present (no-op if it already is). lastOpenedAt starts empty.
		static void add(const std::string& pmdbPath);
		// Un-registers pmdbPath. Touches no files — see header note, no folder deletion here.
		static void remove(const std::string& pmdbPath);
		// Updates the stored last-opened timestamp for pmdbPath (no-op if not registered).
		static void updateLastOpenedAt(const std::string& pmdbPath, const std::string& lastOpenedAtIso);

	};

}

