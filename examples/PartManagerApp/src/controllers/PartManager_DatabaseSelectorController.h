// @file PartManager_DatabaseSelectorController.h
// @brief Behavior behind the startup database selector (§1b) — the only thing the dialog talks to.
//
// Wraps DatabaseRegistry + DatabaseHandle so the view stays dumb (§12b): it
// hands over a path or a name, gets back a list row or an opened handle. Note
// that "Remove from list" un-registers only — nothing is ever deleted from
// disk here, deliberately (§1b).
// @see docs/design/ARCHITECTURE.md §1b, §12b
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include <QString>
#include <memory>
#include <vector>

namespace PartManager
{

	// How a database's schema compares to this build (§1c), decided from the .pmdb cache alone.
	enum class SchemaBadge
	{
		current,		// same version as this build — opens normally
		needsUpdate,	// older — opening backs up and migrates forward
		tooNew,			// newer than this build — refuses to open, ever
		stale,			// the .pmdb file is no longer on disk
		unreadable		// the .pmdb is there but unparsable
	};

	// One row of the selector list.
	struct DatabaseListEntry
	{
		QString name;			// the database's folder name — §1 has no stored name field
		QString pmdbPath;
		QString lastOpenedAt;	// ISO-8601, empty = never opened
		QString description;	// free text from the folder's README.md (§1b)
		int schemaVersion = 0;	// 0 when the badge is stale/unreadable
		SchemaBadge badge = SchemaBadge::unreadable;
	};

	class DatabaseSelectorController
	{
	public:
		// Every registered database, in registry order.
		std::vector<DatabaseListEntry> knownDatabases() const;

		// Schema badge for one .pmdb. Reads the entry file only — never opens the database (§1a).
		static SchemaBadge badgeOf(const QString& pmdbPath, int& outSchemaVersion);

		// The database's description — its folder's README.md minus the title line (§1b).
		static QString descriptionOf(const QString& pmdbPath);
		// Rewrites that README.md. False if it can't be written (folder gone, read-only).
		static bool setDescription(const QString& pmdbPath, const QString& description);

		// Creates `parentFolder/name`, registers it and returns it opened. nullptr on failure.
		std::unique_ptr<DatabaseHandle> createDatabase(const QString& parentFolder, const QString& name,
			QString& outErrorMessage);

		// Adds an existing .pmdb to the known list without opening it ("Browse for existing...").
		void registerDatabase(const QString& pmdbPath);

		// Un-registers a database. Touches no files (§1b).
		void removeFromList(const QString& pmdbPath);

		// Opens a registered database and stamps its last-opened time. nullptr on failure.
		std::unique_ptr<DatabaseHandle> openDatabase(const QString& pmdbPath, QString& outErrorMessage);

	private:
		// Folder name of the database a .pmdb file belongs to.
		static QString displayNameOf(const QString& pmdbPath);
		// Path of the README.md sitting next to a .pmdb file.
		static QString readmePathOf(const QString& pmdbPath);
	};

}
