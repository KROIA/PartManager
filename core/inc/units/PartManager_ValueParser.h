// @file PartManager_ValueParser.h
// @brief §2a entry/search parser: typed text -> base-SI value, and back again for display.
//
// Pure logic — zero Qt, zero SQL, no dependency on any other core module. Never
// throws; every failure comes back as ValueParseResult::ok == false with value
// left at 0, so a garbage number can never reach an `attr_*` column.
//
// One parser serves both the New Part screen and the search bar, exactly as §2a
// requires. What it does NOT do is the §2a "bare mantissa match" second search
// pass — the spec calls that phase 2 and it belongs to core/search; this module
// only produces the canonical base-SI number that pass 1 compares.
// @see docs/design/ARCHITECTURE.md §2a
// @see PartManager_UnitTable.h, PartManager_PartTypeAttribute.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Outcome of parsing one typed value. Carries both halves §2a needs: the base-SI number that
	// goes into the `attr_*` column, and the human-entered mantissa/prefix/unit that stays in the
	// `attributes` JSON for round-tripping in the editor.
	struct PART_MANAGER_API ValueParseResult
	{
		bool ok = false;              // false => nothing else in here is meaningful
		double value = 0.0;           // base-SI, e.g. "4k7" -> 4700
		double mantissa = 0.0;        // the number as typed without the prefix, e.g. "4k7" -> 4.7
		std::string prefix;           // SI prefix as typed: "k", "u", "\xC2\xB5", "" when none
		std::string unit;             // canonical dropdown unit, from the explicit suffix or the field's own
		bool hasPrefix = false;
		bool hasExplicitUnit = false; // true when the user typed a unit suffix — search pass 1 keys off this
	};

	class PART_MANAGER_API ValueParser
	{
		ValueParser() = delete;
	public:
		// Parses a value typed into a field whose §2a dropdown unit is `fieldUnit` ("" = "(no unit)").
		// A unit suffix is optional; when present it must match fieldUnit case-insensitively or the
		// parse fails, so "100uF" in a Resistance field is rejected rather than stored as 0.0001 Ω.
		static ValueParseResult parse(const std::string& text, const std::string& fieldUnit);

		// Same parser with no declared unit to check against — for the search bar, where any unit
		// suffix is legal and the caller uses result.unit to narrow which columns to compare.
		static ValueParseResult parse(const std::string& text);

		// Base-SI value -> the shortest human-readable prefixed form, e.g. 4700 -> "4.7 kΩ".
		// Round-trips: parse(format(v, u), u).value == v within float tolerance.
		static std::string format(double value, const std::string& unit = std::string());
	};

}
