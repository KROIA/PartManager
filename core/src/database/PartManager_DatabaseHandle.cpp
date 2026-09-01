#include "database/PartManager_DatabaseHandle.h"
#include "database/PartManager_DatabaseMetadata.h"
#include "database/PartManager_SchemaMigrator.h"
#include "PartManager_info.h"
#include "PartManager_debug.h"

#include <ctime>
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{
	namespace fs = std::filesystem;

	namespace
	{
		// Current UTC time formatted as "YYYY-MM-DDTHH:MM:SS", matching §1a's example.
		std::string nowIso8601()
		{
			std::time_t t = std::time(nullptr);
			std::tm tmValue{};
#ifdef _MSC_VER
			gmtime_s(&tmValue, &t);
#else
			gmtime_r(&t, &tmValue);
#endif
			char buffer[32];
			std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tmValue);
			return std::string(buffer);
		}
	}

	DatabaseHandle::DatabaseHandle(const std::string& pmdbPath)
		: m_pmdbPath(pmdbPath)
	{
	}

	DatabaseHandle::~DatabaseHandle()
	{
		close();
	}

	std::string DatabaseHandle::siblingPath(const char* name) const
	{
		fs::path folder = fs::path(m_pmdbPath).parent_path();
		return (folder / name).string();
	}

	std::string DatabaseHandle::databaseFilePath() const { return siblingPath("partmanager.db"); }
	std::string DatabaseHandle::filestorePath() const { return siblingPath("filestore"); }
	std::string DatabaseHandle::kicadLibsPath() const { return siblingPath("kicad_libs"); }
	std::string DatabaseHandle::backupsPath() const { return siblingPath("backups"); }

	const std::string& DatabaseHandle::pmdbPath() const { return m_pmdbPath; }
	const std::string& DatabaseHandle::errorMessage() const { return m_errorMessage; }
	bool DatabaseHandle::isOpen() const { return m_isOpen; }

	bool DatabaseHandle::open()
	{
		m_errorMessage.clear();

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		m_db = std::make_unique<SQLiteWrapper::SQLite>(databaseFilePath());
		if (!m_db->open())
		{
			m_errorMessage = "Failed to open " + databaseFilePath();
			m_db.reset();
			return false;
		}
		// WAL mode is required for every database folder (§1) — SQLiteWrapper has no
		// dedicated API for this, so it's set via the standard SQLite pragma.
		m_db->execute("PRAGMA journal_mode=WAL;");

		std::string openedAt = nowIso8601();
		DatabaseMetadataValues dbMetaValues;
		if (!DatabaseMetadata::readDbMeta(*m_db, dbMetaValues))
		{
			// Brand-new database folder — leave schemaVersion at its default (0) so the migrate()
			// call below takes the "lower than current" branch and actually creates the domain
			// tables (§2/§3), rather than short-circuiting to "equal" and creating nothing.
			dbMetaValues.createdAt = openedAt;
		}

		std::string refusalMessage;
		SchemaCompatibility compatibility = SchemaMigrator::migrate(*m_db, dbMetaValues.schemaVersion,
			dbMetaValues.toolVersionLastSaved, refusalMessage);
		if (compatibility == SchemaCompatibility::refused)
		{
			m_errorMessage = refusalMessage;
			m_db->close();
			m_db.reset();
			return false;
		}
		if (compatibility == SchemaCompatibility::migrated)
		{
			dbMetaValues.schemaVersion = CurrentSchemaVersion;
		}
		dbMetaValues.lastOpenedAt = openedAt;
		dbMetaValues.toolVersionLastSaved = LibraryInfo::version.toString();

		DatabaseMetadata::writeDbMeta(*m_db, dbMetaValues);
		DatabaseMetadata::writePmdbFile(m_pmdbPath, dbMetaValues); // .pmdb is a cache, silently rewritten (§1a)

		m_isOpen = true;
		return true;
#else
		m_errorMessage = "SQLiteWrapper library not available";
		return false;
#endif
	}

	void DatabaseHandle::close()
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		if (m_db)
		{
			m_db->close();
			m_db.reset();
		}
#endif
		m_isOpen = false;
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	SQLiteWrapper::SQLite& DatabaseHandle::connection()
	{
		return *m_db;
	}
#endif

}
