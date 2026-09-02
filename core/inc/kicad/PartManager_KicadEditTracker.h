// @file PartManager_KicadEditTracker.h
// @brief Remembers what PartManager last wrote, so a hand edit in KiCad is never overwritten (§5a).
//
// Generated libraries are **not** disposable build output. Fixing a pin or
// nudging a courtyard in KiCad is normal, and a regeneration that silently threw
// that away would make the whole feature untrustworthy. So every generated
// artifact is tracked in `kicad_generated_item` by a hash of the content
// PartManager itself last wrote.
//
// The comparison that matters is **on-disk content vs `last_generated_hash`** —
// not on-disk vs a freshly generated candidate. Comparing against a fresh
// candidate would flag every artifact as "edited" the moment a part's value
// changed, which is the ordinary case. Comparing against what we last wrote
// answers the only question worth asking: *has anyone but us touched this?*
//
//   - **Match** → on disk is exactly what we wrote. Safe to overwrite.
//   - **Mismatch** → someone edited it in KiCad. Skip it, surface it, and let
//     the user force-regenerate (discard their edit) or re-baseline (accept the
//     on-disk version as the new baseline without changing it).
//
// Symbols all share one `.kicad_sym` per category, so they are tracked per
// *symbol* (`target_path` = "Library.kicad_sym:SymbolName") rather than per
// file; footprints are naturally one file each.
// @see docs/design/ARCHITECTURE.md §5a
// @see PartManager_KicadLibraryGenerator.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// `kicad_generated_item.item_type` vocabulary (§5a).
	namespace KicadItemType
	{
		constexpr const char* Symbol = "symbol";
		constexpr const char* Footprint = "footprint";
	}

	// What regeneration should do with one artifact.
	enum class PART_MANAGER_API KicadItemState
	{
		New,                // never generated before — write it
		Unchanged,          // on disk is exactly what we last wrote — safe to overwrite
		EditedExternally,   // someone changed it in KiCad — skip and surface it
		Missing             // we generated it, it is gone from disk now — write it again
	};

	// One `kicad_generated_item` row.
	struct PART_MANAGER_API KicadGeneratedItem
	{
		int id = 0;
		int partId = 0;
		std::string itemType = KicadItemType::Symbol;
		std::string targetPath;         // "Resistors.kicad_sym:RC0603-4K7", or a .kicad_mod path
		std::string lastGeneratedHash;
		std::string lastGeneratedAt;
	};

	class PART_MANAGER_API KicadEditTracker
	{
		KicadEditTracker() = delete;
	public:
		// The hash written into `last_generated_hash`. Same FNV-1a the filestore uses: this is a
		// change detector, not an integrity guarantee, and a collision only means one artifact is
		// wrongly considered unedited.
		static std::string hashContent(const std::string& content);

		// What to do with `targetPath`, given what is on disk right now. `onDiskContent` empty
		// means the artifact is not there.
		//
		// **`currentContent` is deliberately not a parameter.** The comparison is on-disk against
		// the recorded baseline, never against a freshly generated candidate — see the header.
		static KicadItemState stateOf(const std::string& recordedHash,
			const std::string& onDiskContent, bool wasEverGenerated);

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates kicad_generated_item if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// The recorded baseline for one artifact. False when it was never generated.
		static bool find(SQLiteWrapper::SQLite& db, const std::string& targetPath,
			KicadGeneratedItem& outItem);
		// Records (or updates) the baseline after writing `content`. Returns the row id.
		static int record(SQLiteWrapper::SQLite& db, int partId, const std::string& itemType,
			const std::string& targetPath, const std::string& content);
		// Accepts what is on disk as the new baseline without changing the file — the
		// "re-baseline" action, which stops an artifact being flagged while keeping the edit.
		static bool rebaseline(SQLiteWrapper::SQLite& db, const std::string& targetPath,
			const std::string& onDiskContent);
		// Forgets an artifact, e.g. when its part is deleted or leaves the KiCad-relevant set.
		static bool forget(SQLiteWrapper::SQLite& db, const std::string& targetPath);

		// Convenience over find() + stateOf().
		static KicadItemState stateOf(SQLiteWrapper::SQLite& db, const std::string& targetPath,
			const std::string& onDiskContent);

		static std::vector<KicadGeneratedItem> itemsForPart(SQLiteWrapper::SQLite& db, int partId);
		static std::vector<KicadGeneratedItem> allItems(SQLiteWrapper::SQLite& db);
#endif
	};

}
