#pragma once

#include "UnitTest.h"
#include "domain/PartManager_PartFileRole.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_PartRepository : public UnitTest::Test
{
	TEST_CLASS(TST_PartRepository)
public:
	TST_PartRepository()
		: Test("TST_PartRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_PartRepository::crudRoundTripAndSearchableAttrColumn);
		ADD_TEST(TST_PartRepository::smallAndLargeAttrValuesKeepPrecision);
		ADD_TEST(TST_PartRepository::deleteTakesTheChildRowsWithIt);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	static double attrResistanceColumn(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT attr_resistance FROM part WHERE id=" + std::to_string(partId) + ";");
		if (rows.empty() || rows.front().empty() || rows.front().front().empty())
		{
			return 0.0;
		}
		return std::atof(rows.front().front().c_str());
	}

	static int rowCount(SQLiteWrapper::SQLite& db, const std::string& table, int partId)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT COUNT(*) FROM " + table + " WHERE part_id=" + std::to_string(partId) + ";");
		if (rows.empty() || rows.front().empty())
		{
			return -1;
		}
		return std::atoi(rows.front().front().c_str());
	}

	// Tests

	// Foreign keys are declared but never enforced (no PRAGMA foreign_keys=ON anywhere), so a
	// plain DELETE FROM part leaves the tag links, the file rows and the whole stock history
	// behind — and the next part to be handed that id inherits them.
	TEST_FUNCTION(deleteTakesTheChildRowsWithIt)
	{
		TEST_START;

		std::filesystem::path path = std::filesystem::temp_directory_path() / "PartManager_TST_PartRepository_delete.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::TagRepository::createSchema(db);
		PartManager::StockRepository::createSchema(db);

		PartManager::PartType diode;
		diode.name = "Diode";
		diode.domain = "electronic";
		const int typeId = PartManager::PartTypeRepository::insertType(db, diode);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = "1N914BWS";
		const int partId = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT_M(partId != 0, "insertPart failed");

		PartManager::Tag tag;
		tag.name = "SMD";
		const int tagId = PartManager::TagRepository::insertTag(db, tag);
		TEST_ASSERT_M(tagId != 0, "insertTag failed");
		TEST_ASSERT_M(PartManager::TagRepository::addPartTag(db, partId, tagId), "addPartTag failed");

		PartManager::PartFile file;
		file.partId = partId;
		file.role = PartManager::toString(PartManager::PartFileRole::Datasheet);
		file.relativePath = "ab/cd/1n914bws.pdf";
		file.originalFilename = "1n914bws.pdf";
		TEST_ASSERT_M(PartManager::PartRepository::insertFile(db, file) != 0, "insertFile failed");

		TEST_ASSERT_M(PartManager::StockRepository::restock(db, partId, 500, "initial") != 0,
			"restock failed");

		TEST_COMPARE(rowCount(db, "part_tag", partId), 1);
		TEST_COMPARE(rowCount(db, "part_file", partId), 1);
		TEST_COMPARE(rowCount(db, "stock_transaction", partId), 1);

		TEST_ASSERT_M(PartManager::PartRepository::deletePart(db, partId), "deletePart failed");

		PartManager::Part gone;
		TEST_ASSERT_M(!PartManager::PartRepository::findPart(db, partId, gone), "the part must be gone");
		TEST_COMPARE(rowCount(db, "part_tag", partId), 0);
		TEST_COMPARE(rowCount(db, "part_file", partId), 0);
		TEST_COMPARE(rowCount(db, "stock_transaction", partId), 0);
	}

	TEST_FUNCTION(crudRoundTripAndSearchableAttrColumn)
	{
		TEST_START;

		std::filesystem::path path = std::filesystem::temp_directory_path() / "PartManager_TST_PartRepository.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::PartType resistor;
		resistor.name = "Resistor";
		resistor.domain = "electronic";
		int resistorTypeId = PartManager::PartTypeRepository::insertType(db, resistor);

		PartManager::PartTypeAttribute resistance;
		resistance.partTypeId = resistorTypeId;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.unit = "\xCE\xA9";
		resistance.datatype = PartManager::AttributeDataType::Dimension;
		resistance.searchable = true;
		PartManager::PartTypeRepository::insertAttribute(db, resistance);

		PartManager::Part part;
		part.partTypeId = resistorTypeId;
		part.name = "10k 0603 resistor";
		part.manufacturer = "Yageo";
		part.attributes = "{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"}}";

		int partId = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT(partId != 0);
		TEST_COMPARE(attrResistanceColumn(db, partId), 4700.0);

		PartManager::Part readBack;
		TEST_ASSERT_M(PartManager::PartRepository::findPart(db, partId, readBack), "findPart failed");
		TEST_COMPARE(readBack.name, part.name);
		TEST_COMPARE(readBack.manufacturer, part.manufacturer);
		TEST_COMPARE(readBack.attributes, part.attributes);

		// Update: change the attribute value, attr_resistance must follow.
		readBack.attributes = "{\"resistance\":{\"value\":5000,\"unit\":\"\xCE\xA9\"}}";
		TEST_ASSERT_M(PartManager::PartRepository::updatePart(db, readBack), "updatePart failed");
		TEST_COMPARE(attrResistanceColumn(db, partId), 5000.0);

		std::vector<PartManager::Part> all = PartManager::PartRepository::listParts(db, resistorTypeId);
		TEST_COMPARE(all.size(), static_cast<size_t>(1));

		TEST_ASSERT_M(PartManager::PartRepository::deletePart(db, partId), "deletePart failed");
		TEST_ASSERT_M(!PartManager::PartRepository::findPart(db, partId, readBack), "part must be gone after delete");
	}

	// A 100 nF capacitance is a perfectly ordinary part, and it is small enough that a
	// fixed-6-decimal number format writes it into attr_capacitance as a flat 0.
	TEST_FUNCTION(smallAndLargeAttrValuesKeepPrecision)
	{
		TEST_START;

		std::filesystem::path path = std::filesystem::temp_directory_path() / "PartManager_TST_PartRepository_precision.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		int typeId = PartManager::PartTypeRepository::insertType(db, capacitor);

		PartManager::PartTypeAttribute capacitance;
		capacitance.partTypeId = typeId;
		capacitance.key = "capacitance";
		capacitance.label = "Capacitance";
		capacitance.unit = "F";
		capacitance.datatype = PartManager::AttributeDataType::Dimension;
		capacitance.searchable = true;
		PartManager::PartTypeRepository::insertAttribute(db, capacitance);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = "100n 0603 ceramic";
		part.attributes = "{\"capacitance\":{\"value\":1e-07,\"unit\":\"F\"}}";

		int partId = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT(partId != 0);

		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT attr_capacitance FROM part WHERE id=" + std::to_string(partId) + ";");
		TEST_ASSERT(!rows.empty() && !rows.front().empty());
		double stored = std::atof(rows.front().front().c_str());
		TEST_ASSERT_M(stored > 9.9e-8 && stored < 1.01e-7, "100 nF must not be rounded away");
	}
#endif

};

TEST_INSTANTIATE(TST_PartRepository);
