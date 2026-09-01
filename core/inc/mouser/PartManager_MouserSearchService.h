// @file PartManager_MouserSearchService.h
// @brief MouserPart DTO -> domain Part prefill (§6). Pure mapping, no Qt, no network, no SQL.
//
// Everything here is a pure function of a `MouserPartDto`, so the whole mapping
// is unit-testable from a fixed JSON fixture without a key or a network call.
//
// Parametric values go through `ValueParser` (never a second hand-rolled
// parser) and land in the §2a `attributes` shape
// `{"key": {"value": <base-SI number>, "unit": "<dropdown unit>"}}` — always
// base-SI, never the prefix the datasheet happened to use.
//
// Category -> type template mapping is deliberately conservative: only the
// unambiguous cases are mapped and everything else comes back with an empty
// `suggestedTypeName`, so a wrong template is never silently attached to a part.
// @see docs/design/ARCHITECTURE.md §2a, §6
// @see PartManager_MouserClient.h, PartManager_ValueParser.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "mouser/PartManager_MouserClient.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One Mouser search row turned into "everything the New Part form should start out holding".
	// Part alone cannot carry this: it stores a datasheet as a file id, not a URL, and has no
	// field for the Mouser product page at all.
	struct PART_MANAGER_API MouserPartPrefill
	{
		Part part;                         // partTypeId stays 0 whenever the category could not be mapped
		std::string suggestedTypeName;     // 'MOSFET', 'Resistor', ...; empty = undecidable, leave it to the user
		std::string datasheetUrl;          // MouserPart.DataSheetUrl, for the §6 auto-download step
		std::string productDetailUrl;      // MouserPart.ProductDetailUrl — what "Open on Mouser" opens
		std::string mouserPartNumber;      // needed later for cart staging; not a Part field
		std::vector<std::string> unmappedAttributes; // Mouser AttributeNames left for manual entry
	};

	class PART_MANAGER_API MouserSearchService
	{
		MouserSearchService() = delete;
	public:
		// Full DTO -> prefill mapping. Never fails; unmappable pieces are simply left empty.
		static MouserPartPrefill toPrefill(const MouserPartDto& dto);

		// Reorders a result set so the closest match to what the user typed comes first.
		// Mouser answers "595-LM358DR" with the part itself *and* its packaging variants
		// ("595-LM358DRE4"): exact beats prefix beats substring, and among equals the
		// candidate carrying the fewest extra characters wins. Stable, so Mouser's own
		// relevance order survives inside a tier. Both the Mouser and the manufacturer
		// part number are scored; the better of the two counts.
		static void rankByMatch(std::vector<MouserPartDto>& parts, const std::string& query);

		// Mouser's `Category` -> one of the seeded type template names, or "" when ambiguous.
		static std::string suggestedTypeName(const std::string& category);

		// Mouser's ProductAttributes[] -> the §2a attributes JSON, base-SI values only.
		// Attribute names we have no mapping for, and values ValueParser rejects, are reported
		// in outUnmapped instead of being guessed at. Returns "{}" when nothing mapped.
		static std::string attributesJson(const std::vector<MouserProductAttribute>& attributes,
			std::vector<std::string>& outUnmapped);

		// Mouser writes units as words ("4.7 kOhms", "10 Volts"). Rewrites them to the §2a
		// dropdown symbols so ValueParser sees a unit it knows. Public because it is the one
		// piece worth asserting on its own.
		static std::string normalizeUnitWords(const std::string& value);
	};

}
