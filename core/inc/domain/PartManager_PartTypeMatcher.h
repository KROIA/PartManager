// @file PartManager_PartTypeMatcher.h
// @brief Picks the part type a vendor category / description belongs to (§2, §6) — or none at all.
//
// The piece that was missing between MouserSearchService::suggestedTypeName()
// and the New Part form. suggestedTypeName() answers with one of *its own*
// hardcoded template names, and the dialog then looked that string up in the
// type combo by exact text — so a type the user made themselves ("Neopixel 5050
// WS2812B", "Elko") could never win, however obviously the category named it,
// and a renamed built-in stopped matching entirely.
//
// This matches against the types that are actually in the database instead. It
// takes the type list as a plain vector, so it is pure: no SQL, no Qt, no
// network, and provable from three fake types in a unit test. It lives under
// domain/ rather than persistence/ or mouser/ for that reason — it is a rule
// about PartType, not about how types are stored or where the text came from,
// and the KiCad/import paths can reuse it without dragging a vendor client in.
//
// The one rule that outranks every heuristic below: **never guess a type just to
// have one.** A wrong category silently attaches the wrong attribute template,
// so the part gets fields it does not have and loses the ones it does — worse
// than an empty combo the user has to fill in, which at least says what it wants.
// Anything that does not match well comes back `confident == false`.
// @see docs/design/ARCHITECTURE.md §2, §2b, §6
// @see PartManager_PartType.h, PartManager_MouserSearchService.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_PartType.h"
#include <string>
#include <vector>

namespace PartManager
{

	// What matchPartType() concluded. `typeId == 0` with `confident == false` is the honest
	// "no idea" answer and is expected to be common — an IC, a connector or a mechanical part
	// frequently matches no template at all.
	struct PART_MANAGER_API TypeMatch
	{
		int typeId = 0;            // PartType::id, 0 = nothing matched
		bool confident = false;    // false => leave the combo on "(none)" and let the user pick
		// A stable ascii key naming *why* it matched, never a display string: "suggested-type-name",
		// "category-exact", "category-word", "description-word", "synonym-suggested",
		// "synonym-category", "synonym-description". For logging and tests; core does not translate.
		std::string matchedOn;
	};

	// Best match wins, most specific strategy first:
	//   1. a type's name equal (case- and punctuation-insensitively) to `suggestedTypeName`, then
	//      to `mouserCategory`;
	//   2. a type's name appearing as whole words inside `mouserCategory`, then inside
	//      `description`;
	//   3. a synonym of a type's name ("light emitting diode" for LED, "fet" for MOSFET) in the
	//      same three places.
	// Within one strategy the longest — i.e. most specific — name wins, and a type that inherits
	// from another matching type beats its ancestor ("Ceramic Capacitor" over "Capacitor") however
	// the lengths come out. Simple plural folding is applied on both sides, so "MOSFETs" in a
	// category finds the "MOSFET" template.
	//
	// Word matching needs at least three characters of type name, so a two-letter template ("IC")
	// cannot be triggered by a coincidence in a description; it can still be matched exactly.
	PART_MANAGER_API TypeMatch matchPartType(const std::vector<PartType>& types,
		const std::string& mouserCategory, const std::string& description,
		const std::string& suggestedTypeName);

}
