#pragma once

#include "UnitTest.h"
#include "import/PartManager_BomCsvImport.h"

// The CSV/BOM reader behind the import dialog (§4, §5). No database, no widgets: parsing, the
// header guess and the part matching are all pure functions over plain data, which is the whole
// reason they live in core rather than in the dialog.
//
// The fixtures are the two shapes that actually turn up: a KiCad "Generate BOM" export (comma,
// Reference/Value/Quantity, no MPN) and a hand-kept spreadsheet in the shape of the user's own
// .claude/DefaultParts.csv (semicolon, Stockcount/MouserNR/Link).
class TST_BomCsvImport : public UnitTest::Test
{
	TEST_CLASS(TST_BomCsvImport)
public:
	TST_BomCsvImport()
		: Test("TST_BomCsvImport")
	{
		ADD_TEST(TST_BomCsvImport::delimiterAndQuotingSurviveTheRoundTrip);
		ADD_TEST(TST_BomCsvImport::kicadAndSpreadsheetHeadersAreRecognised);
		ADD_TEST(TST_BomCsvImport::quantityFallsBackToTheDesignatorCount);
		ADD_TEST(TST_BomCsvImport::matchingPrefersTheMpnAndKeepsMissesUnresolved);
		ADD_TEST(TST_BomCsvImport::kicadExportMapsBothPartNumberColumns);
		ADD_TEST(TST_BomCsvImport::distributorNumbersMatchThroughSellerLinks);
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

	TEST_FUNCTION(delimiterAndQuotingSurviveTheRoundTrip)
	{
		TEST_START;

		// A quoted field holding the delimiter is the case a naive split gets wrong, and it is
		// how every exporter writes a description.
		const std::string text =
			"Reference;Value;Description\r\n"
			"R1;4k7;\"resistor, 1%\"\r\n"
			"\r\n"                      // Excel's trailing blank line
			"C1;100n;\"a \"\"quoted\"\" one\"\r\n";

		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parseCsv(text, PartManager::AutoDetectDelimiter, table));
		TEST_COMPARE(table.delimiter, ';');
		TEST_COMPARE(table.headers.size(), static_cast<size_t>(3));
		TEST_COMPARE(table.rows.size(), static_cast<size_t>(2));
		TEST_COMPARE(table.rows[0][2], std::string("resistor, 1%"));
		TEST_COMPARE(table.rows[1][2], std::string("a \"quoted\" one"));

		// The comma inside that quoted description must not out-vote the four real semicolons.
		TEST_COMPARE(PartManager::detectDelimiter("Reference;Value;\"a, b\";Qty"), ';');
		TEST_COMPARE(PartManager::detectDelimiter("Reference,Value,Qty"), ',');
		TEST_COMPARE(PartManager::detectDelimiter("Reference\tValue\tQty"), '\t');
		// One column, no separator at all — still a table, not a parse failure.
		TEST_COMPARE(PartManager::detectDelimiter("MPN"), ';');

		PartManager::CsvTable empty;
		TEST_ASSERT_M(!PartManager::parseCsv("\n\n", PartManager::AutoDetectDelimiter, empty),
			"a file with no header line has nothing to map and must say so");
	}

	TEST_FUNCTION(kicadAndSpreadsheetHeadersAreRecognised)
	{
		TEST_START;

		// KiCad's own BOM columns.
		const PartManager::BomColumnMapping kicad = PartManager::guessMapping(
			{ "Id", "Reference", "Value", "Footprint", "Quantity", "MPN" });
		TEST_COMPARE(kicad.designators, 1);
		TEST_COMPARE(kicad.name, 2);
		TEST_COMPARE(kicad.quantity, 4);
		TEST_COMPARE(kicad.mpn, 5);

		// The user's own list (.claude/DefaultParts.csv). 'Link' is recognised by nothing, which
		// is correct — it is not one of the four fields.
		const PartManager::BomColumnMapping spreadsheet =
			PartManager::guessMapping({ "Stockcount", "MouserNR", "Link" });
		TEST_COMPARE(spreadsheet.quantity, 0);
		TEST_COMPARE(spreadsheet.mpn, 1);
		TEST_COMPARE(spreadsheet.designators, PartManager::NoCsvColumn);

		// Nothing recognisable must guess nothing rather than guess wrong — a wrong pre-fill the
		// user does not notice is worse than an empty one they have to fill in.
		const PartManager::BomColumnMapping unknown =
			PartManager::guessMapping({ "Spalte A", "Spalte B" });
		TEST_COMPARE(unknown.designators, PartManager::NoCsvColumn);
		TEST_COMPARE(unknown.mpn, PartManager::NoCsvColumn);
		TEST_COMPARE(unknown.quantity, PartManager::NoCsvColumn);
		TEST_COMPARE(unknown.name, PartManager::NoCsvColumn);
	}

	TEST_FUNCTION(quantityFallsBackToTheDesignatorCount)
	{
		TEST_START;

		TEST_COMPARE(PartManager::countDesignators("R1,R2,R5"), 3);
		TEST_COMPARE(PartManager::countDesignators("R1 R2  R5,"), 3);
		TEST_COMPARE(PartManager::countDesignators(""), 0);

		// A KiCad BOM grouped by value has no Quantity column: the designator list is the quantity.
		const std::string text =
			"Reference,Value\n"
			"R1 R2 R5,4k7\n"
			"U1,LM317T\n";
		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parseCsv(text, PartManager::AutoDetectDelimiter, table));

		std::vector<PartManager::BomRow> rows =
			PartManager::buildRows(table, PartManager::guessMapping(table.headers), inventory());
		TEST_COMPARE(rows.size(), static_cast<size_t>(2));
		TEST_COMPARE(rows[0].quantityPerUnit, 3);
		TEST_COMPARE(rows[1].quantityPerUnit, 1);

		// A quantity column wins over the count, and a non-numeric one falls back rather than
		// throwing the row away — a "n/a" cell must not cost the user a BOM line.
		const std::string withQuantity =
			"Reference,Value,Qty\n"
			"R1 R2 R5,4k7,10\n"
			"C1 C2,100n,n/a\n";
		PartManager::CsvTable quantityTable;
		TEST_ASSERT(PartManager::parseCsv(withQuantity, PartManager::AutoDetectDelimiter, quantityTable));
		rows = PartManager::buildRows(quantityTable,
			PartManager::guessMapping(quantityTable.headers), inventory());
		TEST_COMPARE(rows[0].quantityPerUnit, 10);
		TEST_COMPARE(rows[1].quantityPerUnit, 2);
	}

	TEST_FUNCTION(matchingPrefersTheMpnAndKeepsMissesUnresolved)
	{
		TEST_START;

		const std::string text =
			"Reference;MPN;Value\n"
			"R1;rc0603fr-074k7l;4k7\n"       // MPN, wrong case
			"U1;;LM317T\n"                   // no MPN, the value is one
			"D1;;4k7 0603\n"                 // no MPN, the value is a part *name*
			"X1;NOT-A-PART;whatever\n"       // matches nothing
			";;\n";                          // trailing junk, not a line
		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parseCsv(text, PartManager::AutoDetectDelimiter, table));

		const std::vector<PartManager::BomRow> rows =
			PartManager::buildRows(table, PartManager::guessMapping(table.headers), inventory());

		// The all-empty row is gone; the four real ones are not.
		TEST_COMPARE(rows.size(), static_cast<size_t>(4));
		TEST_COMPARE(rows[0].matchedPartId, 11);
		TEST_COMPARE(rows[1].matchedPartId, 22);
		TEST_COMPARE(rows[2].matchedPartId, 11);
		// §4: an unmatched row stays unresolved. It is never invented as a new part here.
		TEST_COMPARE(rows[3].matchedPartId, PartManager::NoPartId);

		// The original line travels with every row, header-keyed, so a wrong mapping is still
		// recoverable months later.
		TEST_ASSERT_M(rows[3].rawJson.find("NOT-A-PART") != std::string::npos,
			"the raw row must survive the import: " + rows[3].rawJson);
		TEST_ASSERT_M(rows[3].rawJson.find("Reference") != std::string::npos,
			"the raw row must be header-keyed: " + rows[3].rawJson);
	}

	// The header this project is actually fed: KiCad 9's grouped BOM export, which carries a
	// distributor number *and* a manufacturer number and fills whichever the schematic symbol
	// happened to have. Taken verbatim from .claude/Nucleo_DSP10_Extension.csv.
	TEST_FUNCTION(kicadExportMapsBothPartNumberColumns)
	{
		TEST_START;

		const std::vector<std::string> headers = { "Reference", "Qty", "Value", "DNP",
			"Mouser Part Number", "Manufacturer_Name", "Manufacturer_Part_Number" };
		const PartManager::BomColumnMapping mapping = PartManager::guessMapping(headers);

		TEST_COMPARE(mapping.designators, 0);
		TEST_COMPARE(mapping.quantity, 1);
		TEST_COMPARE(mapping.name, 2);
		// The distributor column is the primary one — it is what an order can be placed against.
		TEST_COMPARE(mapping.mpn, 4);
		TEST_COMPARE(mapping.mpnAlt, 6);
		TEST_ASSERT_M(mapping.name != 5,
			"'Manufacturer_Name' must not be taken for the value column");
		// The underscores are the trap: 'Manufacturer_Part_Number' contains neither
		// "part number" nor "partnumber", so a literal substring match misses it entirely.
		TEST_COMPARE(PartManager::guessMapping({ "Manufacturer-Part-No" }).mpn, 0);

		// The distributor column wins the primary slot even when it comes second in the file,
		// and the one it displaces stays reachable rather than being dropped.
		const PartManager::BomColumnMapping reversed =
			PartManager::guessMapping({ "Reference", "Manufacturer_Part_Number", "Mouser Part Number" });
		TEST_COMPARE(reversed.mpn, 2);
		TEST_COMPARE(reversed.mpnAlt, 1);

		// One part-number column is still one, not one plus a duplicate of itself.
		const PartManager::BomColumnMapping single = PartManager::guessMapping({ "Reference", "MPN" });
		TEST_COMPARE(single.mpn, 1);
		TEST_COMPARE(single.mpnAlt, PartManager::NoCsvColumn);
	}

	TEST_FUNCTION(distributorNumbersMatchThroughSellerLinks)
	{
		TEST_START;

		// Two rows in the shape KiCad writes: the first names only a Mouser number, the second
		// only a manufacturer one. Both identify a part that is in stock, through different
		// columns — which is exactly why both are mapped.
		const std::string text =
			"Reference;Qty;Value;DNP;Mouser Part Number;Manufacturer_Name;Manufacturer_Part_Number\n"
			"R1,R2;2;4k7;;595-RC0603;Yageo;\n"
			"U1;1;LM317;;;Texas Instruments;lm317t\n"
			"D1;1;whatever;;595-NOPE;Nobody;NOT-A-PART\n";
		PartManager::CsvTable table;
		TEST_ASSERT(PartManager::parseCsv(text, PartManager::AutoDetectDelimiter, table));

		// A Mouser part number lives in part_seller_link, never in part.mpn. Without the links
		// the first row imports unresolved even though the part is sitting in the inventory.
		PartManager::PartSellerLink link;
		link.partId = 11;
		link.sellerPartNumber = "595-RC0603";

		const std::vector<PartManager::BomRow> rows = PartManager::buildRows(table,
			PartManager::guessMapping(table.headers), inventory(), { link });

		TEST_COMPARE(rows.size(), static_cast<size_t>(3));
		TEST_COMPARE(rows[0].matchedPartId, 11);
		// The alternative column carried this one, and the case does not have to agree.
		TEST_COMPARE(rows[1].matchedPartId, 22);
		TEST_COMPARE(rows[2].matchedPartId, PartManager::NoPartId);

		// Both cells are kept, so the dialog can show one and search Mouser for the other.
		TEST_COMPARE(rows[1].mpn, std::string());
		TEST_COMPARE(rows[1].mpnAlt, std::string("lm317t"));
		TEST_COMPARE(PartManager::bomRowIdentity(rows[0]), std::string("595-RC0603"));
		TEST_COMPARE(PartManager::bomRowIdentity(rows[1]), std::string("lm317t"));

		// Without the links the same file leaves the distributor-numbered row unresolved — the
		// half of the answer that regressing would be silent.
		const std::vector<PartManager::BomRow> unlinked = PartManager::buildRows(table,
			PartManager::guessMapping(table.headers), inventory());
		TEST_COMPARE(unlinked[0].matchedPartId, PartManager::NoPartId);
		TEST_COMPARE(unlinked[1].matchedPartId, 22);
	}
};

TEST_INSTANTIATE(TST_BomCsvImport);
