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
// **Hand edits survive, and they are synced back.** Every symbol goes through
// `KicadEditTracker`: a symbol whose on-disk bytes still match what PartManager
// last wrote is regenerated, one that does not is carried across **verbatim**,
// reported, *and written into that part's own `part_file(role='kicad_symbol')`*.
// The file as a whole is rewritten every run; individual symbol bodies are not.
//
// **Every KiCad artifact therefore lives in exactly two places, byte-identical**
// (§5c): the part's attachment, and the generated bundle. Which one is the
// source depends on who touched it last —
//
//   - attachment newer (a vendor ZIP was imported) → it is spliced into the bundle
//   - bundle newer (edited in KiCad)               → it is written into the attachment
//   - neither touched                              → regenerated from the template
//
// **Nothing is invented.** A part is generated from the KiCad files it actually
// has attached, never from a placeholder:
//
//   - no attached `.kicad_sym` and no attached `.kicad_mod` → the part is not
//     generated at all
//   - symbol only   → symbol only, which is normal and useful
//   - footprint only → the footprint is written, and a symbol is derived from its
//     pads *when they carry pin numbers* — those pins are real copper, so the
//     symbol places correctly. Pads with no numbers derive nothing.
//
// A dummy symbol is worse than an absent part: it places silently in a schematic
// and is wrong on the board. Every skip is named in the result, because a user
// expecting 38 symbols and getting 12 has to be told which 26 went and that it
// was deliberate.
//
// **A part that stops qualifying takes its artifacts with it.** Its
// `kicad_generated_item` rows would otherwise outlive it and report a phantom
// "modified externally" forever. The artifact is removed and the row forgotten —
// **unless its hash says a human edited it**, in which case it is kept on disk,
// still tracked, and surfaced as `stale` so the user decides.
//
// **Only parts whose type is `kicad_relevant`, own or inherited, are generated**
// (§2b) — a child category needs the flag on some ancestor, not on itself, so
// the whole tree under a flagged root exports. A type with no `kicad_category`
// of its own likewise inherits its nearest ancestor's. A part whose type
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

	// One artifact that was left alone because someone edited it in KiCad. Surfaced so the user can
	// force-regenerate or re-baseline it, per §5a.
	struct PART_MANAGER_API KicadSkippedItem
	{
		int partId = 0;
		std::string partName;
		std::string targetPath;      // "Resistors.kicad_sym:RC0603-4K7"
		// True when the part behind it no longer qualifies for generation at all (its KiCad files
		// were detached, or the part is gone). Doing nothing already keeps such an artifact, so
		// re-baselining it is meaningless — the only real choice left is to remove it.
		bool stale = false;
	};

	// What one generation run did.
	struct PART_MANAGER_API KicadGenerationResult
	{
		bool ok = false;
		std::string errorMessage;

		// The library nicknames written this run — "Resistors", "ICs", … These are what KiCad
		// asks for when a library table is filled in by hand, so the screen has to be able to
		// show them; counting them is not enough to act on.
		std::vector<std::string> libraryNames;

		int librariesWritten = 0;
		int symbolsGenerated = 0;
		int symbolsPreserved = 0;        // hand-edited, carried across untouched
		// §5c: hand-edited in KiCad and written back into the part's own attachment, so the two
		// copies agree again. `preserved` still lists them — they were not regenerated.
		int symbolsSyncedBack = 0;
		int footprintsSyncedBack = 0;
		// Symbols taken from the part's attached `.kicad_sym` instead of the generic template.
		int symbolsFromAttachment = 0;
		// Symbols built from the pads of a part's footprint because it has no `.kicad_sym`. Worth
		// counting apart: their pins are real, their arrangement is not the part's pinout, and a
		// user who sees the number knows how many symbols are waiting to be drawn properly.
		int symbolsDerivedFromFootprint = 0;
		int footprintsCopied = 0;
		int modelsCopied = 0;
		// Parts whose type is KiCad-relevant but resolves to no category — they went nowhere and
		// the user needs to know which, rather than wondering why a part never appears in KiCad.
		std::vector<std::string> skippedForNoCategory;
		// Parts with neither an attached KiCad symbol nor an attached footprint: nothing at all was
		// generated for them, deliberately.
		std::vector<std::string> skippedForNoKicadFiles;
		// Parts that got their footprint but no symbol: no usable `.kicad_sym`, and pads with no
		// numbers to derive one from either.
		std::vector<std::string> skippedForNoSymbol;
		// Artifacts of parts that stopped qualifying: dropped from the library and forgotten.
		int staleItemsRemoved = 0;
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
