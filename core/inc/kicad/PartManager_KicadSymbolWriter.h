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
// **The one symbol that is generated with real pins** is
// `derivedSymbolBlock()`: when a part has a footprint and no symbol, its pads
// already carry pin numbers, so a symbol built from them connects to the right
// copper. That is not the invented pinout above — see the function's own note.
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

	// One pin of a symbol derived from a footprint's pads (§5a). Not a spec for what the part's
	// pinout *is* — only for what its pads carry, which is all a footprint knows.
	struct PART_MANAGER_API KicadDerivedPin
	{
		std::string number;     // the pad number, verbatim: "1", "A12", "MH1"
		std::string name;       // `(pinfunction ...)`, empty when the footprint has none
		std::string type;       // `(pintype ...)`, empty when the footprint has none
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

		// The pins a footprint can honestly supply: one per **distinct** pad number, sorted.
		//
		// A pad with no number is copper with no net — a mounting hole, an NPTH, a bare paste
		// island — and is not a pin. Several pads sharing one number are one pin: a thermal pad
		// split into pieces and a ground pad broken up are both drawn that way, and one pin per
		// piece would produce a symbol nothing can wire.
		//
		// Empty when the footprint carries nothing usable, which is the caller's signal to derive
		// no symbol at all rather than to invent one.
		static std::vector<KicadDerivedPin> pinsFromFootprint(const std::string& footprintText);

		// A symbol whose pins are `pins`: a plain rectangle with the pads down its two sides.
		// Empty when `pins` is.
		//
		// **This is not the old placeholder.** That one invented a pin count, so it placed
		// silently in a schematic and was wrong on the board. These pins are the footprint's own
		// pads, numbered as the footprint numbers them, so the symbol places correctly — the
		// arrangement is arbitrary, the connectivity is not. Marked with `DerivedMarker` in its
		// description and keywords so nobody mistakes it for a symbol someone drew.
		//
		// `spec.footprint` is written out as usual. The rule that the Footprint property is left
		// empty rather than guessed from `part.package` does not apply here: this is not a guess,
		// it is the footprint the pins were read out of.
		static std::string derivedSymbolBlock(const KicadSymbolSpec& spec,
			const std::vector<KicadDerivedPin>& pins);

		// What marks a derived symbol in its description and keywords.
		static const char* const DerivedMarker;

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

		// The value of a symbol's `(property "<key>" "<value>" ...)`, empty when it has none.
		static std::string symbolProperty(const std::string& symbolBlock, const std::string& key);

		// The block with that property's value replaced, or the property appended when it was
		// absent. Everything else is byte-identical — which is what lets a vendor symbol keep its
		// real pins and graphics while still carrying PartManager's fields (§5c).
		static std::string withProperty(const std::string& symbolBlock, const std::string& key,
			const std::string& value);

		// The block renamed. KiCad ties a symbol's unit bodies to their parent by the
		// `"<Parent>_<unit>_<style>"` naming convention, so the nested `(symbol ...)` names are
		// renamed with it — renaming only the outer one produces a symbol that draws nothing.
		static std::string renamedSymbol(const std::string& symbolBlock, const std::string& newName);

		// Escapes a string for an s-expression literal: `"` and `\` only, which is all the format
		// defines. Newlines are legal inside a KiCad string and pass through.
		static std::string escape(const std::string& text);

		// A KiCad symbol name may not contain whitespace-sensitive or reserved characters; this
		// rewrites a part name into one that round-trips. Empty in, "Unnamed" out — a symbol with
		// no name makes the whole library unreadable.
		static std::string sanitizeSymbolName(const std::string& name);
	};

}
