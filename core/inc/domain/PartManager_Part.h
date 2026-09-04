// @file PartManager_Part.h
// @brief Plain data class for a `part` row (§2) — one physical component record.
//
// Zero Qt, zero SQL, zero business logic. `attributes` is the raw JSON text
// exactly as stored in the `part.attributes` column (§2a shape:
// `{"resistance": {"value": 4700, "unit": "Ω"}, ...}`) — PartRepository is
// what parses it to derive the `attr_*` fast-filter columns, this class just
// carries it. `storageLocation` is a deferred placeholder (§2c), unused.
// @see docs/design/ARCHITECTURE.md §2, §2a, §2c
// @see PartManager_PartType.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// One `part` row — a physical component record.
	struct PART_MANAGER_API Part
	{
		int id = 0;                        // SQLite primary key; 0 = not yet inserted
		int partTypeId = 0;                 // FK -> PartType::id, which category template this part uses
		std::string name;                   // user-facing display name, freely chosen
		std::string manufacturer;           // who makes it, e.g. "Murata"
		std::string mpn;                    // Manufacturer Part Number, the vendor's own order code
		std::string description;            // free-text notes
		// This part's *own* extra search words, one per line — the ones its category does not
		// already give it (§7a). Free text matches these as well as name/mpn/manufacturer/
		// description, so "Ohm" can find a resistor whose name says none of that. The category's
		// list is not copied in here: it applies through the type at search time, so editing a
		// category's words reaches every part already filed under it.
		std::string searchKeywords;
		std::string package;               // 'SOIC-8', 'M3x10', ...
		std::string attributes = "{}";      // raw JSON, validated against part_type_attribute at save time
		int datasheetFileId = 0;            // FK -> PartFile::id; 0 => none
		int stockQty = 0;                   // cached, derived from stock_transaction sum (later item)
		int stockMinQty = 0;                // reorder threshold
		std::string storageLocation;        // placeholder, unused for now (§2c)
		bool isActive = true;               // soft-delete/archive flag
		std::string createdAt;              // ISO-8601, DB-assigned on insert
		std::string updatedAt;              // ISO-8601, DB-assigned on insert/update
	};

}

