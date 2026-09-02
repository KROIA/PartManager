// @file PartManager_KicadSymbolWriter.h
// @brief Generates `.kicad_sym` symbol libraries from parts (§5a). Pure text, no filesystem.
//
// One library per `part_type.kicad_category`, one symbol per KiCad-relevant
// part. Everything here is string in, string out, so the whole format is
// testable without writing a file or owning a KiCad install.
//
// **Symbols are derived, not drawn.** PartManager stores no pin data, so
// inventing a symbol body per part would mean inventing pin counts — which
// would produce confident, wrong schematic symbols. Instead each generated
// library embeds a handful of **base symbols** (resistor, capacitor, inductor,
// diode, LED, transistor, a generic box) and every part is
// `(extends "<base>")`, carrying its own Value / Footprint / Datasheet /
// `PM_PartID` / `Mouser P/N` fields. KiCad resolves `extends` within the same
// library file, which is why the bases are embedded rather than referenced.
//
// A part whose type maps to no base gets the generic box. That is honest: the
// user places a real, working symbol and fixes the pinout in KiCad if they care,
// and §5a's edit tracker then leaves their fix alone.
//
// The format targets the KiCad 9 `kicad_symbol_lib` s-expression
// (`version 20241209`), which KiCad 7/8 also read.
// @see docs/design/ARCHITECTURE.md §5a
// @see PartManager_KicadLibraryGenerator.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include <string>
#include <vector>

namespace PartManager
{

	// Everything one generated symbol needs. Assembled by the generator from a Part plus its
	// type, datasheet and seller link, so this header needs no database.
	struct PART_MANAGER_API KicadSymbolSpec
	{
		std::string name;               // the symbol's name in the library — the part's name
		std::string baseSymbol;         // which embedded base to extend; empty = the generic box
		std::string reference = "U";    // schematic reference prefix: R, C, L, D, Q, U
		std::string value;              // shown on the schematic — usually the MPN
		std::string footprint;          // "Library:Footprint", empty when unknown
		std::string datasheet;          // URL or a path into the filestore
		std::string description;
		std::string keywords;
		int partId = 0;                 // written as PM_PartID, the round-trip back to this app
		std::string mouserPartNumber;   // written as "Mouser P/N"
		std::string model3DPath;        // written as PM_3DModel, ${KIPRJMOD}-relative where possible
	};

	class PART_MANAGER_API KicadSymbolWriter
	{
		KicadSymbolWriter() = delete;
	public:
		// The reference prefix and base symbol a part type maps to, by name. Conservative in the
		// same way MouserSearchService::suggestedTypeName() is: an unrecognised type gets the
		// generic box and a "U" prefix rather than a guess that looks authoritative.
		static std::string baseSymbolForType(const std::string& typeName);
		static std::string referenceForBase(const std::string& baseSymbol);

		// The name every generic part extends when its type maps to nothing.
		static const char* const GenericBaseSymbol;

		// The base symbols a library must embed, as complete `(symbol ...)` blocks. Every symbol
		// a generated library extends is in here, or KiCad reports a broken library.
		static std::vector<std::string> baseSymbolBlocks();

		// One `(symbol ...)` block for a part, as `(extends "<base>")` plus its properties.
		// Indented to sit directly inside a `(kicad_symbol_lib ...)`.
		static std::string symbolBlock(const KicadSymbolSpec& spec);

		// A complete library file: header, every base symbol, then `symbols` verbatim. Taking the
		// symbol blocks as text rather than specs is what lets the generator preserve
		// hand-edited symbols byte for byte alongside freshly generated ones (§5a).
		static std::string library(const std::vector<std::string>& symbolBlocks);

		// The symbol name of a `(symbol "..." ...)` block, empty when it is not one. Used to
		// match on-disk symbols against generated ones when merging a library.
		static std::string symbolNameOf(const std::string& symbolBlock);

		// Splits an existing library into its top-level `(symbol ...)` blocks, verbatim. This is
		// what makes preserving a hand edit possible: the block is never re-serialised, it is
		// carried across as the exact bytes KiCad wrote.
		static std::vector<std::string> splitSymbols(const std::string& libraryText);

		// Escapes a string for an s-expression literal: `"` and `\` only, which is all the format
		// defines. Newlines are legal inside a KiCad string and pass through.
		static std::string escape(const std::string& text);

		// A KiCad symbol name may not contain whitespace-sensitive or reserved characters; this
		// rewrites a part name into one that round-trips. Empty in, "Unnamed" out — a symbol with
		// no name makes the whole library unreadable.
		static std::string sanitizeSymbolName(const std::string& name);
	};

}
