// @file PartManager_KicadController.h
// @brief Widget-free logic behind the KiCad library screen (§5a, §12b).
//
// A thin wrapper over KicadLibraryGenerator and KicadEditTracker, plus the one
// piece of policy the dialog needs: where the libraries go. That is
// `AppPreferences::kicadLibraryPath` when the user set one, and the database
// folder's own `kicad_libs/` otherwise — so a fresh install generates somewhere
// sensible without being configured first.
// @see docs/design/ARCHITECTURE.md §5a, §9, §12b
// @see PartManager_KicadLibraryGenerator.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "kicad/PartManager_KicadLibraryGenerator.h"
#include <QString>
#include <string>
#include <vector>

namespace PartManager
{

	class KicadController
	{
	public:
		explicit KicadController(DatabaseHandle* handle);

		// Where libraries are written: the configured path, else the database's kicad_libs/.
		// Empty only when there is no open database and no configured path.
		std::string libraryPath() const;

		// Regenerates everything. `forcePaths` are artifacts to overwrite despite having been
		// edited in KiCad — the "discard my edit" action.
		KicadGenerationResult generate(
			const std::vector<std::string>& forcePaths = std::vector<std::string>()) const;

		// Accepts what is on disk as the new baseline without changing the file, so the artifact
		// stops being reported as edited while the edit itself is kept (§5a).
		bool rebaseline(const std::string& targetPath) const;

		// The one-time KiCad setup instructions, with the real path filled in — the user has to
		// add the generated tables to KiCad's global library tables once, and nothing in the app
		// can do it for them.
		QString setupInstructions() const;

	private:
		DatabaseHandle* m_handle;
	};

}
