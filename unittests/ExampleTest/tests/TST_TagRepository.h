#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_TagRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_TagRepository : public UnitTest::Test
{
	TEST_CLASS(TST_TagRepository)
public:
	TST_TagRepository()
		: Test("TST_TagRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_TagRepository::tagCrudRoundTrip);
		ADD_TEST(TST_TagRepository::deleteTagRemovesLinkRows);
		ADD_TEST(TST_TagRepository::effectiveTypeDefaultTagsUnionsChain);
		ADD_TEST(TST_TagRepository::seedTagsForNewPartCopiesDefaults);
		ADD_TEST(TST_TagRepository::seedIsOneTimeNotALiveLink);
		ADD_TEST(TST_TagRepository::seedDefaultTagsIsPopulatedAndIdempotent);
		ADD_TEST(TST_TagRepository::categoriesGroupTagsWithoutOwningThem);
		ADD_TEST(TST_TagRepository::deletingACategoryKeepsItsTags);
		ADD_TEST(TST_TagRepository::seedAdoptsTagsWrittenBeforeCategories);
#endif
		ADD_TEST(TST_TagRepository::oneFamilyIsOneColourInSteps);
		ADD_TEST(TST_TagRepository::aGradientRunsBetweenTheTwoEndsItIsGiven);
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Opens a fresh temp SQLite db with the part_type/part/tag schemas.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		PartManager::PartRepository::createSchema(*db);
		PartManager::TagRepository::createSchema(*db);
		return db;
	}

	static int addTag(SQLiteWrapper::SQLite& db, const std::string& name, int sortOrder)
	{
		PartManager::Tag tag;
		tag.name = name;
		tag.color = "#E53935";
		tag.sortOrder = sortOrder;
		return PartManager::TagRepository::insertTag(db, tag);
	}

	// Tests
	TEST_FUNCTION(tagCrudRoundTrip)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_crud.db");

		int ledId = addTag(*db, "LED", 1);
		TEST_ASSERT(ledId != PartManager::NoTagId);
		int thtId = addTag(*db, "THT", 0);
		TEST_ASSERT(thtId != PartManager::NoTagId);

		PartManager::Tag found;
		TEST_ASSERT_M(PartManager::TagRepository::findTag(*db, ledId, found), "findTag must find an inserted tag");
		TEST_COMPARE(found.name, std::string("LED"));
		TEST_COMPARE(found.color, std::string("#E53935"));
		TEST_COMPARE(found.sortOrder, 1);

		found.name = "LED (5mm)";
		found.color = "#2E7D32";
		TEST_ASSERT(PartManager::TagRepository::updateTag(*db, found));
		PartManager::Tag reread;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, ledId, reread));
		TEST_COMPARE(reread.name, std::string("LED (5mm)"));
		TEST_COMPARE(reread.color, std::string("#2E7D32"));

		// listTags orders by sort_order then name -> THT (0) before LED (1).
		std::vector<PartManager::Tag> all = PartManager::TagRepository::listTags(*db);
		TEST_COMPARE(all.size(), static_cast<size_t>(2));
		TEST_COMPARE(all[0].name, std::string("THT"));
		TEST_COMPARE(all[1].name, std::string("LED (5mm)"));

		TEST_ASSERT(PartManager::TagRepository::deleteTag(*db, ledId));
		TEST_ASSERT_M(!PartManager::TagRepository::findTag(*db, ledId, reread), "deleted tag must not be findable");
		TEST_COMPARE(PartManager::TagRepository::listTags(*db).size(), static_cast<size_t>(1));
	}

	TEST_FUNCTION(deleteTagRemovesLinkRows)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_delete.db");

		PartManager::PartType diode;
		diode.name = "Diode";
		diode.domain = "electronic";
		int diodeId = PartManager::PartTypeRepository::insertType(*db, diode);

		PartManager::Part part;
		part.partTypeId = diodeId;
		part.name = "1N4148";
		int partId = PartManager::PartRepository::insertPart(*db, part);
		TEST_ASSERT(partId != 0);

		int smdId = addTag(*db, "SMD", 0);
		TEST_ASSERT(PartManager::TagRepository::addTypeDefaultTag(*db, diodeId, smdId));
		TEST_ASSERT(PartManager::TagRepository::addPartTag(*db, partId, smdId));
		TEST_COMPARE(PartManager::TagRepository::listTypeDefaultTags(*db, diodeId).size(), static_cast<size_t>(1));
		TEST_COMPARE(PartManager::TagRepository::listPartTags(*db, partId).size(), static_cast<size_t>(1));

		TEST_ASSERT(PartManager::TagRepository::deleteTag(*db, smdId));

		// SQLite doesn't enforce the REFERENCES clauses here, so deleteTag has to clean up itself.
		TEST_ASSERT_M(PartManager::TagRepository::listTypeDefaultTags(*db, diodeId).empty(),
			"deleteTag must remove the tag's part_type_tag rows");
		TEST_ASSERT_M(PartManager::TagRepository::listPartTags(*db, partId).empty(),
			"deleteTag must remove the tag's part_tag rows");
		TEST_ASSERT_M(db->fetchAll("SELECT tag_id FROM part_type_tag;").empty(), "part_type_tag must be empty");
		TEST_ASSERT_M(db->fetchAll("SELECT tag_id FROM part_tag;").empty(), "part_tag must be empty");
	}

	TEST_FUNCTION(effectiveTypeDefaultTagsUnionsChain)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_effective.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);

		PartManager::PartType ceramic;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = capacitorId;
		int ceramicId = PartManager::PartTypeRepository::insertType(*db, ceramic);

		int passiveId = addTag(*db, "Passive", 0);
		int smdId = addTag(*db, "SMD", 1);
		int ceramicTagId = addTag(*db, "Ceramic", 2);

		PartManager::TagRepository::addTypeDefaultTag(*db, capacitorId, passiveId);
		PartManager::TagRepository::addTypeDefaultTag(*db, capacitorId, smdId);
		PartManager::TagRepository::addTypeDefaultTag(*db, ceramicId, ceramicTagId);
		// Re-declaring an inherited tag on the child must not produce a duplicate.
		PartManager::TagRepository::addTypeDefaultTag(*db, ceramicId, passiveId);

		// Own rows only: the child declares Ceramic + Passive, never the parent's SMD.
		TEST_COMPARE(PartManager::TagRepository::listTypeDefaultTags(*db, ceramicId).size(), static_cast<size_t>(2));

		std::vector<PartManager::Tag> effective =
			PartManager::TagRepository::effectiveTypeDefaultTags(*db, ceramicId);
		TEST_COMPARE(effective.size(), static_cast<size_t>(3));
		// Ancestor tags first (by their own sort_order), then the child's new ones.
		TEST_COMPARE(effective[0].name, std::string("Passive"));
		TEST_COMPARE(effective[1].name, std::string("SMD"));
		TEST_COMPARE(effective[2].name, std::string("Ceramic"));

		// The parent itself is unaffected by what the child declares.
		TEST_COMPARE(PartManager::TagRepository::effectiveTypeDefaultTags(*db, capacitorId).size(),
			static_cast<size_t>(2));
	}

	TEST_FUNCTION(seedTagsForNewPartCopiesDefaults)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_seed.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);

		PartManager::PartType ceramic;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = capacitorId;
		int ceramicId = PartManager::PartTypeRepository::insertType(*db, ceramic);

		int passiveId = addTag(*db, "Passive", 0);
		int ceramicTagId = addTag(*db, "Ceramic", 1);
		PartManager::TagRepository::addTypeDefaultTag(*db, capacitorId, passiveId);
		PartManager::TagRepository::addTypeDefaultTag(*db, ceramicId, ceramicTagId);

		// insertPart seeds on its own, so no caller can forget it (§2d).
		PartManager::Part part;
		part.partTypeId = ceramicId;
		part.name = "100nF X7R";
		int partId = PartManager::PartRepository::insertPart(*db, part);
		TEST_ASSERT(partId != 0);

		std::vector<PartManager::Tag> partTags = PartManager::TagRepository::listPartTags(*db, partId);
		TEST_COMPARE(partTags.size(), static_cast<size_t>(2));
		TEST_COMPARE(partTags[0].name, std::string("Passive"));
		TEST_COMPARE(partTags[1].name, std::string("Ceramic"));

		// One-time seed: running it again must not re-add a tag the user removed in the meantime.
		TEST_ASSERT(PartManager::TagRepository::removePartTag(*db, partId, passiveId));
		TEST_ASSERT(PartManager::TagRepository::seedTagsForNewPart(*db, partId, ceramicId));
		TEST_COMPARE(PartManager::TagRepository::listPartTags(*db, partId).size(), static_cast<size_t>(1));

		// setPartTags replaces the whole set.
		TEST_ASSERT(PartManager::TagRepository::setPartTags(*db, partId, { passiveId, ceramicTagId }));
		TEST_COMPARE(PartManager::TagRepository::listPartTags(*db, partId).size(), static_cast<size_t>(2));
	}

	TEST_FUNCTION(seedIsOneTimeNotALiveLink)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_livelink.db");

		PartManager::PartType resistor;
		resistor.name = "Resistor";
		resistor.domain = "electronic";
		int resistorId = PartManager::PartTypeRepository::insertType(*db, resistor);

		int passiveId = addTag(*db, "Passive", 0);
		int thtId = addTag(*db, "THT", 1);
		PartManager::TagRepository::addTypeDefaultTag(*db, resistorId, passiveId);
		PartManager::TagRepository::addTypeDefaultTag(*db, resistorId, thtId);

		PartManager::Part part;
		part.partTypeId = resistorId;
		part.name = "10k 0805";
		int partId = PartManager::PartRepository::insertPart(*db, part);
		TEST_COMPARE(PartManager::TagRepository::listPartTags(*db, partId).size(), static_cast<size_t>(2));

		// Editing the TYPE's defaults afterwards must not touch a part that already exists (§2d).
		int smdId = addTag(*db, "SMD", 2);
		PartManager::TagRepository::addTypeDefaultTag(*db, resistorId, smdId);
		PartManager::TagRepository::removeTypeDefaultTag(*db, resistorId, thtId);

		std::vector<PartManager::Tag> partTags = PartManager::TagRepository::listPartTags(*db, partId);
		TEST_COMPARE(partTags.size(), static_cast<size_t>(2));
		TEST_COMPARE(partTags[0].name, std::string("Passive"));
		TEST_COMPARE(partTags[1].name, std::string("THT"));

		// Type defaults did change, for the NEXT part created under it.
		int nextPartId = PartManager::PartRepository::insertPart(*db, part);
		std::vector<PartManager::Tag> nextTags = PartManager::TagRepository::listPartTags(*db, nextPartId);
		TEST_COMPARE(nextTags.size(), static_cast<size_t>(2));
		TEST_COMPARE(nextTags[0].name, std::string("Passive"));
		TEST_COMPARE(nextTags[1].name, std::string("SMD"));

		// Removing an inherited tag from the part works and leaves the type's defaults alone.
		TEST_ASSERT(PartManager::TagRepository::removePartTag(*db, partId, passiveId));
		TEST_COMPARE(PartManager::TagRepository::listPartTags(*db, partId).size(), static_cast<size_t>(1));
		TEST_COMPARE(PartManager::TagRepository::effectiveTypeDefaultTags(*db, resistorId).size(),
			static_cast<size_t>(2));
	}

	TEST_FUNCTION(seedDefaultTagsIsPopulatedAndIdempotent)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_defaults.db");

		TEST_ASSERT_M(PartManager::TagRepository::seedDefaultTags(*db), "seedDefaultTags failed");
		std::vector<PartManager::Tag> tags = PartManager::TagRepository::listTags(*db);
		const std::vector<PartManager::TagCategory> categories =
			PartManager::TagRepository::listCategories(*db);
		// Bus protocols, PCB placement, Lifecycle, Voltage domain, Handling.
		TEST_COMPARE(categories.size(), static_cast<size_t>(5));
		TEST_ASSERT_M(tags.size() > categories.size(), "every family needs members");
		// Every seeded tag is filed except the deliberately loose ones.
		size_t filed = 0;
		for (const PartManager::Tag& tag : tags)
		{
			filed += (tag.categoryId != PartManager::NoTagCategoryId) ? 1 : 0;
		}
		TEST_COMPARE(filed, tags.size() - 1);   // Favourite is the only loose one
		for (const PartManager::Tag& tag : tags)
		{
			TEST_ASSERT_M(!tag.color.empty(), "every seeded tag needs a chip colour: " + tag.name);
		}

		// Idempotent: a second call on a non-empty tag table must not duplicate or reset anything.
		const size_t seeded = tags.size();
		TEST_ASSERT_M(PartManager::TagRepository::seedDefaultTags(*db), "second seedDefaultTags call failed");
		TEST_COMPARE(PartManager::TagRepository::listTags(*db).size(), seeded);
		TEST_COMPARE(PartManager::TagRepository::listCategories(*db).size(), categories.size());
	}

	// The families are a view over tags, not a second owner of them: a part still carries tags,
	// and a tag still exists on its own.
	TEST_FUNCTION(categoriesGroupTagsWithoutOwningThem)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_categories.db");

		PartManager::TagCategory bus;
		bus.name = "Bus protocols";
		bus.color = "#3949AB";
		const int busId = PartManager::TagRepository::insertCategory(*db, bus);
		TEST_ASSERT(busId != PartManager::NoTagCategoryId);
		// The name is UNIQUE, same as a tag's.
		TEST_COMPARE(PartManager::TagRepository::insertCategory(*db, bus), PartManager::NoTagCategoryId);

		const int i2c = addTag(*db, "I2C", 0);
		const int spi = addTag(*db, "SPI", 1);
		const int loose = addTag(*db, "Favourite", 2);
		TEST_ASSERT(PartManager::TagRepository::setTagCategory(*db, i2c, busId));
		TEST_ASSERT(PartManager::TagRepository::setTagCategory(*db, spi, busId));

		TEST_COMPARE(PartManager::TagRepository::listTagsInCategory(*db, busId).size(),
			static_cast<size_t>(2));
		TEST_COMPARE(PartManager::TagRepository::listTagsInCategory(
			*db, PartManager::NoTagCategoryId).size(), static_cast<size_t>(1));

		// The category comes back on the tag itself, so a reader never needs a second query.
		PartManager::Tag readBack;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, i2c, readBack));
		TEST_COMPARE(readBack.categoryId, busId);
		TEST_ASSERT_M(readBack.color != "#E53935",
			"joining a family recolours the tag into its ramp");

		// Moving into a family is what recolours; moving out leaves the colour alone, because
		// there is no family colour to derive a new one from.
		TEST_ASSERT(PartManager::TagRepository::setTagCategory(
			*db, i2c, PartManager::NoTagCategoryId));
		PartManager::Tag afterMove;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, i2c, afterMove));
		TEST_COMPARE(afterMove.categoryId, PartManager::NoTagCategoryId);
		TEST_COMPARE(afterMove.color, readBack.color);

		// A tag that belongs to no family is still a perfectly good tag, and still uncategorised.
		PartManager::Tag looseTag;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, loose, looseTag));
		TEST_COMPARE(looseTag.categoryId, PartManager::NoTagCategoryId);
	}

	// Deleting a heading must not quietly strip a dozen tags off every part carrying them.
	TEST_FUNCTION(deletingACategoryKeepsItsTags)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_categorydelete.db");

		PartManager::TagCategory family;
		family.name = "PCB placement";
		family.color = "#00897B";
		const int familyId = PartManager::TagRepository::insertCategory(*db, family);
		const int smd = addTag(*db, "SMD", 0);
		PartManager::TagRepository::setTagCategory(*db, smd, familyId);

		const int partId = 42;
		TEST_ASSERT(PartManager::TagRepository::addPartTag(*db, partId, smd));
		TEST_ASSERT(PartManager::TagRepository::deleteCategory(*db, familyId));

		TEST_COMPARE(PartManager::TagRepository::listCategories(*db).size(), static_cast<size_t>(0));
		PartManager::Tag survivor;
		TEST_ASSERT_M(PartManager::TagRepository::findTag(*db, smd, survivor),
			"the tag must outlive its category");
		TEST_COMPARE(survivor.categoryId, PartManager::NoTagCategoryId);
		TEST_ASSERT_M(PartManager::TagRepository::listPartTags(*db, partId).size() == 1,
			"and stay on the part that carries it");
	}

	// The migration case: a database seeded before categories existed. Its tags must end up in
	// the right family instead of a second copy of each name appearing beside them.
	TEST_FUNCTION(seedAdoptsTagsWrittenBeforeCategories)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TagRepository_adopt.db");

		// Exactly what the pre-category seed wrote, colours included.
		PartManager::Tag smd;
		smd.name = "SMD";
		smd.color = "#1E88E5";
		const int smdId = PartManager::TagRepository::insertTag(*db, smd);
		// ...and one the user recoloured afterwards, which must keep the colour they chose.
		PartManager::Tag obsolete;
		obsolete.name = "Obsolete";
		obsolete.color = "#000000";
		const int obsoleteId = PartManager::TagRepository::insertTag(*db, obsolete);

		TEST_ASSERT(PartManager::TagRepository::seedDefaultTags(*db));

		PartManager::Tag adopted;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, smdId, adopted));
		TEST_ASSERT_M(adopted.categoryId != PartManager::NoTagCategoryId,
			"an old tag must join its family rather than be duplicated");
		TEST_ASSERT_M(adopted.color != "#1E88E5",
			"a tag still wearing the old seed colour is recoloured into the family ramp");

		PartManager::Tag kept;
		TEST_ASSERT(PartManager::TagRepository::findTag(*db, obsoleteId, kept));
		TEST_ASSERT(kept.categoryId != PartManager::NoTagCategoryId);
		TEST_ASSERT_M(kept.color == "#000000",
			"a colour the user chose is theirs, family or not");

		// One row per name, still - adoption, not duplication.
		int smdCount = 0;
		for (const PartManager::Tag& tag : PartManager::TagRepository::listTags(*db))
		{
			smdCount += (tag.name == "SMD") ? 1 : 0;
		}
		TEST_COMPARE(smdCount, 1);
	}
#endif

	// The shade ramp is what makes "one colour, different shades" true. No database needed.
	TEST_FUNCTION(oneFamilyIsOneColourInSteps)
	{
		TEST_START;

		// The first member is the base itself, so a one-tag family is simply its own colour.
		TEST_COMPARE(PartManager::TagRepository::shadeOf("#3949AB", 0, 4), std::string("#3949AB"));
		TEST_COMPARE(PartManager::TagRepository::shadeOf("#3949AB", 0, 1), std::string("#3949AB"));

		// Every later step is lighter than the one before, and distinct from it - two chips the
		// eye cannot separate are the failure this guards against.
		std::string previous = PartManager::TagRepository::shadeOf("#3949AB", 0, 6);
		for (int i = 1; i < 6; ++i)
		{
			const std::string shade = PartManager::TagRepository::shadeOf("#3949AB", i, 6);
			TEST_COMPARE(shade.size(), static_cast<size_t>(7));
			TEST_ASSERT_M(shade != previous, "each step must be a different colour: " + shade);
			TEST_ASSERT_M(shade > previous, "and a lighter one: " + previous + " -> " + shade);
			previous = shade;
		}

		// It stops short of white: the last member of a big family still reads as the family's
		// colour rather than as an empty chip.
		const std::string last = PartManager::TagRepository::shadeOf("#3949AB", 9, 10);
		TEST_ASSERT_M(last < std::string("#FFFFFF"), "the ramp must not reach white: " + last);

		// An index past the end clamps instead of running off into white.
		TEST_COMPARE(PartManager::TagRepository::shadeOf("#3949AB", 99, 4),
			PartManager::TagRepository::shadeOf("#3949AB", 3, 4));

		// Anything it cannot read comes back untouched - never an empty string, which would
		// paint a chip with no colour at all.
		TEST_COMPARE(PartManager::TagRepository::shadeOf("", 0, 3), std::string(""));
		TEST_COMPARE(PartManager::TagRepository::shadeOf("red", 1, 3), std::string("red"));
		TEST_COMPARE(PartManager::TagRepository::shadeOf("#GGHHII", 1, 3), std::string("#GGHHII"));
	}

	// The gradient the Manage Tags dialog recalculates: the two ends are the user's, the middle
	// is derived. No database needed.
	TEST_FUNCTION(aGradientRunsBetweenTheTwoEndsItIsGiven)
	{
		TEST_START;
		using Repo = PartManager::TagRepository;

		// The ends come back exactly, which is what lets the button be pressed twice without
		// walking its own endpoints inwards.
		TEST_COMPARE(Repo::blendOf("#00FF00", "#FF0000", 0, 5), std::string("#00FF00"));
		TEST_COMPARE(Repo::blendOf("#00FF00", "#FF0000", 4, 5), std::string("#FF0000"));

		// Halfway is halfway on every channel - green through olive to red, the ramp a single
		// base colour cannot express and the reason this exists beside shadeOf(). Note it is a
		// straight RGB line, so the midpoint of green->red is #808000 and not a bright amber;
		// going through a colour space that keeps the chroma up is the upgrade path if the
		// middle of a long family ever looks muddy.
		TEST_COMPARE(Repo::blendOf("#00FF00", "#FF0000", 2, 5), std::string("#808000"));
		TEST_COMPARE(Repo::blendOf("#000000", "#FFFFFF", 1, 3), std::string("#808080"));

		// Every step differs from the one before, so no two chips in a family look identical.
		std::string previous = Repo::blendOf("#1E88E5", "#C62828", 0, 6);
		for (int i = 1; i < 6; ++i)
		{
			const std::string step = Repo::blendOf("#1E88E5", "#C62828", i, 6);
			TEST_COMPARE(step.size(), static_cast<size_t>(7));
			TEST_ASSERT_M(step != previous, "each step must be a different colour: " + step);
			previous = step;
		}

		// Clamped past the end, and a single-member ramp is its own start.
		TEST_COMPARE(Repo::blendOf("#00FF00", "#FF0000", 99, 5), std::string("#FF0000"));
		TEST_COMPARE(Repo::blendOf("#00FF00", "#FF0000", 0, 1), std::string("#00FF00"));

		// An unreadable end hands back a colour rather than blanking a chip.
		TEST_COMPARE(Repo::blendOf("#00FF00", "red", 1, 3), std::string("#00FF00"));
		TEST_COMPARE(Repo::blendOf("", "#FF0000", 1, 3), std::string(""));
		TEST_COMPARE(Repo::blendOf("#GGHHII", "#FF0000", 1, 3), std::string("#GGHHII"));
	}

};

TEST_INSTANTIATE(TST_TagRepository);
