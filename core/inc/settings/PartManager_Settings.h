// @file PartManager_Settings.h
// @brief App-wide settings facade over the vendored AppSettings library (§1b, §9, §9a).
//
// Two groups: the known-databases list core/database's DatabaseRegistry needs
// (§1b), and the user preferences the Settings dialog edits (§9) — language,
// theme, currency, KiCad output path, and the backup schedule (§9a).
//
// **The Mouser API keys are deliberately absent.** §9 lists an API-key field in
// the Storage tab, but a key written here would land in a plain-text settings
// file in the user's data folder. Both keys are read from `MOUSER_SEARCH_API` /
// `MOUSER_CART_API` and from nowhere else; the dialog shows whether each is set
// and says how to set it, and never offers to store one.
// @see docs/design/ARCHITECTURE.md §1b, §9, §9a
// @see PartManager_DatabaseRegistry.h, PartManager_BackupManager.h
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

	// §9 theme vocabulary. Stored as TEXT so an unknown value from a future version reads back
	// as itself and falls through to the system default rather than crashing a switch.
	namespace ThemeName
	{
		constexpr const char* System = "system";
		constexpr const char* Light = "light";
		constexpr const char* Dark = "dark";
	}

	// The user preferences the Settings dialog edits (§9, §9a). One struct rather than a getter
	// per field: the dialog reads them all at once and writes them all at once, and a partial
	// write is not a state anything wants.
	struct PART_MANAGER_API AppPreferences
	{
		std::string language = "en";              // 'en' | 'de' (§8)
		std::string theme = ThemeName::System;
		std::string currency = "CHF";             // display default only; prices carry their own
		std::string kicadLibraryPath;             // where §5a writes generated libraries; empty = beside the database

		// §9a. The DB file is small and holds everything autosave can silently overwrite, so it
		// is the only thing on the automatic schedule; filestore/ and kicad_libs/ are large,
		// additive and content-hashed, and are left to a manual full-folder backup.
		bool backupsEnabled = true;
		int backupIntervalHours = 6;
		int backupRetentionCount = 20;
		std::string backupFolder;                 // empty = `backups/` inside the database folder

		// §7a's "Hide empty" tick on the category tree. A view preference rather than a §9 one —
		// the Settings dialog does not show it, the checkbox itself is the control. It lives here
		// because it is per-user and not per-database: a user who never wants to see empty
		// categories does not want to re-tick it in every database they open.
		bool hideEmptyCategories = false;
	};

	// One remembered CSV/BOM column mapping (§5). Keyed by `headerSignature` — the file's own
	// header row, lowercased and joined — rather than by path, because the thing that repeats is
	// the *shape*: next month's revision of the same schematic is a different file with the same
	// columns, and a second board exported by the same KiCad settings is too.
	//
	// Column indices, not names: the mapping is only ever replayed onto a table whose headers
	// already matched, so the positions are the same by construction. The delimiter is not
	// stored for the same reason — a different delimiter splits the header differently and
	// therefore produces a different signature, so a match already implies the same one.
	struct PART_MANAGER_API ImportMappingMemory
	{
		std::string headerSignature;
		int designators = -1;   // BomColumnMapping's NoCsvColumn; spelt out to keep
		int mpn = -1;           // core/settings free of a core/import dependency
		int mpnAlt = -1;
		int quantity = -1;
		int name = -1;
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

		// §9 preferences. Reading a never-written setting yields the struct's defaults, so a
		// fresh install behaves as if the dialog had been opened and left alone.
		static AppPreferences getPreferences();
		static void setPreferences(const AppPreferences& preferences);

		// §5 import mappings. `false` when this header shape has never been imported before, in
		// which case the caller falls back to guessMapping().
		static bool getImportMapping(const std::string& headerSignature,
			ImportMappingMemory& outMapping);
		// Replaces any entry with the same signature and writes the file. The list is trimmed to
		// MaxRememberedImportMappings, oldest use first — it is a convenience, not an archive,
		// and an unbounded one would grow the settings file forever.
		static void rememberImportMapping(const ImportMappingMemory& mapping);

		static constexpr int MaxRememberedImportMappings = 20;

		// Clamped on the way out of getPreferences() as well as on the way in, so a hand-edited
		// settings file cannot produce a 0-hour backup loop or a retention of 0 that deletes
		// every snapshot the moment it is written. Public because the dialog's spin boxes use
		// the same bounds and there should be one source for them.
		static constexpr int MinBackupIntervalHours = 1;
		static constexpr int MaxBackupIntervalHours = 24 * 7;
		static constexpr int MinBackupRetentionCount = 1;
		static constexpr int MaxBackupRetentionCount = 500;
	};

}

