#pragma once

#include "UnitTest.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_PartTypeRepository : public UnitTest::Test
{
	TEST_CLASS(TST_PartTypeRepository)
public:
	TST_PartTypeRepository()
		: Test("TST_PartTypeRepository")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_PartTypeRepository::inheritanceOverridesAndOrders);
		ADD_TEST(TST_PartTypeRepository::kicadCategoryAndDomainFallBackToParent);
		ADD_TEST(TST_PartTypeRepository::seedDefaultTypesIsPopulatedAndIdempotent);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Opens a fresh temp SQLite db with the part_type/part_type_attribute/part_type_file_slot schema.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		return db;
	}

	// Tests
	TEST_FUNCTION(inheritanceOverridesAndOrders)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeRepository_inherit.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		capacitor.kicadCategory = "Capacitors";
		int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);
		TEST_ASSERT(capacitorId != PartManager::NoParentType);

		auto addAttr = [&](int typeId, const std::string& key, const std::string& label, int sortOrder)
		{
			PartManager::PartTypeAttribute attribute;
			attribute.partTypeId = typeId;
			attribute.key = key;
			attribute.label = label;
			attribute.unit = "F";
			attribute.datatype = PartManager::AttributeDataType::Dimension;
			attribute.sortOrder = sortOrder;
			return PartManager::PartTypeRepository::insertAttribute(*db, attribute);
		};

		addAttr(capacitorId, "capacitance", "Capacitance", 0);
		addAttr(capacitorId, "voltage", "Voltage", 1);
		addAttr(capacitorId, "tolerance", "Tolerance", 2);

		PartManager::PartType ceramic;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = capacitorId;
		int ceramicId = PartManager::PartTypeRepository::insertType(*db, ceramic);

		// Override "tolerance" (same key, new label) and add a new "dielectric" attribute.
		addAttr(ceramicId, "tolerance", "Tolerance (Ceramic)", 0);
		addAttr(ceramicId, "dielectric", "Dielectric", 1);

		std::vector<PartManager::PartTypeAttribute> effective =
			PartManager::PartTypeRepository::effectiveAttributes(*db, ceramicId);

		TEST_COMPARE(effective.size(), static_cast<size_t>(4));
		TEST_COMPARE(effective[0].key, std::string("capacitance"));
		TEST_COMPARE(effective[1].key, std::string("voltage"));
		TEST_COMPARE(effective[2].key, std::string("tolerance"));
		TEST_ASSERT_M(effective[2].label == "Tolerance (Ceramic)", "child override must replace the ancestor's label");
		TEST_COMPARE(effective[3].key, std::string("dielectric"));
	}

	TEST_FUNCTION(kicadCategoryAndDomainFallBackToParent)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeRepository_fallback.db");

		PartManager::PartType parent;
		parent.name = "Capacitor";
		parent.domain = "electronic";
		parent.kicadCategory = "Capacitors";
		int parentId = PartManager::PartTypeRepository::insertType(*db, parent);

		PartManager::PartType child;
		child.name = "Ceramic Capacitor";
		child.parentTypeId = parentId;
		// domain/kicadCategory left empty on purpose -> should fall back to parent's.
		int childId = PartManager::PartTypeRepository::insertType(*db, child);

		TEST_COMPARE(PartManager::PartTypeRepository::effectiveDomain(*db, childId), std::string("electronic"));
		TEST_COMPARE(PartManager::PartTypeRepository::effectiveKicadCategory(*db, childId), std::string("Capacitors"));

		// Child overrides kicad_category -> effective value must be the child's own, not the parent's.
		child.id = childId;
		child.kicadCategory = "SpecialCaps";
		PartManager::PartTypeRepository::updateType(*db, child);
		TEST_COMPARE(PartManager::PartTypeRepository::effectiveKicadCategory(*db, childId), std::string("SpecialCaps"));
	}

	TEST_FUNCTION(seedDefaultTypesIsPopulatedAndIdempotent)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeRepository_seed.db");

		TEST_ASSERT_M(PartManager::PartTypeRepository::seedDefaultTypes(*db), "seedDefaultTypes failed");
		std::vector<PartManager::PartType> types = PartManager::PartTypeRepository::listTypes(*db);
		// Resistor, Capacitor, Ceramic Capacitor, Inductor, Power Regulator, Transistor, MOSFET,
		// Diode, LED, Connector, Crystal / Oscillator, Microcontroller, Op-Amp, Logic IC, Switch,
		// Relay, Fuse, Sensor
		TEST_COMPARE(types.size(), static_cast<size_t>(18));

		int mosfetId = 0;
		for (const PartManager::PartType& type : types)
		{
			if (type.name == "MOSFET") mosfetId = type.id;
		}
		TEST_ASSERT_M(mosfetId != 0, "MOSFET must be seeded");
		std::vector<PartManager::PartTypeAttribute> mosfetAttrs =
			PartManager::PartTypeRepository::effectiveAttributes(*db, mosfetId);
		TEST_COMPARE(mosfetAttrs.size(), static_cast<size_t>(3)); // vds_max, id_max, channel_type (Transistor adds none of its own)

		// Idempotent: calling again on an already-seeded db must not duplicate rows.
		TEST_ASSERT_M(PartManager::PartTypeRepository::seedDefaultTypes(*db), "second seedDefaultTypes call failed");
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(18));
	}
#endif

};

TEST_INSTANTIATE(TST_PartTypeRepository);
