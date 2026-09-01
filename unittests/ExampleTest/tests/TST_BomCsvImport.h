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
};

TEST_INSTANTIATE(TST_BomCsvImport);
