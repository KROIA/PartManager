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
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates tag/part_type_tag/part_tag if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

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

		// Seeds the starting tag vocabulary (SMD, THT, Favourite, Obsolete, Do not use,
		// Needs datasheet) on a fresh database. No-op (returns true) if `tag` already has rows.
		// No type default tags are attached: a default lands on every new part of that type, and a
		// wrong one is then on every part before anyone notices.
		static bool seedDefaultTags(SQLiteWrapper::SQLite& db);
#endif

	};

}
