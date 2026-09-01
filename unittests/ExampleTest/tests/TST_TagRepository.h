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
#endif
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
#endif

};

TEST_INSTANTIATE(TST_TagRepository);
