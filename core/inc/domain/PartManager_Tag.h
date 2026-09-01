// @file PartManager_Tag.h
// @brief Plain data class for a `tag` row (§2d) — a cross-cutting, many-to-many part grouping.
//
// Zero Qt, zero SQL, zero business logic. The `part_type_tag` (type default tags)
// and `part_tag` (a part's actual tags) link tables are pure id pairs, so they get
// no domain struct of their own — TagRepository works with plain tag ids for both.
// @see docs/design/ARCHITECTURE.md §2d
// @see PartManager_PartType.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for Tag::id meaning "not yet inserted" / "no tag".
	constexpr int NoTagId = 0;

	// One `tag` row — 'LED', 'THT', 'SMD', 'Red', 'Light source', ...
	struct PART_MANAGER_API Tag
	{
		int id = NoTagId;      // SQLite primary key; NoTagId (0) = not yet inserted
		std::string name;      // unique, user-managed vocabulary (§2d)
		std::string color;     // hex chip background, e.g. '#E53935'
		int sortOrder = 0;     // display order in the managed tag list
	};

}
