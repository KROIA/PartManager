#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_MainWindowController.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <algorithm>
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// The Home tab's widget-free logic (§7a tree assembly + stock aggregation, §7b column
// derivation, §2a display formatting). The widgets around it need a live QApplication
// and are not exercised here.
//
// The §7a all-categories search scope is the one case that needs a real database — a scope
// is only observable against actual rows in more than one category, so it runs against a
// throwaway database in %TEMP%, never the user's own.
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
		ADD_TEST(TST_MainWindowController::theTreeCountsPartsNotOnlyTheOnesInStock);
		ADD_TEST(TST_MainWindowController::searchErrorOnlyReportsMalformedQueries);
		ADD_TEST(TST_MainWindowController::previewShapesTheSelectedPartsFields);
		ADD_TEST(TST_MainWindowController::emptyColumnConfigFallsBackToDerived);
		ADD_TEST(TST_MainWindowController::savedColumnConfigReordersHidesAndResizes);
		ADD_TEST(TST_MainWindowController::columnConfigSurvivesAddedAndRemovedAttributes);
		ADD_TEST(TST_MainWindowController::allCategoryColumnsNameTheCategory);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_MainWindowController::allCategoriesSearchCrossesCategoryBoundaries);
#endif
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

	static PartManager::PartTypeAttribute makeAttribute(const std::string& key)
	{
		PartManager::PartTypeAttribute attribute;
		attribute.key = key;
		attribute.label = key;
		attribute.datatype = PartManager::AttributeDataType::Text;
		return attribute;
	}

	static PartManager::PartTypeListColumn makeListColumn(const std::string& key, bool visible, int widthPx)
	{
		PartManager::PartTypeListColumn column;
		column.columnKey = key;
		column.visible = visible;
		column.widthPx = widthPx;
		return column;
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

		// name, manufacturer, mpn, package, <attributes in declaration order>, files, stock_qty
		TEST_COMPARE(columns.size(), static_cast<size_t>(8));
		TEST_COMPARE(columns[0].key.toStdString(), std::string("name"));
		TEST_COMPARE(columns[3].key.toStdString(), std::string("package"));
		TEST_COMPARE(columns[4].key.toStdString(), std::string("resistance"));
		TEST_ASSERT(columns[4].isAttribute);
		TEST_COMPARE(columns[5].key.toStdString(), std::string("tolerance"));
		// The attachment glyphs; a built-in like the rest, so it can be hidden or moved.
		TEST_COMPARE(columns[6].key.toStdString(), std::string("files"));
		TEST_ASSERT(!columns[6].isAttribute);
		TEST_COMPARE(columns[7].key.toStdString(), std::string("stock_qty"));
		TEST_ASSERT(!columns[7].isAttribute);

		// No attributes at all still yields the built-ins.
		TEST_COMPARE(PartManager::deriveColumns({}).size(), static_cast<size_t>(6));
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

	// The tree's "(n)" counts parts, not in-stock parts. It used to be in-stock, and the symptom
	// was that creating a part with no opening quantity left the number exactly where it was —
	// the one number the user had just changed was the one that did not move.
	TEST_FUNCTION(theTreeCountsPartsNotOnlyTheOnesInStock)
	{
		TEST_START;

		std::vector<PartManager::PartType> types{
			makeType(1, "Passive", PartManager::NoParentType),
			makeType(2, "Resistor", 1)
		};
		// Ten resistors on the shelf, of which two are in stock; the parent holds none itself.
		std::map<int, int> inStock{ {1, 0}, {2, 2} };
		std::map<int, int> partCount{ {1, 0}, {2, 10} };

		std::vector<PartManager::CategoryNode> roots =
			PartManager::buildCategoryTree(types, inStock, std::map<int, int>(), partCount);

		TEST_COMPARE(roots.size(), static_cast<size_t>(1));
		// Both counts roll up through the parent, so a category shows what is underneath it.
		TEST_COMPARE(roots[0].partCount, 10);
		TEST_COMPARE(roots[0].inStockCount, 2);
		TEST_COMPARE(roots[0].children[0].partCount, 10);

		// A part added with zero stock moves partCount and leaves inStockCount alone — which is
		// exactly the case the old label could not show.
		partCount[2] = 11;
		std::vector<PartManager::CategoryNode> after =
			PartManager::buildCategoryTree(types, inStock, std::map<int, int>(), partCount);
		TEST_COMPARE(after[0].partCount, 11);
		TEST_COMPARE(after[0].inStockCount, 2);

		// Not passing the map at all leaves it zero rather than falling back to inStockCount,
		// so a caller that forgets it fails visibly instead of showing a plausible wrong number.
		TEST_COMPARE(PartManager::buildCategoryTree(types, inStock)[0].partCount, 0);
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

	TEST_FUNCTION(previewShapesTheSelectedPartsFields)
	{
		TEST_START;

		const std::string ohm = "\xCE\xA9";

		PartManager::PartTypeAttribute resistance;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.unit = ohm;
		resistance.datatype = PartManager::AttributeDataType::Dimension;

		// Declared by the type, never filled in by this part.
		PartManager::PartTypeAttribute tolerance;
		tolerance.key = "tolerance";
		tolerance.label = "Tolerance";
		tolerance.datatype = PartManager::AttributeDataType::Text;

		PartManager::Part part;
		part.id = 42;
		part.name = "RES-0603-4K7";
		part.manufacturer = "Yageo";
		part.mpn = "";                      // deliberately blank
		part.package = "0603";
		part.description = "thin film";
		part.attributes = "{\"resistance\":{\"value\":4700,\"unit\":\"" + ohm + "\"}}";
		part.stockQty = 12;

		PartManager::Tag smd;
		smd.id = 1;
		smd.name = "SMD";
		smd.color = "#E53935";

		PartManager::PartPreview preview = PartManager::buildPreview(part,
			PartManager::deriveColumns({ resistance, tolerance }),
			{ smd },
			PartManager::datasheetState("rc0603.pdf", true));

		TEST_COMPARE(preview.partId, 42);
		TEST_COMPARE(preview.name.toStdString(), std::string("RES-0603-4K7"));
		TEST_COMPARE(preview.description.toStdString(), std::string("thin film"));
		TEST_COMPARE(preview.tags.size(), static_cast<size_t>(1));

		// The name is the panel title rather than a field; the blank MPN and the unfilled
		// attribute are dropped, so what is left is Manufacturer, Package, Resistance, Stock,
		// Datasheet — and the datasheet line is always last.
		TEST_COMPARE(preview.fields.size(), static_cast<size_t>(5));
		TEST_COMPARE(preview.fields[0].value.toStdString(), std::string("Yageo"));
		TEST_COMPARE(preview.fields[1].value.toStdString(), std::string("0603"));
		TEST_COMPARE(preview.fields[2].label.toStdString(), std::string("Resistance"));
		// §2a formatting is the table's own, not a second implementation for the panel.
		TEST_COMPARE(preview.fields[2].value.toStdString(), std::string("4.7 k") + ohm);
		TEST_COMPARE(preview.fields[3].value.toStdString(), std::string("12"));
		TEST_COMPARE(preview.fields.back().value.toStdString(), std::string("rc0603.pdf"));

		// A part with no id is the empty state the panel renders instead of a blank form.
		TEST_COMPARE(PartManager::buildPreview(PartManager::Part(), {}, {}, QString()).partId, 0);

		// The three datasheet states have to be distinguishable: none, present, gone from disk.
		TEST_ASSERT(!PartManager::datasheetState(QString(), false).isEmpty());
		TEST_ASSERT(PartManager::datasheetState("rc0603.pdf", false).contains("rc0603.pdf"));
		TEST_ASSERT(PartManager::datasheetState("rc0603.pdf", false) != QString("rc0603.pdf"));
	}

	TEST_FUNCTION(emptyColumnConfigFallsBackToDerived)
	{
		TEST_START;

		std::vector<PartManager::PartColumn> derived = PartManager::deriveColumns({ makeAttribute("resistance") });

		// The whole compatibility story: no saved rows, nothing changes.
		std::vector<PartManager::PartColumn> applied =
			PartManager::applyColumnConfig(derived, std::vector<PartManager::PartTypeListColumn>());
		TEST_COMPARE(applied.size(), derived.size());
		for (size_t index = 0; index < applied.size(); ++index)
		{
			TEST_COMPARE(applied[index].key.toStdString(), derived[index].key.toStdString());
			TEST_ASSERT_M(applied[index].visible, "a derived column is visible");
			TEST_COMPARE(applied[index].widthPx, 0);
		}
	}

	TEST_FUNCTION(savedColumnConfigReordersHidesAndResizes)
	{
		TEST_START;

		std::vector<PartManager::PartColumn> derived = PartManager::deriveColumns({ makeAttribute("resistance") });
		std::vector<PartManager::PartTypeListColumn> config{
			makeListColumn("stock_qty", true, 80),
			makeListColumn("resistance", true, 120),
			makeListColumn("name", true, 0),
			makeListColumn("mpn", false, 0),
			makeListColumn("manufacturer", false, 0),
			makeListColumn("package", false, 0)
		};

		std::vector<PartManager::PartColumn> applied = PartManager::applyColumnConfig(derived, config);

		// `name` is pinned first however the layout ordered it — the table hangs the part id and
		// the tag chips off column 0. Seven, not six: `files` is a built-in this saved layout
		// predates, so it is appended by the rule the next test covers.
		TEST_COMPARE(applied.size(), static_cast<size_t>(7));
		TEST_COMPARE(applied[0].key.toStdString(), std::string("name"));
		TEST_COMPARE(applied[1].key.toStdString(), std::string("stock_qty"));
		TEST_COMPARE(applied[1].widthPx, 80);
		TEST_COMPARE(applied[2].key.toStdString(), std::string("resistance"));
		TEST_COMPARE(applied[2].widthPx, 120);
		TEST_ASSERT_M(!applied[3].visible, "a hidden column stays in the list, marked hidden");

		// toColumnConfig() is the inverse, so a save-then-load leaves the layout where it was.
		std::vector<PartManager::PartColumn> reapplied =
			PartManager::applyColumnConfig(derived, PartManager::toColumnConfig(applied));
		TEST_COMPARE(reapplied.size(), applied.size());
		for (size_t index = 0; index < applied.size(); ++index)
		{
			TEST_COMPARE(reapplied[index].key.toStdString(), applied[index].key.toStdString());
			TEST_COMPARE(reapplied[index].widthPx, applied[index].widthPx);
			TEST_ASSERT(reapplied[index].visible == applied[index].visible);
		}
	}

	TEST_FUNCTION(columnConfigSurvivesAddedAndRemovedAttributes)
	{
		TEST_START;

		// The layout was saved when the type still had `tolerance` and not yet `power`.
		std::vector<PartManager::PartTypeListColumn> config{
			makeListColumn("name", true, 0),
			makeListColumn("tolerance", true, 0),
			makeListColumn("stock_qty", true, 0)
		};
		std::vector<PartManager::PartColumn> derived =
			PartManager::deriveColumns({ makeAttribute("resistance"), makeAttribute("power") });

		std::vector<PartManager::PartColumn> applied = PartManager::applyColumnConfig(derived, config);

		// The deleted attribute is dropped; everything the layout never mentioned is appended
		// visible rather than silently disappearing.
		auto keyAt = [&applied](size_t index) { return applied[index].key.toStdString(); };
		TEST_COMPARE(applied.size(), static_cast<size_t>(8));
		TEST_COMPARE(keyAt(0), std::string("name"));
		TEST_COMPARE(keyAt(1), std::string("stock_qty"));
		TEST_COMPARE(keyAt(2), std::string("manufacturer"));
		TEST_COMPARE(keyAt(5), std::string("resistance"));
		TEST_COMPARE(keyAt(6), std::string("power"));
		// `files` was added to the built-ins after this layout was saved, so it lands at the end
		// rather than being dropped — the same rule that keeps a newly added attribute visible.
		TEST_COMPARE(keyAt(7), std::string("files"));
		for (const PartManager::PartColumn& column : applied)
		{
			TEST_ASSERT_M(column.key != QString("tolerance"), "a deleted attribute must not stay a column");
		}
	}

	// A cross-category result is unreadable without the category on every row, and the column
	// carrying it has to be an ordinary §7b column or none of the table's machinery reaches it.
	TEST_FUNCTION(allCategoryColumnsNameTheCategory)
	{
		TEST_START;

		std::vector<PartManager::PartColumn> columns = PartManager::allCategoryColumns();

		// The built-ins plus one: name, type, manufacturer, mpn, package, files, stock_qty.
		TEST_COMPARE(columns.size(), static_cast<size_t>(7));
		TEST_COMPARE(columns[0].key.toStdString(), std::string("name"));
		TEST_COMPARE(columns[1].key.toStdString(), std::string("type"));
		TEST_ASSERT_M(!columns[1].isAttribute, "the category column is a built-in, not an attribute");
		TEST_ASSERT_M(columns[1].visible, "the category column is what makes the result readable");

		// No per-type attributes: the rows span every type, so such a column would be blank on
		// nearly all of them.
		for (const PartManager::PartColumn& column : columns)
		{
			TEST_ASSERT_M(!column.isAttribute, "an all-categories table declares no attributes");
		}
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// The whole point of the Ctrl+F scope: the same query, the same grammar, run over the
	// database instead of the selected category — and the scoped search still not doing that.
	TEST_FUNCTION(allCategoriesSearchCrossesCategoryBoundaries)
	{
		TEST_START;

		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / "PartManager_TST_MainWindowController_scope";
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Scope", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// Names nothing the seeded templates use, so the counts below are exactly these rows.
		const int alphaId = addType(handle->connection(), "ScopeAlpha");
		const int betaId = addType(handle->connection(), "ScopeBeta");
		TEST_ASSERT(alphaId != PartManager::NoParentType && betaId != PartManager::NoParentType);
		TEST_ASSERT(addPart(handle->connection(), alphaId, "SCOPEMARK-alpha") != 0);
		TEST_ASSERT(addPart(handle->connection(), betaId, "SCOPEMARK-beta") != 0);

		PartManager::MainWindowController controller(std::move(handle));

		// Scoped to ScopeAlpha, which is what a click in the tree gives you: its own part only.
		const std::vector<PartManager::PartColumn> scoped = controller.columnsFor(alphaId);
		std::vector<PartManager::PartRow> scopedRows =
			controller.partsFor(alphaId, scoped, "SCOPEMARK");
		TEST_COMPARE(scopedRows.size(), static_cast<size_t>(1));
		TEST_COMPARE(scopedRows[0].typeName.toStdString(), std::string("ScopeAlpha"));

		// Same query, same selected category, all-categories scope on: the part in the other
		// category comes back too.
		const std::vector<PartManager::PartColumn> all = PartManager::allCategoryColumns();
		std::vector<PartManager::PartRow> allRows =
			controller.partsFor(alphaId, all, "SCOPEMARK", true);
		TEST_COMPARE(allRows.size(), static_cast<size_t>(2));

		// And each row says where it lives — the Category column is filled from the row's type,
		// not left blank the way an unknown built-in key would be.
		std::vector<std::string> categories;
		for (const PartManager::PartRow& row : allRows)
		{
			TEST_ASSERT_M(row.cells.size() == static_cast<int>(all.size()),
				"every all-categories column has a cell");
			categories.push_back(row.cells.at(1).toStdString());
		}
		std::sort(categories.begin(), categories.end());
		TEST_COMPARE(categories[0], std::string("ScopeAlpha"));
		TEST_COMPARE(categories[1], std::string("ScopeBeta"));

		// An empty box in all-categories scope is "every part in the database", not "nothing" —
		// the seeded templates carry no parts, so the two above are still the whole answer.
		TEST_COMPARE(controller.partsFor(alphaId, all, QString(), true).size(),
			static_cast<size_t>(2));
	}

	static int addType(SQLiteWrapper::SQLite& db, const std::string& name)
	{
		PartManager::PartType type;
		type.name = name;
		type.domain = "electronic";
		type.parentTypeId = PartManager::NoParentType;
		return PartManager::PartTypeRepository::insertType(db, type);
	}

	static int addPart(SQLiteWrapper::SQLite& db, int typeId, const std::string& name)
	{
		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		return PartManager::PartRepository::insertPart(db, part);
	}
#endif

};

TEST_INSTANTIATE(TST_MainWindowController);
