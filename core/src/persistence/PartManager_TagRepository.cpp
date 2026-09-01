#include "persistence/PartManager_TagRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_set>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		Tag rowToTag(const std::vector<std::string>& row)
		{
			Tag tag;
			tag.id = std::atoi(row[0].c_str());
			tag.name = row[1];
			tag.color = row[2];
			tag.sortOrder = std::atoi(row[3].c_str());
			return tag;
		}

		// Root ancestor -> typeId, typeId last. Same walk as PartTypeRepository's effective* resolution
		// (its own copy is file-local there); stops early if a parent cycle is found.
		std::vector<int> ancestorChainRootFirst(SQLiteWrapper::SQLite& db, int typeId)
		{
			std::vector<int> chain;
			std::unordered_set<int> visited;
			int current = typeId;
			while (current != NoParentType && visited.find(current) == visited.end())
			{
				visited.insert(current);
				chain.push_back(current);
				PartType type;
				if (!PartTypeRepository::findType(db, current, type))
				{
					break;
				}
				current = type.parentTypeId;
			}
			std::reverse(chain.begin(), chain.end());
			return chain;
		}

		// tag rows joined through a link table, ordered the same way listTags() is.
		std::vector<Tag> linkedTags(SQLiteWrapper::SQLite& db, const std::string& linkTable,
			const std::string& ownerColumn, int ownerId)
		{
			std::vector<Tag> result;
			for (const std::vector<std::string>& row : db.fetchAll(
				"SELECT t.id,t.name,t.color,t.sort_order FROM tag t "
				"JOIN " + linkTable + " l ON l.tag_id=t.id "
				"WHERE l." + ownerColumn + "=" + std::to_string(ownerId) + " ORDER BY t.sort_order,t.name;"))
			{
				result.push_back(rowToTag(row));
			}
			return result;
		}
	}

	bool TagRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = true;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS tag ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL UNIQUE,"
			"color TEXT NOT NULL,"
			"sort_order INTEGER NOT NULL DEFAULT 0"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_type_tag ("
			"part_type_id INTEGER NOT NULL REFERENCES part_type(id),"
			"tag_id INTEGER NOT NULL REFERENCES tag(id),"
			"PRIMARY KEY(part_type_id, tag_id)"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_tag ("
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"tag_id INTEGER NOT NULL REFERENCES tag(id),"
			"PRIMARY KEY(part_id, tag_id)"
			");") && ok;
		return ok;
	}

	int TagRepository::insertTag(SQLiteWrapper::SQLite& db, const Tag& tag)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO tag (name, color, sort_order) VALUES (?, ?, ?);",
			{ tag.name, tag.color, std::to_string(tag.sortOrder) });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoTagId;
	}

	bool TagRepository::updateTag(SQLiteWrapper::SQLite& db, const Tag& tag)
	{
		return db.executeWithParams(
			"UPDATE tag SET name=?, color=?, sort_order=? WHERE id=?;",
			{ tag.name, tag.color, std::to_string(tag.sortOrder), std::to_string(tag.id) });
	}

	bool TagRepository::deleteTag(SQLiteWrapper::SQLite& db, int tagId)
	{
		std::string id = std::to_string(tagId);
		bool ok = db.executeWithParams("DELETE FROM part_type_tag WHERE tag_id=?;", { id });
		ok = db.executeWithParams("DELETE FROM part_tag WHERE tag_id=?;", { id }) && ok;
		ok = db.executeWithParams("DELETE FROM tag WHERE id=?;", { id }) && ok;
		return ok;
	}

	bool TagRepository::findTag(SQLiteWrapper::SQLite& db, int tagId, Tag& outTag)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,name,color,sort_order FROM tag WHERE id=" + std::to_string(tagId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outTag = rowToTag(rows.front());
		return true;
	}

	std::vector<Tag> TagRepository::listTags(SQLiteWrapper::SQLite& db)
	{
		std::vector<Tag> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,name,color,sort_order FROM tag ORDER BY sort_order,name;"))
		{
			result.push_back(rowToTag(row));
		}
		return result;
	}

	bool TagRepository::addTypeDefaultTag(SQLiteWrapper::SQLite& db, int typeId, int tagId)
	{
		return db.executeWithParams(
			"INSERT OR IGNORE INTO part_type_tag (part_type_id, tag_id) VALUES (?, ?);",
			{ std::to_string(typeId), std::to_string(tagId) });
	}

	bool TagRepository::removeTypeDefaultTag(SQLiteWrapper::SQLite& db, int typeId, int tagId)
	{
		return db.executeWithParams(
			"DELETE FROM part_type_tag WHERE part_type_id=? AND tag_id=?;",
			{ std::to_string(typeId), std::to_string(tagId) });
	}

	std::vector<Tag> TagRepository::listTypeDefaultTags(SQLiteWrapper::SQLite& db, int typeId)
	{
		return linkedTags(db, "part_type_tag", "part_type_id", typeId);
	}

	std::vector<Tag> TagRepository::effectiveTypeDefaultTags(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<Tag> accumulated;
		std::unordered_set<int> seen;
		for (int level : ancestorChainRootFirst(db, typeId))
		{
			// Union, not override (§2d): tags have no key to conflict on, so an ancestor's tag
			// simply stays and a redeclaration on the child is a duplicate to drop.
			for (const Tag& tag : listTypeDefaultTags(db, level))
			{
				if (seen.insert(tag.id).second)
				{
					accumulated.push_back(tag);
				}
			}
		}
		return accumulated;
	}

	bool TagRepository::addPartTag(SQLiteWrapper::SQLite& db, int partId, int tagId)
	{
		return db.executeWithParams(
			"INSERT OR IGNORE INTO part_tag (part_id, tag_id) VALUES (?, ?);",
			{ std::to_string(partId), std::to_string(tagId) });
	}

	bool TagRepository::removePartTag(SQLiteWrapper::SQLite& db, int partId, int tagId)
	{
		return db.executeWithParams(
			"DELETE FROM part_tag WHERE part_id=? AND tag_id=?;",
			{ std::to_string(partId), std::to_string(tagId) });
	}

	std::vector<Tag> TagRepository::listPartTags(SQLiteWrapper::SQLite& db, int partId)
	{
		return linkedTags(db, "part_tag", "part_id", partId);
	}

	bool TagRepository::setPartTags(SQLiteWrapper::SQLite& db, int partId, const std::vector<int>& tagIds)
	{
		// ponytail: delete-all + re-insert, not a diff — a part carries a handful of tags, and the
		// whole thing is one autosave-sized write. Diff it only if this ever runs per keystroke.
		bool ok = db.executeWithParams("DELETE FROM part_tag WHERE part_id=?;", { std::to_string(partId) });
		for (int tagId : tagIds)
		{
			ok = addPartTag(db, partId, tagId) && ok;
		}
		return ok;
	}

	bool TagRepository::seedTagsForNewPart(SQLiteWrapper::SQLite& db, int partId, int partTypeId)
	{
		// PartRepository::insertPart() calls this unconditionally, but PartRepository::createSchema()
		// can be used on its own (without TagRepository::createSchema()) — don't query tables that
		// aren't there.
		if (db.fetchAll("SELECT name FROM sqlite_master WHERE type='table' AND name='part_tag';").empty())
		{
			return true;
		}
		if (!db.fetchAll("SELECT tag_id FROM part_tag WHERE part_id=" + std::to_string(partId) + " LIMIT 1;").empty())
		{
			return true; // already has tags, this is a one-time seed (§2d) — don't touch them
		}
		bool ok = true;
		for (const Tag& tag : effectiveTypeDefaultTags(db, partTypeId))
		{
			ok = addPartTag(db, partId, tag.id) && ok;
		}
		return ok;
	}

	bool TagRepository::seedDefaultTags(SQLiteWrapper::SQLite& db)
	{
		// A starting vocabulary, not a taxonomy: mounting style, lifecycle, and the two flags every
		// bench ends up wanting. Tags are cross-cutting (§2d), so these deliberately say nothing about
		// what a part *is* — that is what the type template is for.
		struct SeedTag
		{
			const char* name;
			const char* color;
		};
		static const SeedTag seedTags[] = {
			{ "SMD",             "#1E88E5" },
			{ "THT",             "#43A047" },
			{ "Favourite",       "#FDD835" },
			{ "Obsolete",        "#E53935" },
			{ "Do not use",      "#8E24AA" },
			{ "Needs datasheet", "#6D4C41" },
		};

		// Per-name, so this is safe to re-run on a database created before a tag was added — the only
		// way an existing database ever gets one. Renamed or deleted tags are the user's, left alone.
		const std::vector<Tag> present = listTags(db);
		bool ok = true;
		int sortOrder = 0;
		for (const SeedTag& seed : seedTags)
		{
			const int order = sortOrder++;
			bool exists = false;
			for (const Tag& tag : present)
			{
				exists = exists || tag.name == seed.name;
			}
			if (exists)
			{
				continue;
			}
			Tag tag;
			tag.name = seed.name;
			tag.color = seed.color;
			tag.sortOrder = order;
			ok = (insertTag(db, tag) != NoTagId) && ok;
		}
		return ok;
	}

#endif

}
