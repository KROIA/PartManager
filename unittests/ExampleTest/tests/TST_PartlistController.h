#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_PartlistController.h"

// The partlist screens' widget-free logic: the source vocabulary, the footer summary and the
// picker label. Everything here is a pure function of a PartlistLine vector, so no database and
// no widgets are involved — the repository half is TST_PartlistRepository's job.
class TST_PartlistController : public UnitTest::Test
{
	TEST_CLASS(TST_PartlistController)
public:
	TST_PartlistController()
		: Test("TST_PartlistController")
	{
		ADD_TEST(TST_PartlistController::sourceLabelsFallBackToTheRawValue);
		ADD_TEST(TST_PartlistController::summarySpeaksOnlyWhenSomethingIsWrong);
		ADD_TEST(TST_PartlistController::pickerLabelDropsAnEmptyMpn);
		ADD_TEST(TST_PartlistController::onePartIsOneLineWithEveryDesignatorKept);
	}

private:

	static PartManager::PartlistLine resolvedLine(int needed, int stock)
	{
		PartManager::PartlistLine line;
		line.resolved = true;
		line.item.partId = 1;
		line.neededQty = needed;
		line.stockQty = stock;
		line.shortfallQty = needed > stock ? needed - stock : 0;
		return line;
	}

	static PartManager::PartlistLine unresolvedLine()
	{
		PartManager::PartlistLine line;   // resolved stays false, partId stays NoPartId
		line.neededQty = 5;
		return line;
	}

	TEST_FUNCTION(sourceLabelsFallBackToTheRawValue)
	{
		TEST_START;

		TEST_COMPARE(PartManager::partlistSourceLabel(PartManager::PartlistSource::Manual),
			QString("Manual"));
		TEST_COMPARE(PartManager::partlistSourceLabel(PartManager::PartlistSource::KicadImport),
			QString("KiCad import"));
		TEST_COMPARE(PartManager::partlistSourceLabel(PartManager::PartlistSource::CsvImport),
			QString("CSV import"));
		// `source` is free-form TEXT: a value written by a future version has to render as
		// itself rather than vanish into an empty cell.
		TEST_COMPARE(PartManager::partlistSourceLabel("altium_import"), QString("altium_import"));
	}

	TEST_FUNCTION(summarySpeaksOnlyWhenSomethingIsWrong)
	{
		TEST_START;

		// Everything resolved and in stock: nothing to say. Printing "all fine" on every list
		// is noise the user learns to stop reading.
		std::vector<PartManager::PartlistLine> healthy = { resolvedLine(10, 45), resolvedLine(3, 3) };
		TEST_ASSERT_M(PartManager::partlistStatusSummary(healthy).isEmpty(),
			"a healthy list must produce no status line");
		TEST_COMPARE(PartManager::unresolvedCount(healthy), 0);

		std::vector<PartManager::PartlistLine> shortOnly = { resolvedLine(15, 12), resolvedLine(10, 45) };
		const QString shortText = PartManager::partlistStatusSummary(shortOnly);
		TEST_ASSERT_M(shortText.contains("short"), "a shortfall must be reported: " + shortText.toStdString());
		TEST_ASSERT_M(!shortText.contains("matched"),
			"nothing is unmatched here, so the summary must not say so: " + shortText.toStdString());

		std::vector<PartManager::PartlistLine> both = { resolvedLine(15, 12), unresolvedLine(), unresolvedLine() };
		TEST_COMPARE(PartManager::unresolvedCount(both), 2);
		const QString bothText = PartManager::partlistStatusSummary(both);
		TEST_ASSERT_M(bothText.contains("2"), "both unmatched lines must be counted: " + bothText.toStdString());
		TEST_ASSERT_M(bothText.contains("short"), "the short line must still be reported: " + bothText.toStdString());

		// An unresolved line has no stock to be short of, so it must not also count as short.
		std::vector<PartManager::PartlistLine> unresolvedOnly = { unresolvedLine() };
		const QString unresolvedText = PartManager::partlistStatusSummary(unresolvedOnly);
		TEST_ASSERT_M(!unresolvedText.contains("short"),
			"an unresolved line must not be reported as short: " + unresolvedText.toStdString());
	}

	TEST_FUNCTION(pickerLabelDropsAnEmptyMpn)
	{
		TEST_START;

		PartManager::Part part;
		part.name = "RC0603FR-074K7L";
		// No MPN yet — every part imported in item 7a is in exactly this state, and "name ()"
		// would be the label for all ten of them.
		TEST_COMPARE(PartManager::partPickerLabel(part), QString("RC0603FR-074K7L"));

		part.mpn = "RC0603FR-074K7L";
		TEST_COMPARE(PartManager::partPickerLabel(part),
			QString("RC0603FR-074K7L (RC0603FR-074K7L)"));
	}

	TEST_FUNCTION(onePartIsOneLineWithEveryDesignatorKept)
	{
		TEST_START;

		// Both separators, stray spaces, and a designator that is on both lines: R4 must survive
		// exactly once, and nothing may be dropped — a lost BMK is a missing placement on a board.
		TEST_COMPARE(PartManager::mergeDesignators("R1, R4", "R4;R7 ; R9"),
			std::string("R1, R4, R7, R9"));
		TEST_COMPARE(PartManager::mergeDesignators("", "C1"), std::string("C1"));
		TEST_COMPARE(PartManager::mergeDesignators("C1", ""), std::string("C1"));

		std::vector<PartManager::PartlistItem> items(4);
		items[0].partId = 7;   items[0].quantityPerUnit = 2;  items[0].designators = "R1,R2";
		items[1].partId = 0;   items[1].quantityPerUnit = 1;  items[1].designators = "?";
		items[2].partId = 7;   items[2].quantityPerUnit = 3;  items[2].designators = "R5";
		items[3].partId = 0;   items[3].quantityPerUnit = 1;  items[3].designators = "??";

		TEST_ASSERT_M(PartManager::mergeDuplicateItems(items), "the two part-7 rows are one row");
		TEST_COMPARE(items.size(), static_cast<size_t>(3));
		TEST_COMPARE(items[0].quantityPerUnit, 5);
		TEST_COMPARE(items[0].designators, std::string("R1, R2, R5"));
		// Unresolved rows have no part in common, so they are left as the separate to-dos they are.
		TEST_COMPARE(items[1].partId, 0);
		TEST_COMPARE(items[2].partId, 0);

		TEST_ASSERT_M(!PartManager::mergeDuplicateItems(items), "a merged list is left alone");
	}
};

TEST_INSTANTIATE(TST_PartlistController);
