// @file PartManager_PartTypeFileSlot.h
// @brief Plain data class for a `part_type_file_slot` row (§2) — an expected file attachment on a type template.
//
// Same idea as PartTypeAttribute but for files rather than data fields
// (e.g. Resistor: Datasheet optional; Screw: CAD Model required). Zero Qt,
// zero SQL, zero business logic.
// @see docs/design/ARCHITECTURE.md §2, §2b
// @see PartManager_PartFile.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// One `part_type_file_slot` row — an expected file attachment on a type template.
	struct PART_MANAGER_API PartTypeFileSlot
	{
		int id = 0;                        // SQLite primary key; 0 = not yet inserted
		int partTypeId = 0;                 // FK -> PartType::id
		std::string role;    // matches part_file.role (PartFileRole), e.g. 'datasheet'|'kicad_3dmodel'|'cad_model'|'photo'
		std::string label;                  // 'Datasheet', 'CAD Model', ...
		bool required = false;             // blocks part creation until this file slot is filled
		std::string tooltip;               // (i)-hint text shown next to the file slot
		int sortOrder = 0;                  // display ordering
	};

}

