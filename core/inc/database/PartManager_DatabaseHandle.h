// @file PartManager_DatabaseHandle.h
// @brief Opens/connects a single database folder (§1, §1a) and wraps its SQLiteWrapper connection.
//
// Given a `.pmdb` entry file path, resolves the fixed sibling paths
// (`partmanager.db`, `filestore/`, `kicad_libs/`, `backups/`) relative to
// wherever that file actually sits — the folder is fully relocatable/renamable
// as a unit. On open, compares schema_version (§1c) via SchemaMigrator +
// DatabaseMetadata and migrates forward or refuses as appropriate.
// @see PartManager_DatabaseMetadata.h
// @see PartManager_SchemaMigrator.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <memory>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// Opens a single database folder given its `.pmdb` entry file path.
	class PART_MANAGER_API DatabaseHandle
	{
	public:
		explicit DatabaseHandle(const std::string& pmdbPath);
		~DatabaseHandle();

		// Creates a brand-new database folder `parentFolder/name` with the §1 layout
		// (filestore/, kicad_libs/, backups/, README.md), then open()s it — which lays
		// down the schema (§1c) and writes `<name>.pmdb` — and seeds the default type
		// templates. Returns nullptr with outErrorMessage set on failure: `name` empty
		// or containing a path separator, target folder already exists and is non-empty,
		// parent folder not writable. Registering the result in DatabaseRegistry is the
		// caller's job — creating and remembering are separate concerns (§1b).
		static std::unique_ptr<DatabaseHandle> createNew(const std::string& parentFolder,
			const std::string& name, std::string& outErrorMessage);

		DatabaseHandle(const DatabaseHandle&) = delete;
		DatabaseHandle& operator=(const DatabaseHandle&) = delete;

		// Opens the database: connects partmanager.db, sets WAL mode, checks/migrates schema (§1c).
		// Returns false on failure (see errorMessage()) — including a "newer schema" refusal.
		bool open();
		// Closes the underlying SQLite connection, if open.
		void close();
		// True after a successful open() that hasn't been close()d since.
		bool isOpen() const;

		// The last error/refusal message set by open(), empty if none.
		const std::string& errorMessage() const;

		// The .pmdb entry file path this handle was constructed with.
		const std::string& pmdbPath() const;
		// Path to partmanager.db, sibling to the .pmdb file.
		std::string databaseFilePath() const;
		// Path to the filestore/ folder, sibling to the .pmdb file.
		std::string filestorePath() const;
		// Path to the kicad_libs/ folder, sibling to the .pmdb file.
		std::string kicadLibsPath() const;
		// Path to the backups/ folder, sibling to the .pmdb file.
		std::string backupsPath() const;

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// The underlying SQLite connection. Only valid while isOpen() is true.
		SQLiteWrapper::SQLite& connection();
#endif

	private:
		// Resolves a fixed sibling name relative to the .pmdb file's own folder.
		std::string siblingPath(const char* name) const;

		std::string m_pmdbPath;
		std::string m_errorMessage;
		bool m_isOpen = false;

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		std::unique_ptr<SQLiteWrapper::SQLite> m_db;
#endif
	};

}

