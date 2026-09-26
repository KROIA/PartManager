#pragma once

#include "UnitTest.h"
#include "import/PartManager_PartListMigration.h"

// Bulk migration of a pasted part list (§4, §5) — the clipboard half of the import path. No
// database, no widgets: parsing, the header guess and the duplicate detection are pure functions
// over plain data, which is why they live in core and can be proven with three fake parts.
//
// The fixtures are the three shapes a user actually pastes: a bare newline-separated list of
// order codes, an Excel column block (tab-separated, with a header row), and a headerless
// semicolon list they typed themselves.
class TST_PartListMigration : public UnitTest::Test
{
	TEST_CLASS(TST_PartListMigration)
public:
	TST_PartListMigration()
		: Test("TST_PartListMigration")
	{
		ADD_TEST(TST_PartListMigration::bareListPastesAsOneColumn);
		ADD_TEST(TST_PartListMigration::excelPasteMapsItsHeaders);
		ADD_TEST(TST_PartListMigration::headerlessPasteKeepsItsFirstLine);
		ADD_TEST(TST_PartListMigration::knownPartNumbersComeBackExisting);
		ADD_TEST(TST_PartListMigration::sellerNumbersAlsoCountAsKnown);
		ADD_TEST(TST_PartListMigration::junkRowsAndStockCellsAreHandled);
		ADD_TEST(TST_PartListMigration::aMouserColumnIsItsOwnColumn);
		ADD_TEST(TST_PartListMigration::aMouserOnlyRowIsAWholeRow);
	}

private:

	static std::vector<PartManager::Part> inventory()
	{
		PartManager::Part resistor;
		resistor.id = 11;
		resistor.name = "4k7 0603";
		resistor.mpn = "RC0603FR-074K7L";

		PartManager::Part regulator;
		regulator.id = 22;
		regulator.name = "LM317";
		regulator.mpn = "LM317T";

		return { resistor, regulator };
	}

	static std::string statusOf(const PartManager::MigrationRow& row)
	{
		return std::string(PartManager::migrationStatusKey(row.status));
	}

	// The paste this feature exists for: a column of order codes, nothing else. detectDelimiter()
	// answers ';' for every one of these lines, so without the bare-list branch parseCsv() would
	// take line one for a header and the user would lose their first part.
	TEST_FUNCTION(bareListPastesAsOneColumn)
	{
		TEST_START;

		const std::string text =
			"RC0603FR-074K7L\r\n"
			"  LM317T  \r\n"
			"\r\n"                 // a blank line in the middle is not a part
			"NE555P\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, false));
		TEST_COMPARE(table.headers.size(), static_cast<size_t>(1));
		TEST_COMPARE(table.rows.size(), static_cast<size_t>(3));
		TEST_COMPARE(table.rows[0][0], std::string("RC0603FR-074K7L"));
		TEST_ASSERT_M(table.rows[1][0] == std::string("LM317T"),
			"a pasted line keeps its spaces only in the clipboard: " + table.rows[1][0]);
		TEST_COMPARE(table.rows[2][0], std::string("NE555P"));

		// One column is a part-number list by definition, whatever the header is called.
		const PartManager::MigrationColumnMapping mapping =
			PartManager::guessMigrationMapping(table.headers);
		TEST_COMPARE(mapping.mpn, 0);
		TEST_COMPARE(mapping.stock, PartManager::NoMigrationColumn);

		// Nothing usable at all is the one failure case.
		PartManager::CsvTable empty;
		TEST_ASSERT_M(!PartManager::parsePastedList("  \n\n", PartManager::AutoDetectDelimiter,
			empty, false), "a paste with no usable line has nothing to migrate and must say so");

		// The status keys are stored and logged, so they are part of the contract.
		TEST_COMPARE(std::string(PartManager::migrationStatusKey(PartManager::MigrationStatus::Pending)),
			std::string("pending"));
		TEST_COMPARE(std::string(PartManager::migrationStatusKey(PartManager::MigrationStatus::Existing)),
			std::string("existing"));
		TEST_COMPARE(std::string(PartManager::migrationStatusKey(PartManager::MigrationStatus::Created)),
			std::string("created"));
		TEST_COMPARE(std::string(PartManager::migrationStatusKey(PartManager::MigrationStatus::Skipped)),
			std::string("skipped"));
	}

	// A block of cells copied out of Excel arrives tab-separated, header row included.
	TEST_FUNCTION(excelPasteMapsItsHeaders)
	{
		TEST_START;

		const std::string text =
			"Part Number\tStock Count\tManufacturer\tDescription\n"
			"RC0603FR-074K7L\t250\tYageo\t4k7 1% 0603\n"
			"NE555P\t5\tTexas Instruments\ttimer\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, true));
		TEST_COMPARE(table.delimiter, '\t');
		TEST_COMPARE(table.headers.size(), static_cast<size_t>(4));
		TEST_COMPARE(table.rows.size(), static_cast<size_t>(2));

		const PartManager::MigrationColumnMapping mapping =
			PartManager::guessMigrationMapping(table.headers);
		TEST_COMPARE(mapping.mpn, 0);
		TEST_COMPARE(mapping.stock, 1);
		TEST_COMPARE(mapping.manufacturer, 2);
		TEST_COMPARE(mapping.description, 3);
		TEST_COMPARE(mapping.notes, PartManager::NoMigrationColumn);

		const std::vector<PartManager::MigrationRow> rows =
			PartManager::buildMigrationRows(table, mapping, inventory());
		TEST_COMPARE(rows.size(), static_cast<size_t>(2));
		TEST_COMPARE(rows[0].manufacturer, std::string("Yageo"));
		TEST_COMPARE(rows[0].description, std::string("4k7 1% 0603"));
		TEST_COMPARE(rows[0].stock, 250);
		TEST_ASSERT(rows[0].stockGiven);

		// 'Manufacturer_Part_Number' is a part number, not the manufacturer column — the
		// underscores are the trap a literal substring match falls into.
		const PartManager::MigrationColumnMapping kicad = PartManager::guessMigrationMapping(
			{ "Manufacturer_Part_Number", "Manufacturer_Name", "Bestand", "Bemerkung" });
		TEST_COMPARE(kicad.mpn, 0);
		TEST_COMPARE(kicad.manufacturer, 1);
		TEST_COMPARE(kicad.stock, 2);
		TEST_COMPARE(kicad.notes, 3);

		// Nothing recognisable must guess nothing rather than guess wrong.
		const PartManager::MigrationColumnMapping unknown =
			PartManager::guessMigrationMapping({ "Spalte A", "Spalte B" });
		TEST_COMPARE(unknown.mpn, PartManager::NoMigrationColumn);
		TEST_COMPARE(unknown.stock, PartManager::NoMigrationColumn);

		// A file with only a 'Comment' means it as the description; a file with both means the
		// second one as a note.
		TEST_COMPARE(PartManager::guessMigrationMapping({ "MPN", "Comment" }).description, 1);
		const PartManager::MigrationColumnMapping both =
			PartManager::guessMigrationMapping({ "MPN", "Description", "Comment" });
		TEST_COMPARE(both.description, 1);
		TEST_COMPARE(both.notes, 2);
	}

	TEST_FUNCTION(headerlessPasteKeepsItsFirstLine)
	{
		TEST_START;

		const std::string text =
			"RC0603FR-074K7L;250\n"
			"NE555P;5\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, false));
		TEST_COMPARE(table.headers.size(), static_cast<size_t>(2));
		TEST_COMPARE(table.headers[0], std::string("Column 1"));
		TEST_COMPARE(table.headers[1], std::string("Column 2"));
		// The point of the flag: line one is a part, not a caption.
		TEST_COMPARE(table.rows.size(), static_cast<size_t>(2));
		TEST_COMPARE(table.rows[0][0], std::string("RC0603FR-074K7L"));

		// 'Column N' is recognised by nothing, which is correct — the user picks the columns.
		const PartManager::MigrationColumnMapping mapping =
			PartManager::guessMigrationMapping(table.headers);
		TEST_COMPARE(mapping.mpn, PartManager::NoMigrationColumn);

		// With the same text read *as* a header the first line disappears from the rows, which
		// is the difference the flag has to make.
		PartManager::CsvTable headed;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, headed, true));
		TEST_COMPARE(headed.rows.size(), static_cast<size_t>(1));
	}

	TEST_FUNCTION(knownPartNumbersComeBackExisting)
	{
		TEST_START;

		const std::string text =
			"rc0603fr-074k7l\n"      // already in stock, wrong case
			"NOT-A-PART\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, false));

		const std::vector<PartManager::MigrationRow> rows = PartManager::buildMigrationRows(
			table, PartManager::guessMigrationMapping(table.headers), inventory());

		TEST_COMPARE(rows.size(), static_cast<size_t>(2));
		TEST_COMPARE(statusOf(rows[0]), std::string("existing"));
		TEST_COMPARE(rows[0].matchedPartId, 11);
		// §4's rule: a row that matches nothing is never invented here. The caller decides.
		TEST_COMPARE(statusOf(rows[1]), std::string("pending"));
		TEST_COMPARE(rows[1].matchedPartId, PartManager::NoPartId);
		TEST_COMPARE(rows[1].createdPartId, PartManager::NoPartId);

		// The original line travels with every row, header-keyed.
		TEST_ASSERT_M(rows[1].rawJson.find("NOT-A-PART") != std::string::npos,
			"the raw row must survive the migration: " + rows[1].rawJson);
	}

	TEST_FUNCTION(sellerNumbersAlsoCountAsKnown)
	{
		TEST_START;

		// A user pasting their order history pastes Mouser numbers, which are nowhere in
		// part.mpn — they live in part_seller_link. Without the links every such line reads as
		// a new part and the migration duplicates the whole inventory.
		const std::string text = "595-RC0603\nLM317T\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, false));
		const PartManager::MigrationColumnMapping mapping =
			PartManager::guessMigrationMapping(table.headers);

		PartManager::PartSellerLink link;
		link.partId = 11;
		link.sellerPartNumber = "595-RC0603";

		const std::vector<PartManager::MigrationRow> linked =
			PartManager::buildMigrationRows(table, mapping, inventory(), { link });
		TEST_COMPARE(statusOf(linked[0]), std::string("existing"));
		TEST_COMPARE(linked[0].matchedPartId, 11);
		TEST_COMPARE(statusOf(linked[1]), std::string("existing"));
		TEST_COMPARE(linked[1].matchedPartId, 22);

		// The half that regressing would be silent: without the links the distributor number is
		// an unknown part again.
		const std::vector<PartManager::MigrationRow> unlinked =
			PartManager::buildMigrationRows(table, mapping, inventory());
		TEST_COMPARE(statusOf(unlinked[0]), std::string("pending"));
		TEST_COMPARE(unlinked[0].matchedPartId, PartManager::NoPartId);
	}

	TEST_FUNCTION(junkRowsAndStockCellsAreHandled)
	{
		TEST_START;

		const std::string text =
			"MPN;Qty\n"
			"NE555P;12 pcs\n"        // a unit suffix is how a human writes a stock count
			"BC547;n/a\n"            // not a number: not a stock count either
			"BC557;0\n"              // a real zero, which is not the same as no column
			";\n";                   // trailing spreadsheet junk

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, true));

		const std::vector<PartManager::MigrationRow> rows = PartManager::buildMigrationRows(
			table, PartManager::guessMigrationMapping(table.headers), inventory());

		TEST_COMPARE(rows.size(), static_cast<size_t>(3));
		TEST_COMPARE(rows[0].stock, 12);
		TEST_ASSERT(rows[0].stockGiven);
		TEST_COMPARE(rows[1].stock, 0);
		TEST_ASSERT_M(!rows[1].stockGiven,
			"garbage in the stock cell must not read as zero parts in the drawer");
		TEST_COMPARE(rows[2].stock, 0);
		TEST_ASSERT_M(rows[2].stockGiven, "a written 0 is a stock count, an absent one is not");

		// No stock column at all leaves every row stockGiven == false.
		PartManager::CsvTable bare;
		TEST_ASSERT(PartManager::parsePastedList("NE555P\n", PartManager::AutoDetectDelimiter,
			bare, false));
		const std::vector<PartManager::MigrationRow> bareRows = PartManager::buildMigrationRows(
			bare, PartManager::guessMigrationMapping(bare.headers), inventory());
		TEST_COMPARE(bareRows.size(), static_cast<size_t>(1));
		TEST_ASSERT(!bareRows[0].stockGiven);
	}

	// The distributor number and the manufacturer number are two different identifiers living in
	// two different tables, so they get two different columns — and the guess has to keep them
	// apart in both directions, which is the whole difficulty: 'Mouser Part Number' contains the
	// words "part number", and 'Manufacturer_Part_Number' must not be dragged off by the new rule.
	TEST_FUNCTION(aMouserColumnIsItsOwnColumn)
	{
		TEST_START;

		const PartManager::MigrationColumnMapping both = PartManager::guessMigrationMapping(
			{ "Mouser Part Number", "Manufacturer Part Number", "Manufacturer", "Qty" });
		TEST_COMPARE(both.mouser, 0);
		TEST_ASSERT_M(both.mpn == 1,
			"a file carrying both numbers must map each to its own field");
		TEST_COMPARE(both.manufacturer, 2);
		TEST_COMPARE(both.stock, 3);

		// The trap in the other direction, spelled the way KiCad writes it. This header names no
		// distributor at all and must stay the manufacturer part number.
		const PartManager::MigrationColumnMapping kicad = PartManager::guessMigrationMapping(
			{ "Manufacturer_Part_Number", "Manufacturer_Name" });
		TEST_ASSERT_M(kicad.mouser == PartManager::NoMigrationColumn,
			"'Manufacturer_Part_Number' is not a Mouser column");
		TEST_COMPARE(kicad.mpn, 0);
		TEST_COMPARE(kicad.manufacturer, 1);

		// Every spelling of the Mouser column a pasted order history actually carries.
		for (const char* header : { "Mouser", "Mouser No", "Mouser Nr", "Mouser #",
			"Mouser Artikelnummer", "Distributor Part Number", "Distributor Part No" })
		{
			const PartManager::MigrationColumnMapping mapping =
				PartManager::guessMigrationMapping({ std::string(header), "Qty" });
			TEST_ASSERT_M(mapping.mouser == 0,
				std::string("unrecognised Mouser header: ") + header);
		}

		// Either column on its own is enough to identify a part; neither is not.
		PartManager::MigrationColumnMapping nothing;
		TEST_ASSERT(!PartManager::hasPartNumberColumn(nothing));
		PartManager::MigrationColumnMapping mouserOnly;
		mouserOnly.mouser = 0;
		TEST_ASSERT(PartManager::hasPartNumberColumn(mouserOnly));
		PartManager::MigrationColumnMapping mpnOnly;
		mpnOnly.mpn = 0;
		TEST_ASSERT(PartManager::hasPartNumberColumn(mpnOnly));
	}

	// A list of order codes and nothing else is a complete inventory: every row names a part
	// exactly once. It must survive the "all mapped cells empty" drop rule, and its numbers must
	// resolve against part_seller_link — which is where a distributor number is stored and where
	// part.mpn will never have it.
	TEST_FUNCTION(aMouserOnlyRowIsAWholeRow)
	{
		TEST_START;

		const std::string text =
			"Mouser Part Number\n"
			"595-LM358DR\n"
			"81-GRM188R71C104KA1D\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parsePastedList(text, PartManager::AutoDetectDelimiter, table, true));
		// One column, so the bare-list rule claims it for the MPN whatever it is called — the user
		// re-points it at the Mouser combo, which is what this mapping stands for.
		PartManager::MigrationColumnMapping mapping;
		mapping.mouser = 0;

		PartManager::PartSellerLink link;
		link.partId = 22;
		link.sellerPartNumber = "595-LM358DR";

		const std::vector<PartManager::MigrationRow> rows =
			PartManager::buildMigrationRows(table, mapping, inventory(), { link });
		TEST_ASSERT_M(rows.size() == static_cast<size_t>(2),
			"a row whose only mapped cell is the Mouser number is still a row");
		TEST_COMPARE(rows[0].mouserPartNumber, std::string("595-LM358DR"));
		TEST_ASSERT_M(rows[0].mpn.empty(), "an unmapped part-number column must stay empty");
		// The seller link is what makes it a duplicate — matched against sellerPartNumber, never
		// against part.mpn.
		TEST_COMPARE(statusOf(rows[0]), std::string("existing"));
		TEST_COMPARE(rows[0].matchedPartId, 22);
		TEST_COMPARE(statusOf(rows[1]), std::string("pending"));

		// Both columns mapped: each number is looked up in the table it belongs to, and a hit on
		// either one is enough.
		PartManager::CsvTable wide;
		TEST_ASSERT(PartManager::parsePastedList(
			"Mouser Part Number;Manufacturer Part Number\n"
			"999-NOTHING;LM317T\n"
			"595-LM358DR;ALSO-NOTHING\n",
			PartManager::AutoDetectDelimiter, wide, true));
		const PartManager::MigrationColumnMapping wideMapping =
			PartManager::guessMigrationMapping(wide.headers);
		TEST_COMPARE(wideMapping.mouser, 0);
		TEST_COMPARE(wideMapping.mpn, 1);
		const std::vector<PartManager::MigrationRow> wideRows =
			PartManager::buildMigrationRows(wide, wideMapping, inventory(), { link });
		TEST_ASSERT_M(statusOf(wideRows[0]) == std::string("existing"),
			"a known manufacturer number is a duplicate even when the Mouser number is unknown");
		TEST_COMPARE(wideRows[0].matchedPartId, 22);
		TEST_ASSERT_M(statusOf(wideRows[1]) == std::string("existing"),
			"a known Mouser number is a duplicate even when the manufacturer number is unknown");
		TEST_COMPARE(wideRows[1].matchedPartId, 22);

		// The fallback that must not regress: a list mapped entirely to the part-number column
		// still matches its distributor numbers, exactly as it did before this column existed.
		PartManager::CsvTable legacy;
		TEST_ASSERT(PartManager::parsePastedList("595-LM358DR\n",
			PartManager::AutoDetectDelimiter, legacy, false));
		const std::vector<PartManager::MigrationRow> legacyRows = PartManager::buildMigrationRows(
			legacy, PartManager::guessMigrationMapping(legacy.headers), inventory(), { link });
		TEST_COMPARE(statusOf(legacyRows[0]), std::string("existing"));
		TEST_COMPARE(legacyRows[0].matchedPartId, 22);
	}
};

TEST_INSTANTIATE(TST_PartListMigration);
