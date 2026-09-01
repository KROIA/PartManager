// @file PartManager_Partlist.h
// @brief Plain data class for a `partlist` row (§4) — one BOM / build list.
//
// Zero Qt, zero SQL, zero business logic. `multiplier` is the PCB production
// count: every item's needed quantity is `quantityPerUnit * multiplier`, which
// is the number "Check Stock" diffs against `part.stock_qty` (§4).
// `source` records where the list came from and is display-only — a
// `csv_import` list behaves exactly like a `manual` one once its rows resolve.
// @see docs/design/ARCHITECTURE.md §4
// @see PartManager_PartlistItem.h, PartManager_PartlistRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for Partlist::id meaning "not yet inserted".
	constexpr int NoPartlistId = 0;

	// `partlist.source` values. TEXT in the schema, so these are the only strings that belong in it.
	namespace PartlistSource
	{
		constexpr const char* Manual = "manual";
		constexpr const char* KicadImport = "kicad_import";
		constexpr const char* CsvImport = "csv_import";
	}

	// One `partlist` row — a BOM the user builds from.
	struct PART_MANAGER_API Partlist
	{
		int id = NoPartlistId;
		std::string name;               // user-facing, freely chosen
		std::string description;
		std::string projectLinkUrl;     // repo/project page, opened in the system browser
		int multiplier = 1;             // PCB production count; needed qty = quantityPerUnit * this
		std::string source = PartlistSource::Manual;
		std::string createdAt;          // ISO-8601, DB-assigned on insert
		std::string updatedAt;          // ISO-8601, DB-assigned on insert/update
	};

}
