// @file PartManager_DatabaseMetadata.h
// @brief Reads/writes a database's `.pmdb` JSON entry file and its `db_meta` SQL table (§1a).
//
// `db_meta` inside partmanager.db is the trusted source; the `.pmdb` file next
// to it is a fast cache silently rewritten to match on every open, so a
// database picker can show a schema-version badge without opening the DB.
// @see docs/design/ARCHITECTURE.md §1a
#pragma once

#include "PartManager_global.h"
#include <string>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// The four flat fields stored in both the `.pmdb` file and the `db_meta` table.
	struct PART_MANAGER_API DatabaseMetadataValues
	{
		int schemaVersion = 0;
		std::string toolVersionLastSaved;
		std::string createdAt;
		std::string lastOpenedAt;
	};

	// Reads/writes DatabaseMetadataValues to/from the `.pmdb` JSON file and the `db_meta` SQL table.
	class PART_MANAGER_API DatabaseMetadata
	{
		DatabaseMetadata() = delete;
	public:
		// Reads the .pmdb JSON entry file. Returns false if the file is missing or malformed.
		static bool readPmdbFile(const std::string& pmdbPath, DatabaseMetadataValues& outValues);
		// Writes/overwrites the .pmdb JSON entry file.
		static bool writePmdbFile(const std::string& pmdbPath, const DatabaseMetadataValues& values);

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Reads db_meta rows from an already-open connection. Returns false if the table doesn't exist yet.
		static bool readDbMeta(SQLiteWrapper::SQLite& db, DatabaseMetadataValues& outValues);
		// Creates db_meta if missing and writes/overwrites every value as a row.
		static bool writeDbMeta(SQLiteWrapper::SQLite& db, const DatabaseMetadataValues& values);
#endif

	};

}

