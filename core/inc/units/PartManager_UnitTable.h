// @file PartManager_UnitTable.h
// @brief The §2a fixed unit dropdown and the SI-prefix table. Pure data + lookups.
//
// Zero Qt, zero SQL, no dependency on any other core module. `unit` strings here
// are exactly the values stored in `part_type_attribute.unit` (see
// PartManager_PartTypeAttribute.h) — "(no unit)" is the empty string.
//
// Symbols that are multi-byte in UTF-8 are hex-escaped constants rather than
// literal characters in the source, so the file's encoding and MSVC's
// source-charset guess can never silently mangle them. Everything in this
// module treats std::string as a byte sequence; the only multi-byte tokens are
// OhmSymbol and MicroSign and both are matched as whole substrings, never by
// indexing one char at a time.
// @see docs/design/ARCHITECTURE.md §2a
// @see PartManager_ValueParser.h, PartManager_PartTypeAttribute.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API UnitTable
	{
		UnitTable() = delete;
	public:
		// U+03A9 GREEK CAPITAL LETTER OMEGA, UTF-8. Matches the docs and the mockups.
		static const char* const OhmSymbol;
		// U+00B5 MICRO SIGN, UTF-8. Interchangeable with the ASCII 'u' prefix.
		static const char* const MicroSign;

		// The §2a dropdown, in spec order. Index 0 is "" = "(no unit)".
		static const std::vector<std::string>& units();

		// True for any dropdown value, including "" ("(no unit)").
		static bool isValidUnit(const std::string& unit);

		// Case-insensitive unit compare ("uf" == "F"), ASCII-only folding.
		// Multi-byte symbols are unaffected because UTF-8 continuation bytes are never A-Z/a-z.
		static bool unitsEqual(const std::string& a, const std::string& b);

		// If `text` ends with a known unit symbol, returns its length in bytes and writes the
		// canonical dropdown spelling to outUnit. Longest match wins ("mm" before "m"-that-isn't,
		// "Hz" before "H"). Returns 0 when no unit suffix is present.
		static size_t matchUnitSuffix(const std::string& text, std::string& outUnit);

		// SI prefix at byte offset `pos`. Writes the multiplier and the token's byte length
		// ('u' = 1, MicroSign = 2). CASE-SENSITIVE: 'm' is milli, 'M' is mega.
		static bool prefixAt(const std::string& text, size_t pos, double& outMultiplier, size_t& outLength);

		// Canonical prefix for a power-of-ten exponent (-12, -9, -6, -3, 0, 3, 6, 9).
		// Returns "" for 0 and for any exponent outside the table.
		static std::string prefixForExponent(int exponent);
	};

}
