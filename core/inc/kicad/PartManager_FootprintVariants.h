// @file PartManager_FootprintVariants.h
// @brief Which parts share a package but not a footprint (§5a).
//
// Footprint files are named after the package (`KicadLibraryGenerator::
// footprintNameFor`), so every 0402 in a library wants to be one
// `C_0402.kicad_mod`. Measured on the user's real database on 2026-09-27: **13
// package names were claimed by different pad layouts and nothing at all was
// shared**, because every part carries its own vendor footprint and no two are
// byte-identical. Naming alone therefore does not consolidate anything — a human
// has to look at the variants and say which one is authoritative.
//
// This is the grouping behind that screen, and it is deliberately **read-only
// and pure**: parts in, groups out, no database and no widgets. Reassigning a
// part to another variant is a later slice, and it is a write; keeping the
// reading half free of it is what lets the write be designed as a transaction
// rather than bolted onto a view.
//
// **Grouped by content hash, not by file name.** Two parts whose footprints are
// byte-identical are one variant even if they were imported from different
// vendors under different names — that is precisely the case that needs no
// decision from the user, and it has to fall out of the grouping rather than
// being spotted by eye.
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

	// One part that has a KiCad footprint attached.
	struct PART_MANAGER_API FootprintPartRef
	{
		int partId = 0;
		std::string partName;
		std::string package;         // as the part carries it; empty is its own group
		std::string contentHash;     // of the attached footprint — what variants are keyed on
		std::string relativePath;    // inside the filestore, so the viewer can read the bytes
	};

	// One distinct footprint, and every part pointing at it.
	struct PART_MANAGER_API FootprintVariant
	{
		std::string contentHash;
		std::string relativePath;
		// Sorted by part name, so the same database always produces the same list — a screen
		// whose rows move between runs cannot be compared against itself.
		std::vector<FootprintPartRef> parts;
	};

	// One package name and the footprints its parts disagree about.
	struct PART_MANAGER_API FootprintPackageGroup
	{
		std::string package;
		std::vector<FootprintVariant> variants;

		// One variant (or none) means every part with this package already agrees. This is the
		// distinction the screen is for: a consistent package is finished work, a package with
		// several variants is the user's work list.
		//
		// **A part with no package is never in conflict**, however many of them there are.
		// `footprintNameFor()` falls back to the part's own name for those, so they each write
		// their own file and none of them is claiming a name another part wants. Counting them
		// as a clash put 11 of the user's parts at the top of the work list with nothing to
		// decide about — observed on the real database, 2026-09-27.
		bool consistent() const { return package.empty() || variants.size() <= 1; }
		int partCount() const;
	};

	class PART_MANAGER_API FootprintVariants
	{
		FootprintVariants() = delete;
	public:
		// Groups by package, then by content hash. Packages are ordered by name and variants by
		// descending part count then by hash, so the variant most parts already use comes first
		// — it is the likeliest answer to "which of these is authoritative", offered by position
		// rather than by being chosen for the user.
		static std::vector<FootprintPackageGroup> group(const std::vector<FootprintPartRef>& refs);

		// Only the groups that need a decision. `group()` keeps the consistent ones because a
		// screen that shows only problems cannot show that the rest are fine.
		static std::vector<FootprintPackageGroup> inconsistentOnly(
			const std::vector<FootprintPackageGroup>& groups);

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Every part with a `kicad_footprint` attachment. Goes through PartRepository and
		// FileStore rather than its own query: repositories own the SQL, and at the size of a
		// parts database the per-part lookup is not worth a new one.
		static std::vector<FootprintPartRef> collect(SQLiteWrapper::SQLite& db);
#endif
	};

}
