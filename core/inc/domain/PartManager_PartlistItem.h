// @file PartManager_PartlistItem.h
// @brief Plain data class for a `partlist_item` row (§4) — one line of a BOM.
//
// Zero Qt, zero SQL, zero business logic. `partId == NoPartId` is the §4
// **unresolved** state: an imported row that matched nothing in the inventory
// yet. Such a row keeps its original CSV/BOM line in `rawImportData` so the
// user can still see what it was supposed to be, and the partlist editor paints
// it orange until it is matched to a real part.
// @see docs/design/ARCHITECTURE.md §4
// @see PartManager_Partlist.h, PartManager_PartlistRepository.h
#pragma once

#include "PartManager_global.h"
// For NoPartlistId, which partlistId defaults to. Every caller so far happened to include
// Partlist.h first, so this header only looked self-contained.
#include "domain/PartManager_Partlist.h"
#include <string>

namespace PartManager
{

	// Sentinel for PartlistItem::id meaning "not yet inserted".
	constexpr int NoPartlistItemId = 0;
	// Sentinel for PartlistItem::partId meaning "not resolved to a part yet" (the NULL FK of §4).
	constexpr int NoPartId = 0;

	// One `partlist_item` row — a part (or an unresolved import row) needed by a partlist.
	struct PART_MANAGER_API PartlistItem
	{
		int id = NoPartlistItemId;
		int partlistId = NoPartlistId;
		int partId = NoPartId;          // NoPartId while the row is still unresolved
		std::string designators;        // 'R1,R2,R5' — user/BOM data, never parsed by core
		int quantityPerUnit = 1;        // multiplied by Partlist::multiplier for the needed qty
		std::string rawImportData;      // the original import row, kept for unresolved matches
	};

}
