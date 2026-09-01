// @file PartManager_PartTypeListColumn.h
// @brief Plain data class for a `part_type_list_column` row (§7b) — one saved table column.
//
// Zero Qt, zero SQL, zero business logic. A type with no rows at all is the
// normal state: the Home table then derives its columns from the type's
// effective attributes exactly as it did before this table existed, and rows
// only appear once the user customizes something.
//
// `columnKey` is either a built-in key ('name', 'manufacturer', 'mpn',
// 'package', 'stock_qty', ...) or a `part_type_attribute.key` of that type.
// A key that no longer resolves to either is simply ignored on read.
// @see docs/design/ARCHITECTURE.md §7b, §2b
// @see PartManager_ListColumnRepository.h, PartManager_PartTypeAttribute.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for PartTypeListColumn::id meaning "not yet inserted".
	constexpr int NoListColumnId = 0;

	// One `part_type_list_column` row.
	struct PART_MANAGER_API PartTypeListColumn
	{
		int id = NoListColumnId;
		int partTypeId = 0;
		std::string columnKey;
		std::string labelOverride;  // empty = use the built-in/attribute label
		bool visible = true;
		int sortOrder = 0;
		int widthPx = 0;            // 0 = no saved width, size the column to its contents
	};

}
