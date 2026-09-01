#include "database/PartManager_SchemaMigrator.h"
#include "PartManager_global.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_TagRepository.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	SchemaCompatibility SchemaMigrator::migrate(SQLiteWrapper::SQLite& db, int storedSchemaVersion,
		const std::string& toolVersionLastSaved, std::string& outErrorMessage)
	{
		if (storedSchemaVersion == CurrentSchemaVersion)
		{
			return SchemaCompatibility::equal;
		}
		if (storedSchemaVersion > CurrentSchemaVersion)
		{
			outErrorMessage = "This database was last saved by a newer PartManager (schema v"
				+ std::to_string(storedSchemaVersion) + ", tool v" + toolVersionLastSaved
				+ ") - this copy only supports up to schema v" + std::to_string(CurrentSchemaVersion)
				+ ". Update PartManager to open it.";
			return SchemaCompatibility::refused;
		}

		// storedSchemaVersion < CurrentSchemaVersion: run forward migrations in order.
		// TODO(core/backup): once BackupManager exists, snapshot partmanager.db here before
		// running any migration step (§9a) — the single riskiest mutation an install ever does.
		if (storedSchemaVersion < 1 && db.isOpen())
		{
			// v0 -> v1: create the core/domain + core/persistence tables (§2, §3). CREATE TABLE IF NOT
			// EXISTS, so this is also what lays down the schema on a brand-new database folder.
			// db.isOpen() guard: migrate() is only ever meaningfully called with an open connection in
			// real use (DatabaseHandle::open() calls it right after m_db->open() succeeds) — the guard
			// just keeps this a no-op for TST_SchemaMigrator's unopened test double instead of calling
			// execute() on a null connection (a latent SQLiteWrapper::SQLite::execute() bug: on failure
			// with no sqlite3-provided error message, it concatenates a null errMsg into a std::string
			// and crashes in strlen()).
			PartTypeRepository::createSchema(db);
			PartRepository::createSchema(db);
		}
		if (storedSchemaVersion < 2 && db.isOpen())
		{
			// v1 -> v2: tags (§2d). Same CREATE TABLE IF NOT EXISTS / db.isOpen() reasoning as above.
			TagRepository::createSchema(db);
		}
		return SchemaCompatibility::migrated;
	}
#endif

}
