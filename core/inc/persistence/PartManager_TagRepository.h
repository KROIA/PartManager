// @file PartManager_TagRepository.h
// @brief CRUD for `tag`/`part_type_tag`/`part_tag` (§2d) + the one-time seed onto new parts.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartTypeRepository. `effectiveTypeDefaultTags()`
// resolves a type's default tags the §2b way — walk root ancestor -> type — but
// unions instead of overriding: tags have no key to conflict on, so every
// ancestor's tag stays, de-duplicated by tag id.
//
// `seedTagsForNewPart()` implements §2d's one-time seed: a new part copies its
// type's effective default tags into `part_tag` once, at creation (called from
// PartRepository::insertPart()). It is NOT a live link — later edits to
// `part_type_tag` never touch parts that already exist.
//
// SQLite only enforces the REFERENCES clauses with `PRAGMA foreign_keys=ON`,
// which this codebase does not set, so `deleteTag()` removes that tag's
// `part_type_tag`/`part_tag` rows explicitly rather than trusting the FKs.
// @see docs/design/ARCHITECTURE.md §2d
// @see PartManager_Tag.h, PartManager_PartTypeRepository.h, PartManager_PartRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Tag.h"
#include "domain/PartManager_TagCategory.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API TagRepository
	{
		TagRepository() = delete;
	public:
		// One step of a category's colour family: `base` blended towards white by `index`/`count`.
		// Index 0 is the base itself and the ramp stops well short of white, so the last member of
		// a ten-tag family is still visibly that colour rather than a grey chip.
		//
		// Pure string maths on `#RRGGBB` — no database, no Qt. A colour it cannot read comes back
		// unchanged, so a hand-edited category colour can never turn a tag into an empty string.
		static std::string shadeOf(const std::string& baseHex, int index, int count);

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates tag_category/tag/part_type_tag/part_tag if missing, and adds `tag.category_id`
		// to a `tag` table written before categories existed. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// tag_category CRUD. Deleting a category does NOT delete its tags — they fall back to
		// uncategorised, because a mis-click on a heading must not take a part's tags with it.
		static int insertCategory(SQLiteWrapper::SQLite& db, const TagCategory& category);
		static bool updateCategory(SQLiteWrapper::SQLite& db, const TagCategory& category);
		static bool deleteCategory(SQLiteWrapper::SQLite& db, int categoryId);
		static bool findCategory(SQLiteWrapper::SQLite& db, int categoryId, TagCategory& outCategory);
		static std::vector<TagCategory> listCategories(SQLiteWrapper::SQLite& db);

		// The tags in one family, or the uncategorised ones for NoTagCategoryId.
		static std::vector<Tag> listTagsInCategory(SQLiteWrapper::SQLite& db, int categoryId);

		// Moves a tag into a category (or out of one with NoTagCategoryId). When `recolour` is
		// set the tag also takes the next shade of its new family, which is what the tag tree's
		// re-parent does; pass false to move a tag whose colour the user chose deliberately.
		static bool setTagCategory(SQLiteWrapper::SQLite& db, int tagId, int categoryId,
			bool recolour = true);

		// tag CRUD
		// Inserts a new tag, returns its new id (NoTagId on failure, e.g. duplicate name).
		static int insertTag(SQLiteWrapper::SQLite& db, const Tag& tag);
		static bool updateTag(SQLiteWrapper::SQLite& db, const Tag& tag);
		// Also removes this tag's part_type_tag/part_tag rows (see header note on foreign keys).
		static bool deleteTag(SQLiteWrapper::SQLite& db, int tagId);
		// Looks up a single tag by id. Returns false if not found.
		static bool findTag(SQLiteWrapper::SQLite& db, int tagId, Tag& outTag);
		static std::vector<Tag> listTags(SQLiteWrapper::SQLite& db);

		// part_type_tag — a type's OWN default tags, not inherited (see effectiveTypeDefaultTags()).
		static bool addTypeDefaultTag(SQLiteWrapper::SQLite& db, int typeId, int tagId);
		static bool removeTypeDefaultTag(SQLiteWrapper::SQLite& db, int typeId, int tagId);
		static std::vector<Tag> listTypeDefaultTags(SQLiteWrapper::SQLite& db, int typeId);

		// §2d resolution: root ancestor -> typeId, ancestor tags first, union (no override), unique by id.
		static std::vector<Tag> effectiveTypeDefaultTags(SQLiteWrapper::SQLite& db, int typeId);

		// part_tag — the tags actually on one part, independently editable after creation.
		static bool addPartTag(SQLiteWrapper::SQLite& db, int partId, int tagId);
		static bool removePartTag(SQLiteWrapper::SQLite& db, int partId, int tagId);
		static std::vector<Tag> listPartTags(SQLiteWrapper::SQLite& db, int partId);
		// Replaces a part's whole tag set with tagIds.
		static bool setPartTags(SQLiteWrapper::SQLite& db, int partId, const std::vector<int>& tagIds);

		// §2d one-time seed: copies effectiveTypeDefaultTags(partTypeId) into part_tag for partId.
		// No-op (returns true) if the part already has any part_tag rows.
		static bool seedTagsForNewPart(SQLiteWrapper::SQLite& db, int partId, int partTypeId);

		// Seeds the starting vocabulary: the Bus protocols / PCB placement / Lifecycle /
		// Voltage domain / Handling families and their tags, plus the loose Favourite flag.
		//
		// Per-name and per-category, so it is safe to re-run — that is how a database created
		// before a family existed ever gets it. A tag the user renamed, recoloured or deleted is
		// theirs and is left alone. Tags seeded before categories existed (SMD, THT, Obsolete,
		// ...) are adopted into their family, and recoloured into its ramp only when they still
		// carry the exact colour this function gave them.
		//
		// No type default tags are attached: a default lands on every new part of that type, and a
		// wrong one is then on every part before anyone notices.
		static bool seedDefaultTags(SQLiteWrapper::SQLite& db);
#endif

	};

}
