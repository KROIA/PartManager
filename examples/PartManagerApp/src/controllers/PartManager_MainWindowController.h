// @file PartManager_MainWindowController.h
// @brief Owns the open database for the main window and answers what it needs to display (§12b).
//
// The main window body (category tree, part table, preview) is a later slice —
// this currently exposes only the open handle's identity, and is the seam the
// repositories will be reached through when it lands.
// @see docs/design/ARCHITECTURE.md §7, §12b
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include <QString>
#include <memory>

namespace PartManager
{

	class MainWindowController
	{
	public:
		explicit MainWindowController(std::unique_ptr<DatabaseHandle> handle);

		// Display name of the open database — its folder name (§1, no stored name field).
		QString databaseName() const;
		// Path of the open database's .pmdb entry file.
		QString pmdbPath() const;

	private:
		std::unique_ptr<DatabaseHandle> m_handle;
	};

}
