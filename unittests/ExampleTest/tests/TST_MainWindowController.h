#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_MainWindowController.h"
#include <algorithm>

// The Home tab's widget-free logic (§7a tree assembly + stock aggregation, §7b column
// derivation, §2a display formatting). The widgets around it need a live QApplication
// and are not exercised here.
class TST_MainWindowController : public UnitTest::Test
{
	TEST_CLASS(TST_MainWindowController)
public:
	TST_MainWindowController()
		: Test("TST_MainWindowController")
	{
		ADD_TEST(TST_MainWindowController::treeNestsChildrenAndSumsStock);
		ADD_TEST(TST_MainWindowController::treeSurvivesOrphansAndCycles);
		ADD_TEST(TST_MainWindowController::descendantsOfAParentIncludeGrandchildren);
		ADD_TEST(TST_MainWindowController::columnsFollowEffectiveAttributeOrder);
		ADD_TEST(TST_MainWindowController::dimensionValuesFormatWithSiPrefix);
		ADD_TEST(TST_MainWindowController::treeMatchCountsRollUpLikeStock);
		ADD_TEST(TST_MainWindowController::searchErrorOnlyReportsMalformedQueries);
	}

private:

	static PartManager::PartType makeType(int id, const std::string& name, int parentId)
	{
		PartManager::PartType type;
		type.id = id;
		type.name = name;
		type.domain = "electronic";
		type.parentTypeId = parentId;
		return type;
	}

	// Tests
	TEST_FUNCTION(treeNestsChildrenAndSumsStock)
	{
		TEST_START;

		std::vector<PartManager::PartType> types{
			makeType(1, "Capacitor", PartManager::NoParentType),
			makeType(2, "Ceramic Capacitor", 1),
			makeType(3, "Electrolytic Capacitor", 1),
			makeType(4, "Resistor", PartManager::NoParentType)
		};
		std::map<int, int> inStock{ {1, 5}, {2, 20}, {3, 7}, {4, 412} };

		std::vector<PartManager::CategoryNode> roots = PartManager::buildCategoryTree(types, inStock);

		TEST_COMPARE(roots.size(), static_cast<size_t>(2));
		// Roots and children are name-sorted, so Capacitor comes before Resistor.
		TEST_COMPARE(roots[0].name.toStdString(), std::string("Capacitor"));
		TEST_COMPARE(roots[1].name.toStdString(), std::string("Resistor"));
		TEST_COMPARE(roots[0].children.size(), static_cast<size_t>(2));
		TEST_COMPARE(roots[0].children[0].name.toStdString(), std::string("Ceramic Capacitor"));

		// A parent's count is its own plus every descendant's (§7a live counts).
		TEST_COMPARE(roots[0].inStockCount, 32);
		TEST_COMPARE(roots[0].children[0].inStockCount, 20);
		TEST_COMPARE(roots[1].inStockCount, 412);
	}

	TEST_FUNCTION(treeSurvivesOrphansAndCycles)
	{
		TEST_START;

		// 7's parent does not exist; 10 and 11 point at each other.
		std::vector<PartManager::PartType> types{
			makeType(7, "Orphan", 99),
			makeType(10, "LoopA", 11),
			makeType(11, "LoopB", 10)
		};

		std::vector<PartManager::CategoryNode> roots =
			PartManager::buildCategoryTree(types, std::map<int, int>());

		// The orphan becomes a root; the cycle contributes no roots and must not hang.
		TEST_COMPARE(roots.size(), static_cast<size_t>(1));
		TEST_COMPARE(roots[0].name.toStdString(), std::string("Orphan"));
	}

	TEST_FUNCTION(descendantsOfAParentIncludeGrandchildren)
	{
		TEST_START;

		std::vector<PartManager::PartType> types{
			makeType(1, "Capacitor", PartManager::NoParentType),
			makeType(2, "Ceramic Capacitor", 1),
			makeType(3, "X7R", 2),
			makeType(4, "Resistor", PartManager::NoParentType)
		};

		std::vector<int> ids = PartManager::typeIdWithDescendants(types, 1);
		TEST_COMPARE(ids.size(), static_cast<size_t>(3));
		TEST_ASSERT(std::find(ids.begin(), ids.end(), 3) != ids.end());
		TEST_ASSERT(std::find(ids.begin(), ids.end(), 4) == ids.end());

		// A leaf resolves to just itself.
		TEST_COMPARE(PartManager::typeIdWithDescendants(types, 4).size(), static_cast<size_t>(1));
		// "no type selected" resolves to nothing at all, so no rows get listed.
		TEST_COMPARE(PartManager::typeIdWithDescendants(types, PartManager::NoParentType).size(),
			static_cast<size_t>(0));
	}

	TEST_FUNCTION(columnsFollowEffectiveAttributeOrder)
	{
		TEST_START;

		PartManager::PartTypeAttribute resistance;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.unit = "\xCE\xA9";
		resistance.datatype = PartManager::AttributeDataType::Dimension;

		PartManager::PartTypeAttribute tolerance;
		tolerance.key = "tolerance";
		tolerance.label = "Tolerance";
		tolerance.unit = "%";
		tolerance.datatype = PartManager::AttributeDataType::Dimension;

		std::vector<PartManager::PartColumn> columns =
			PartManager::deriveColumns({ resistance, tolerance });

		// name, manufacturer, mpn, package, <attributes in declaration order>, stock_qty
		TEST_COMPARE(columns.size(), static_cast<size_t>(7));
		TEST_COMPARE(columns[0].key.toStdString(), std::string("name"));
		TEST_COMPARE(columns[3].key.toStdString(), std::string("package"));
		TEST_COMPARE(columns[4].key.toStdString(), std::string("resistance"));
		TEST_ASSERT(columns[4].isAttribute);
		TEST_COMPARE(columns[5].key.toStdString(), std::string("tolerance"));
		TEST_COMPARE(columns[6].key.toStdString(), std::string("stock_qty"));
		TEST_ASSERT(!columns[6].isAttribute);

		// No attributes at all still yields the built-ins.
		TEST_COMPARE(PartManager::deriveColumns({}).size(), static_cast<size_t>(5));
	}

	TEST_FUNCTION(dimensionValuesFormatWithSiPrefix)
	{
		TEST_START;

		const std::string ohm = "\xCE\xA9";

		PartManager::PartColumn resistance;
		resistance.key = "resistance";
		resistance.isAttribute = true;
		resistance.unit = QString::fromStdString(ohm);
		resistance.datatype = PartManager::AttributeDataType::Dimension;

		QString json = QString::fromStdString(
			"{\"resistance\":{\"value\":4700,\"unit\":\"" + ohm + "\"},"
			"\"note\":\"thin film\",\"pins\":8,\"rohs\":true}");

		// §2a: a stored 4700 must never reach the table as "4700".
		TEST_COMPARE(PartManager::formatAttributeValue(json, resistance).toStdString(),
			std::string("4.7 k") + ohm);

		PartManager::PartColumn note;
		note.key = "note";
		note.isAttribute = true;
		note.datatype = PartManager::AttributeDataType::Text;
		TEST_COMPARE(PartManager::formatAttributeValue(json, note).toStdString(), std::string("thin film"));

		PartManager::PartColumn pins;
		pins.key = "pins";
		pins.isAttribute = true;
		pins.datatype = PartManager::AttributeDataType::Number;
		TEST_COMPARE(PartManager::formatAttributeValue(json, pins).toStdString(), std::string("8"));

		PartManager::PartColumn rohs;
		rohs.key = "rohs";
		rohs.isAttribute = true;
		rohs.datatype = PartManager::AttributeDataType::Bool;
		TEST_ASSERT(!PartManager::formatAttributeValue(json, rohs).isEmpty());

		// A key the part never filled in, and outright broken JSON, both come back empty
		// rather than printing a placeholder into the table.
		PartManager::PartColumn missing;
		missing.key = "capacitance";
		missing.isAttribute = true;
		missing.datatype = PartManager::AttributeDataType::Dimension;
		TEST_ASSERT(PartManager::formatAttributeValue(json, missing).isEmpty());
		TEST_ASSERT(PartManager::formatAttributeValue("not json at all", resistance).isEmpty());
	}

	TEST_FUNCTION(treeMatchCountsRollUpLikeStock)
	{
		TEST_START;

		std::vector<PartManager::PartType> types{
			makeType(1, "Capacitor", PartManager::NoParentType),
			makeType(2, "Ceramic Capacitor", 1),
			makeType(4, "Resistor", PartManager::NoParentType)
		};
		std::map<int, int> inStock{ {1, 5}, {2, 20}, {4, 412} };
		// §7a tree filter: only the child and the unrelated root have hits.
		std::map<int, int> matches{ {2, 3}, {4, 1} };

		std::vector<PartManager::CategoryNode> roots =
			PartManager::buildCategoryTree(types, inStock, matches);

		TEST_COMPARE(roots[0].matchCount, 3);              // parent inherits its child's hits
		TEST_COMPARE(roots[0].children[0].matchCount, 3);
		TEST_COMPARE(roots[1].matchCount, 1);
		// No filter at all leaves every count at zero, which is what suppresses the ": n" suffix.
		TEST_COMPARE(PartManager::buildCategoryTree(types, inStock)[0].matchCount, 0);
	}

	TEST_FUNCTION(searchErrorOnlyReportsMalformedQueries)
	{
		TEST_START;

		// An empty box and a well-formed query are both silent, including one that will match
		// nothing — "no results" is an answer, not an error.
		TEST_ASSERT(PartManager::searchError("").isEmpty());
		TEST_ASSERT(PartManager::searchError("   ").isEmpty());
		TEST_ASSERT(PartManager::searchError("resistance>1k").isEmpty());
		TEST_ASSERT(PartManager::searchError("tag:SMD \"power supply\"").isEmpty());
		TEST_ASSERT(PartManager::searchError("nothing_matches_this").isEmpty());

		// A typo has to say something, or the view would just empty itself unexplained.
		TEST_ASSERT(!PartManager::searchError("tag:").isEmpty());
		TEST_ASSERT(!PartManager::searchError("resistance>").isEmpty());
		TEST_ASSERT(!PartManager::searchError("resistance>abc").isEmpty());
	}

};

TEST_INSTANTIATE(TST_MainWindowController);
