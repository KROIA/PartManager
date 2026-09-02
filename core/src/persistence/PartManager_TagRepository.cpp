#include "persistence/PartManager_TagRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	namespace
	{
		// One `#RRGGBB` channel, or -1 when the pair is not two hex digits.
		int hexByte(const std::string& text, size_t at)
		{
			int value = 0;
			for (size_t i = at; i < at + 2; ++i)
			{
				const char c = text[i];
				if (c >= '0' && c <= '9')      value = value * 16 + (c - '0');
				else if (c >= 'a' && c <= 'f') value = value * 16 + (c - 'a' + 10);
				else if (c >= 'A' && c <= 'F') value = value * 16 + (c - 'A' + 10);
				else return -1;
			}
			return value;
		}
	}

	std::string TagRepository::shadeOf(const std::string& baseHex, int index, int count)
	{
		if (baseHex.size() != 7 || baseHex[0] != '#')
		{
			return baseHex;   // not a colour this understands — hand it back rather than blank it
		}
		int channel[3] = { 0, 0, 0 };
		for (int i = 0; i < 3; ++i)
		{
			channel[i] = hexByte(baseHex, 1 + static_cast<size_t>(i) * 2);
			if (channel[i] < 0)
			{
				return baseHex;
			}
		}

		const int steps = count > 1 ? count - 1 : 1;
		const int step = std::max(0, std::min(index, steps));
		// Stops at 62% of the way to white: past roughly there a ten-member family's last chips
		// stop reading as the family's colour and start reading as "pale".
		const double t = 0.62 * static_cast<double>(step) / static_cast<double>(steps);
		char buffer[8] = { 0 };
		std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
			static_cast<int>(channel[0] + (255 - channel[0]) * t + 0.5),
			static_cast<int>(channel[1] + (255 - channel[1]) * t + 0.5),
			static_cast<int>(channel[2] + (255 - channel[2]) * t + 0.5));
		return std::string(buffer);
	}

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
			tag.categoryId = std::atoi(row[4].c_str());
			return tag;
		}

		// Every tag SELECT reads the same five columns in the same order, which is what rowToTag
		// assumes. One spelling of that list so the two cannot drift apart.
		const char* const TagColumns = "t.id,t.name,t.color,t.sort_order,t.category_id";

		TagCategory rowToCategory(const std::vector<std::string>& row)
		{
			TagCategory category;
			category.id = std::atoi(row[0].c_str());
			category.name = row[1];
			category.color = row[2];
			category.sortOrder = std::atoi(row[3].c_str());
			return category;
		}

		bool columnExists(SQLiteWrapper::SQLite& db, const std::string& table, const std::string& column)
		{
			for (const std::vector<std::string>& row : db.fetchAll("PRAGMA table_info(" + table + ");"))
			{
				if (row.size() > 1 && row[1] == column)
				{
					return true;
				}
			}
			return false;
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
				std::string("SELECT ") + TagColumns + " FROM tag t "
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
			"CREATE TABLE IF NOT EXISTS tag_category ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL UNIQUE,"
			"color TEXT NOT NULL,"
			"sort_order INTEGER NOT NULL DEFAULT 0"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS tag ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL UNIQUE,"
			"color TEXT NOT NULL,"
			"sort_order INTEGER NOT NULL DEFAULT 0,"
			"category_id INTEGER NOT NULL DEFAULT 0"
			");") && ok;
		// A `tag` table from before categories existed keeps its rows and gains the column, which
		// defaults every existing tag to uncategorised — the state the schema calls valid.
		if (!columnExists(db, "tag", "category_id"))
		{
			ok = db.execute("ALTER TABLE tag ADD COLUMN category_id INTEGER NOT NULL DEFAULT 0;") && ok;
		}
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

	int TagRepository::insertCategory(SQLiteWrapper::SQLite& db, const TagCategory& category)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO tag_category (name, color, sort_order) VALUES (?, ?, ?);",
			{ category.name, category.color, std::to_string(category.sortOrder) });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoTagCategoryId;
	}

	bool TagRepository::updateCategory(SQLiteWrapper::SQLite& db, const TagCategory& category)
	{
		return db.executeWithParams(
			"UPDATE tag_category SET name=?, color=?, sort_order=? WHERE id=?;",
			{ category.name, category.color, std::to_string(category.sortOrder),
			  std::to_string(category.id) });
	}

	bool TagRepository::deleteCategory(SQLiteWrapper::SQLite& db, int categoryId)
	{
		const std::string id = std::to_string(categoryId);
		// The tags survive as uncategorised. Deleting a heading is a tidying-up action; taking a
		// dozen tags off every part that carries them is not what anyone means by it.
		bool ok = db.executeWithParams("UPDATE tag SET category_id=0 WHERE category_id=?;", { id });
		ok = db.executeWithParams("DELETE FROM tag_category WHERE id=?;", { id }) && ok;
		return ok;
	}

	bool TagRepository::findCategory(SQLiteWrapper::SQLite& db, int categoryId, TagCategory& outCategory)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,name,color,sort_order FROM tag_category WHERE id="
			+ std::to_string(categoryId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outCategory = rowToCategory(rows.front());
		return true;
	}

	std::vector<TagCategory> TagRepository::listCategories(SQLiteWrapper::SQLite& db)
	{
		std::vector<TagCategory> result;
		if (!db.tableExists("tag_category"))
		{
			return result;   // a database migrated only as far as v2 has tags but no families
		}
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,name,color,sort_order FROM tag_category ORDER BY sort_order,name;"))
		{
			result.push_back(rowToCategory(row));
		}
		return result;
	}

	std::vector<Tag> TagRepository::listTagsInCategory(SQLiteWrapper::SQLite& db, int categoryId)
	{
		std::vector<Tag> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			std::string("SELECT ") + TagColumns + " FROM tag t WHERE t.category_id="
			+ std::to_string(categoryId) + " ORDER BY t.sort_order,t.name;"))
		{
			result.push_back(rowToTag(row));
		}
		return result;
	}

	bool TagRepository::setTagCategory(SQLiteWrapper::SQLite& db, int tagId, int categoryId, bool recolour)
	{
		std::string color;
		if (recolour && categoryId != NoTagCategoryId)
		{
			TagCategory category;
			if (findCategory(db, categoryId, category))
			{
				// Appended to the family, so it takes the shade after the current last member.
				const int position = static_cast<int>(listTagsInCategory(db, categoryId).size());
				color = shadeOf(category.color, position, position + 1);
			}
		}
		if (color.empty())
		{
			return db.executeWithParams("UPDATE tag SET category_id=? WHERE id=?;",
				{ std::to_string(categoryId), std::to_string(tagId) });
		}
		return db.executeWithParams("UPDATE tag SET category_id=?, color=? WHERE id=?;",
			{ std::to_string(categoryId), color, std::to_string(tagId) });
	}

	int TagRepository::insertTag(SQLiteWrapper::SQLite& db, const Tag& tag)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO tag (name, color, sort_order, category_id) VALUES (?, ?, ?, ?);",
			{ tag.name, tag.color, std::to_string(tag.sortOrder), std::to_string(tag.categoryId) });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoTagId;
	}

	bool TagRepository::updateTag(SQLiteWrapper::SQLite& db, const Tag& tag)
	{
		return db.executeWithParams(
			"UPDATE tag SET name=?, color=?, sort_order=?, category_id=? WHERE id=?;",
			{ tag.name, tag.color, std::to_string(tag.sortOrder), std::to_string(tag.categoryId),
			  std::to_string(tag.id) });
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
			std::string("SELECT ") + TagColumns + " FROM tag t WHERE t.id=" + std::to_string(tagId) + ";");
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
			std::string("SELECT ") + TagColumns + " FROM tag t ORDER BY t.sort_order,t.name;"))
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
		// A starting vocabulary, not a taxonomy. Tags are cross-cutting (§2d), so these deliberately
		// say nothing about what a part *is* (the type template's job) nor how it is packaged (the
		// part's own `package` field) — only the things neither of those can express.
		struct SeedTag
		{
			const char* name;
			// The colour this function gave the tag before families existed, or nullptr for one
			// that never had a different one. Only a tag still wearing exactly this is recoloured
			// into its family's ramp; anything else is a colour the user chose.
			const char* legacyColor;
		};
		struct SeedFamily
		{
			const char* name;
			const char* color;                 // base shade; members are lightened steps of it
			std::vector<SeedTag> tags;
		};
		static const std::vector<SeedFamily> families = {
			{ "Bus protocols", "#3949AB", {
				{ "I2C", nullptr }, { "SPI", nullptr }, { "UART", nullptr }, { "CAN", nullptr },
				{ "USB", nullptr }, { "1-Wire", nullptr }, { "RS-485", nullptr },
				{ "Ethernet", nullptr }, { "I2S", nullptr }, { "JTAG/SWD", nullptr } } },
			{ "PCB placement", "#00897B", {
				{ "SMD", "#1E88E5" }, { "THT", "#43A047" },
				{ "Panel mount", nullptr }, { "Screw terminal", nullptr } } },
			{ "Lifecycle", "#C62828", {
				{ "Active", nullptr }, { "NRND", nullptr }, { "Obsolete", "#E53935" },
				{ "Do not use", "#8E24AA" }, { "Needs datasheet", "#6D4C41" } } },
			{ "Voltage domain", "#EF6C00", {
				{ "1V8", nullptr }, { "3V3", nullptr }, { "5V", nullptr },
				{ "12V", nullptr }, { "24V", nullptr }, { "48V", nullptr } } },
			{ "Handling", "#6A1B9A", {
				{ "ESD sensitive", nullptr }, { "Moisture sensitive", nullptr },
				{ "Fine pitch", nullptr }, { "Hand-solderable", nullptr },
				{ "Reflow only", nullptr } } },
		};
		// Tags that belong to no family: nothing else answers the same question they do.
		static const SeedTag looseTags[] = { { "Favourite", "#FDD835" } };

		// Per-name throughout, so this is safe to re-run — that is the only way an existing database
		// ever gets a family added later. Renamed or deleted entries are the user's and stay so.
		const std::vector<Tag> present = listTags(db);
		const std::vector<TagCategory> existingCategories = listCategories(db);
		bool ok = true;
		int familyOrder = 0;

		for (const SeedFamily& family : families)
		{
			const int order = familyOrder++;
			int categoryId = NoTagCategoryId;
			for (const TagCategory& category : existingCategories)
			{
				if (category.name == family.name)
				{
					categoryId = category.id;
				}
			}
			if (categoryId == NoTagCategoryId)
			{
				TagCategory category;
				category.name = family.name;
				category.color = family.color;
				category.sortOrder = order;
				categoryId = insertCategory(db, category);
				ok = (categoryId != NoTagCategoryId) && ok;
			}
			if (categoryId == NoTagCategoryId)
			{
				continue;   // could not create the family, so there is nowhere to put its tags
			}

			const int memberCount = static_cast<int>(family.tags.size());
			for (int i = 0; i < memberCount; ++i)
			{
				const SeedTag& seed = family.tags[static_cast<size_t>(i)];
				const std::string shade = shadeOf(family.color, i, memberCount);
				const Tag* existing = nullptr;
				for (const Tag& tag : present)
				{
					if (tag.name == seed.name)
					{
						existing = &tag;
					}
				}
				if (existing == nullptr)
				{
					Tag tag;
					tag.name = seed.name;
					tag.color = shade;
					tag.sortOrder = i;
					tag.categoryId = categoryId;
					ok = (insertTag(db, tag) != NoTagId) && ok;
					continue;
				}
				if (existing->categoryId != NoTagCategoryId)
				{
					continue;   // already filed, by this function on an earlier run or by the user
				}
				// Adopt a tag seeded before families existed. Recolour it into the ramp only when
				// it still carries the colour this function gave it — otherwise that colour was a
				// deliberate choice and the family can live with one odd member.
				Tag adopted = *existing;
				adopted.categoryId = categoryId;
				adopted.sortOrder = i;
				if (seed.legacyColor != nullptr && adopted.color == seed.legacyColor)
				{
					adopted.color = shade;
				}
				ok = updateTag(db, adopted) && ok;
			}
		}

		for (const SeedTag& seed : looseTags)
		{
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
			tag.color = seed.legacyColor;
			tag.sortOrder = 0;
			ok = (insertTag(db, tag) != NoTagId) && ok;
		}
		return ok;
	}

#endif

}
