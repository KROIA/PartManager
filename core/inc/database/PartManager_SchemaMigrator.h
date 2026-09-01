// @file PartManager_SchemaMigrator.h
// @brief Forward-only schema migration for a single database (§1c).
//
// Compares a database's stored schema_version against CurrentSchemaVersion
// and either opens as-is (equal), migrates forward in order (lower), or
// refuses to open (higher — no downgrade path, ever). Migration steps in
// migrate() are ordered and cumulative — a v0 database runs every block in
// turn, so a step is never rewritten once shipped, only appended after.
//   v1: core/domain + core/persistence tables (§2, §3).
//   v2: tag/part_type_tag/part_tag (§2d).
//   v3: stock_transaction (§3) + the opening-balance backfill for parts whose
//       stock_qty was written directly, before the log existed.
// @see docs/design/ARCHITECTURE.md §1c
#pragma once

#include "PartManager_global.h"
#include <string>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// The schema (table/column structure) version this build of PartManager understands.
	constexpr int CurrentSchemaVersion = 3;

	// Outcome of comparing a database's stored schema_version against CurrentSchemaVersion.
	enum class SchemaCompatibility
	{
		equal,		// already current, open normally
		migrated,	// was lower, forward migration ran successfully
		refused		// was higher than this build supports, refused to open
	};

	class PART_MANAGER_API SchemaMigrator
	{
		SchemaMigrator() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Compares storedSchemaVersion to CurrentSchemaVersion and migrates db forward if needed.
		// outErrorMessage is set to the §1c refusal wording when the DB is newer than this build.
		static SchemaCompatibility migrate(SQLiteWrapper::SQLite& db, int storedSchemaVersion,
			const std::string& toolVersionLastSaved, std::string& outErrorMessage);
#endif

	};

}

