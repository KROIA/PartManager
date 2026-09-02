// @file PartManager_SearchQuery.h
// @brief §7a search-box grammar: query text -> free-text / `key op value` / `tag:` terms.
//
// Pure logic — zero Qt, zero SQL, no database. Depends only on core/units for the
// §2a value parsing, so the whole grammar is unit-testable without opening a file.
// Never throws: a malformed query comes back as ok == false with a message in
// `error` and every term list left empty, so a typo in the search box can never
// reach SQL as anything but "no query".
//
// The grammar is deliberately tiny (§7a asks for a filter box, not a query
// language): whitespace-separated terms, ANDed together, double quotes to keep
// spaces inside one term. No parentheses, no negation. The one piece of OR is a
// comma inside a single `tag:` term, which is what the tag filter tree needs to
// say "any of these" — and it composes without precedence rules, because OR can
// only ever appear inside one term and AND only ever between them.
//
//   1k5              free text, matched case-insensitively against name/mpn/manufacturer/description
//   "power supply"   same, as one term including the space
//   resistance>1k    attribute comparison, value parsed by units/ValueParser into base-SI
//   voltage<=25V     ops: = != < <= > >=
//   tag:SMD          tag filter by exact (case-insensitive) tag name
//   tag:"Do not use" tag names with spaces
//   tag:I2C,SPI      any of these tags — one whole family, or a hand-picked few
//   tag:I2C,SPI tag:SMD   (I2C or SPI) and SMD
//
// A comma always separates: a tag whose *name* contains one cannot be searched
// for as a single term. Quoting does not help, and deliberately so — quotes
// already mean "one token, spaces included" and giving them a second meaning
// inside `tag:` would make `tag:"a,b"` unreadable at a glance.
//
// @see docs/design/ARCHITECTURE.md §2a, §2d, §7a
// @see PartManager_SearchEngine.h, PartManager_ValueParser.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// Comparison operators the `key op value` term form accepts.
	enum class SearchCompareOp
	{
		Equal,
		NotEqual,
		Less,
		LessEqual,
		Greater,
		GreaterEqual
	};

	// One `key op value` term, e.g. `resistance>1k`. `value` is already base-SI (§2a), so it
	// compares directly against the part table's `attr_<key>` fast-filter column.
	struct PART_MANAGER_API SearchAttributeTerm
	{
		std::string key;                                 // attribute key, lowercased -> column `attr_<key>`
		SearchCompareOp op = SearchCompareOp::Equal;
		double value = 0.0;                              // base-SI, e.g. "1k" -> 1000
		std::string unit;                                // unit the user typed, "" when none was given
	};

	// A parsed search box. All three term lists AND together; each list's own entries AND too.
	struct PART_MANAGER_API SearchQuery
	{
		bool ok = true;                                  // false => malformed, see `error`; term lists are empty
		std::string error;                               // human-readable reason, empty when ok
		std::vector<std::string> textTerms;              // lowercased free-text substrings
		std::vector<SearchAttributeTerm> attributeTerms;
		// One entry per `tag:` term; the names inside one entry OR together, the entries AND.
		// A plain `tag:SMD` is therefore a group of one, so nothing special-cases the common case.
		std::vector<std::vector<std::string>> tagGroups;

		// True for a parse of empty/whitespace-only text: matches everything rather than nothing.
		bool isEmpty() const;

		// The one entry point. Never throws; malformed input returns ok == false.
		static SearchQuery parse(const std::string& text);

		// `text` with every `tag:` term replaced by `groups`, everything else left exactly as
		// typed. This is what a tag-picker writes back into the search box: the picker owns the
		// tag terms, the user owns the rest, and neither overwrites the other.
		//
		// Names are quoted when they need it, so the result always parses back to `groups`. A
		// name containing a comma cannot be written (see the grammar note above) and is dropped
		// rather than emitted as two tags that would then be ORed.
		static std::string withTagGroups(const std::string& text,
			const std::vector<std::vector<std::string>>& groups);
	};

}
