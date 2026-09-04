// @file PartManager_PartType.h
// @brief Plain data class for a `part_type` row (§2) — a component type template.
//
// Zero Qt, zero SQL, zero business logic. `parentTypeId == NoParentType` means
// a root type; §2b inheritance resolution (walking parent chains, merging
// attributes/file-slots) lives in PartTypeRepository, not here.
// @see docs/design/ARCHITECTURE.md §2, §2b
// @see PartManager_PartTypeAttribute.h
// @see PartManager_PartTypeFileSlot.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for PartType::parentTypeId / PartType::id meaning "no parent" / "not yet inserted".
	constexpr int NoParentType = 0;

	// One `part_type` row — a component category template ('Resistor', 'Ceramic Capacitor', ...).
	struct PART_MANAGER_API PartType
	{
		int id = NoParentType;             // SQLite primary key; NoParentType (0) = not yet inserted
		std::string name;                 // 'Resistor', 'Screw', 'Wood Sheet'
		std::string domain;                // 'electronic' | 'mechanical' | 'generic'
		bool kicadRelevant = false;        // whether this type participates in KiCad library generation
		std::string kicadCategory;         // groups types into one KiCad library file, e.g. 'Resistors'; empty = unset
		int parentTypeId = NoParentType;   // FK -> PartType::id to inherit attributes/file-slots from (§2b); NoParentType => root type
		std::string description;           // free text
		// The default search words every part of this type answers to, one per line — "R", "Res",
		// "Ohm" for a resistor (§7a). Inherited down the §2b chain and never copied onto a part,
		// so editing this reaches the parts already filed under the type; a part adds its own in
		// Part::searchKeywords rather than overriding these.
		std::string searchKeywords;
	};

}

