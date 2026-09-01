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

	// One row of the selector list.
	struct DatabaseListEntry
	{
		QString name;			// the database's folder name — §1 has no stored name field
		QString pmdbPath;
		QString lastOpenedAt;	// ISO-8601, empty = never opened
	};

	class DatabaseSelectorController
	{
	public:
		// Every registered database, in registry order.
		std::vector<DatabaseListEntry> knownDatabases() const;

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
	};

}
