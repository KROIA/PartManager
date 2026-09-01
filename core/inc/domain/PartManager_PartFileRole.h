// @file PartManager_PartFileRole.h
// @brief Fixed vocabulary for `part_file.role` / `part_type_file_slot.role` (§3).
//
// Zero Qt, zero SQL, zero business logic. The DB column stays a free-form
// TEXT — this enum + its conversion functions exist only to catch typos at
// the C++ call site; unknown/legacy strings still round-trip via Other.
// @see docs/design/ARCHITECTURE.md §3
// @see PartManager_PartFile.h
// @see PartManager_PartTypeFileSlot.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// One `part_file.role` / `part_type_file_slot.role` value.
	enum class PART_MANAGER_API PartFileRole
	{
		Datasheet,
		KicadSymbol,
		KicadFootprint,
		Kicad3DModel,
		Image,
		Other
	};

	// PartFileRole <-> the TEXT value stored in part_file.role / part_type_file_slot.role.
	PART_MANAGER_API std::string toString(PartFileRole role);
	PART_MANAGER_API PartFileRole partFileRoleFromString(const std::string& text);

}
