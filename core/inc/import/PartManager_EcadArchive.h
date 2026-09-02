// @file PartManager_EcadArchive.h
// @brief Pulls the KiCad files out of a vendor ECAD download (§5c).
//
// SamacSys / Component Search Engine / Ultra Librarian ship one ZIP per part
// holding the same component for twenty CAD tools. Measured against a real one
// (`LIB_74HC4051PW-Q100,11(5).zip`, 69 entries):
//
//     74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.kicad_sym    <- symbol
//     74HC4051PW-Q100,11/KiCad/SOP65P640X110-16N.kicad_mod     <- footprint
//     74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.lib + .dcm   <- KiCad 5, ignored
//     74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.mod          <- KiCad 5, ignored
//     74HC4051PW-Q100,11/3D/74HC4051PW-Q100,11.stp             <- model, NOT under KiCad/
//     74HC4051PW-Q100,11/{Altium,EAGLE,PADS,Proteus,...}/       <- ignored
//
// Two things that listing teaches, and that a guess would have got wrong: the
// 3D model lives **outside** the KiCad folder, and `.lib` is not a KiCad
// extension on its own — `CADSTAR/74HC4051PW-Q100_11.lib` is in the same
// archive. Legacy KiCad 5 files are therefore only recognised **inside a
// KiCad folder**, and are reported rather than imported: PartManager writes
// `.kicad_sym`, and mixing formats in one library is what makes KiCad refuse to
// load it.
//
// `classify()` is a pure function of the entry names, so the whole selection is
// unit-tested against that real listing without a ZIP or a network. Reading the
// archive itself needs Qt.
// @see docs/design/ARCHITECTURE.md §5a, §5c
// @see PartManager_KicadLibraryGenerator.h, PartManager_FileStore.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// Which entries of an archive PartManager wants. Each is empty when the archive has none.
	struct PART_MANAGER_API EcadArchiveContents
	{
		bool ok = false;
		std::string errorMessage;

		std::string symbolEntry;        // a `.kicad_sym`
		std::string footprintEntry;     // a `.kicad_mod`
		std::string modelEntry;         // a `.step` / `.stp` / `.wrl`

		// A KiCad folder was there but held only KiCad 5 files (`.lib`/`.dcm`/`.mod`). Worth
		// saying out loud: the archive *does* have KiCad support and PartManager still took
		// nothing, which otherwise looks like a broken importer.
		bool legacyKicadOnly = false;
		int ignoredEntries = 0;         // everything belonging to other CAD tools

		// True when there is at least one file worth attaching.
		bool hasAnything() const
		{
			return !symbolEntry.empty() || !footprintEntry.empty() || !modelEntry.empty();
		}
	};

	// The bytes behind `contents`, read in one pass over the archive.
	struct PART_MANAGER_API EcadArchivePayload
	{
		EcadArchiveContents contents;
		std::string symbolBytes;
		std::string footprintBytes;
		std::string modelBytes;
		// Base names, kept so the stored `part_file.original_filename` is the vendor's own.
		std::string symbolName;
		std::string footprintName;
		std::string modelName;
	};

	class PART_MANAGER_API EcadArchive
	{
		EcadArchive() = delete;
	public:
		// Picks the KiCad files out of a list of archive entry paths. Pure — no ZIP, no disk.
		// When several entries could serve a role the one inside a KiCad folder wins, then the
		// shallowest path, then the shortest name, so the choice is stable rather than
		// whichever the archive happened to list first.
		static EcadArchiveContents classify(const std::vector<std::string>& entryPaths);

		// True when `path` has a segment that is exactly "kicad", case-insensitively.
		static bool isUnderKicadFolder(const std::string& path);

#if QT_ENABLED
		// Opens `zipPath`, classifies it, and reads out whatever it found.
		// ponytail: QZipReader is a QtGui *private* header — Qt 5 publishes no zip API at all,
		// and core already links Gui, so this costs one include path rather than a dependency.
		// Ceiling: private API can change between Qt minor versions; it is used through these two
		// functions only, so swapping in a real zip library is a one-file change.
		static EcadArchivePayload read(const std::string& zipPath);
#endif
	};

}
