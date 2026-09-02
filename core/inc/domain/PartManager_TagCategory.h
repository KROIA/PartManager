// @file PartManager_TagCategory.h
// @brief Plain data class for a `tag_category` row (§2d) — the second level of the tag tree.
//
// A category is a heading over tags that answer the same question: *Bus protocols*
// over I2C/SPI/UART, *PCB placement* over SMD/THT. It carries no parts of its own —
// `part_tag` still points at tags — so nothing that reads a part's tags changes
// shape because categories exist.
//
// `color` is the family's base shade. A tag created under a category takes a
// lightened step of it (TagRepository::shadeOf), which is what makes one family
// read as one colour in the table without every chip being identical. The shade is
// written into `tag.color` at creation, so it stays editable per tag afterwards.
//
// Categories are optional, not required: `Tag::categoryId == NoTagCategoryId` is a
// perfectly good tag that simply belongs to no family, and every tag written before
// this table existed is one.
// @see docs/design/ARCHITECTURE.md §2d
// @see PartManager_Tag.h, PartManager_TagRepository.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	// Sentinel for TagCategory::id meaning "not yet inserted", and for Tag::categoryId
	// meaning "uncategorised".
	constexpr int NoTagCategoryId = 0;

	// One `tag_category` row — 'Bus protocols', 'PCB placement', 'Lifecycle', ...
	struct PART_MANAGER_API TagCategory
	{
		int id = NoTagCategoryId;   // SQLite primary key; NoTagCategoryId (0) = not yet inserted
		std::string name;           // unique, user-managed
		std::string color;          // hex base shade the family's tags are derived from
		int sortOrder = 0;          // display order in the tag tree
	};

}
