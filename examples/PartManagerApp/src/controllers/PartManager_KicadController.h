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

		// The one-time KiCad setup instructions, with the real path filled in — for anyone who
		// would rather wire the libraries up by hand than let install() do it.
		QString setupInstructions() const;

		// KiCad's per-version configuration folders, newest first — `%APPDATA%/kicad/9.0` and
		// friends. Empty when KiCad has never run on this machine, which is a normal state and
		// the reason install() takes a folder rather than finding one itself.
		static QStringList kicadConfigDirs();

		// Writes `sym-lib-table` / `fp-lib-table` in `tableFolder`, merging our libraries in
		// beside whatever is already there (see KicadLibTable). `tableFolder` is a KiCad config
		// folder for a global install, or a project folder for a project-only one.
		//
		// **Both need the path variable**, even the project install: the copied footprints point
		// their `(model ...)` at `${PARTMANAGER_KICAD_LIBS}/3dmodels/...`, so without it the
		// symbols and footprints resolve and the 3D models do not. `setPathVariable` writes it
		// into `kicad_common.json`, which is global by nature — KiCad has no project-scoped
		// path variables.
		//
		// Every file touched is copied to `<name>.bak` first. Returns false with a reason in
		// `outError`; a partial install is reported rather than half-claimed.
		bool install(const QString& tableFolder, bool setPathVariable, QString* outError) const;

		// The libraries as they were last generated, for a table written without regenerating.
		QStringList lastLibraryNames() const;

	private:
		// The nicknames of the libraries currently on disk under libraryPath()/symbols.
		QStringList libraryNamesOnDisk() const;
		// Every KiCad version folder under one candidate root, newest major first.
		static void appendConfigDirsUnder(const QString& root, QStringList& found);

		DatabaseHandle* m_handle;
	};

}
