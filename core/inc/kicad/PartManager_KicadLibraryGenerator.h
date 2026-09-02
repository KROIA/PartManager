// @file PartManager_KicadLibraryGenerator.h
// @brief Writes the KiCad libraries for a database's parts (§5a).
//
// One `.kicad_sym` per `part_type.kicad_category`, plus the lib-table files
// KiCad is pointed at once and never has to be reconfigured for again. Layout
// under the database folder's `kicad_libs/`:
//
//     kicad_libs/symbols/<Category>.kicad_sym
//     kicad_libs/footprints/<Category>.pretty/<name>.kicad_mod
//     kicad_libs/3dmodels/<file>
//     kicad_libs/partmanager-sym-lib-table
//     kicad_libs/partmanager-fp-lib-table
//
// **Hand edits survive.** Every symbol goes through `KicadEditTracker`: a
// symbol whose on-disk bytes still match what PartManager last wrote is
// regenerated, one that does not is carried across **verbatim** and reported.
// The file as a whole is rewritten every run; individual symbol bodies are not.
//
// **Only parts whose type is `kicad_relevant` are generated**, and a type with
// no `kicad_category` inherits its nearest ancestor's (§2b). A part whose type
// resolves to no category at all is skipped and counted, not silently dropped.
// @see docs/design/ARCHITECTURE.md §5a, §2b
// @see PartManager_KicadSymbolWriter.h, PartManager_KicadEditTracker.h
#pragma once

#include "PartManager_global.h"
#include "kicad/PartManager_KicadEditTracker.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// One symbol that was left alone because someone edited it in KiCad. Surfaced so the user can
	// force-regenerate or re-baseline it, per §5a.
	struct PART_MANAGER_API KicadSkippedItem
	{
		int partId = 0;
		std::string partName;
		std::string targetPath;      // "Resistors.kicad_sym:RC0603-4K7"
	};

	// What one generation run did.
	struct PART_MANAGER_API KicadGenerationResult
	{
		bool ok = false;
		std::string errorMessage;

		int librariesWritten = 0;
		int symbolsGenerated = 0;
		int symbolsPreserved = 0;        // hand-edited, carried across untouched
		int footprintsCopied = 0;
		int modelsCopied = 0;
		// Parts whose type is KiCad-relevant but resolves to no category — they went nowhere and
		// the user needs to know which, rather than wondering why a part never appears in KiCad.
		std::vector<std::string> skippedForNoCategory;
		std::vector<KicadSkippedItem> preserved;

		// Human summary of the counts above, for a status line.
		std::string summary() const;
	};

	class PART_MANAGER_API KicadLibraryGenerator
	{
		KicadLibraryGenerator() = delete;
	public:
		// A KiCad library name from a category: KiCad rejects a library nickname containing a
		// space or a colon, so "Power Regulators" becomes "Power_Regulators".
		static std::string libraryNameFor(const std::string& category);

		// The `partmanager-sym-lib-table` / `partmanager-fp-lib-table` contents for a set of
		// library names. `pathVariable` is the KiCad environment variable the user points at
		// `kicad_libs/` during the one-time setup — using a variable rather than an absolute path
		// is what makes the generated tables portable between machines (§5a).
		static std::string symLibTable(const std::vector<std::string>& libraryNames,
			const std::string& pathVariable);
		static std::string fpLibTable(const std::vector<std::string>& libraryNames,
			const std::string& pathVariable);

		// The environment variable the generated tables are written against.
		static const char* const PathVariable;

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Regenerates everything under `kicadLibsPath` for the parts in `db`.
		// `filestorePath` is where attached footprints and 3D models are read from.
		//
		// `forcePaths` lists target paths to overwrite even though they were edited externally —
		// the "force-regenerate, discard my edit" action. Empty is the normal run.
		static KicadGenerationResult generate(SQLiteWrapper::SQLite& db,
			const std::string& kicadLibsPath, const std::string& filestorePath,
			const std::vector<std::string>& forcePaths = std::vector<std::string>());
#endif
	};

}
