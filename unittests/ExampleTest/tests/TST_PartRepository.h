#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
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

	// Tests
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
