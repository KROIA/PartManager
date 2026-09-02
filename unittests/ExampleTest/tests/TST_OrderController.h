#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_OrderController.h"

// The widget-free half of the order view (§12b). `planStaging()` is the piece worth pinning down:
// it decides what actually reaches the Cart API, and every way it can be wrong ends with either a
// cart that is quietly short or one that orders parts twice.
class TST_OrderController : public UnitTest::Test
{
	TEST_CLASS(TST_OrderController)
public:
	TST_OrderController()
		: Test("TST_OrderController")
	{
		ADD_TEST(TST_OrderController::stagingSkipsWhatCannotOrArrivedAlready);
		ADD_TEST(TST_OrderController::stagingAsksOnlyForTheOutstandingRemainder);
		ADD_TEST(TST_OrderController::statusLabelsSurviveAnUnknownValue);
	}

private:

	// One order line, spelled out so each test reads as the situation it describes.
	static PartManager::OrderLine makeLine(const std::string& name, const std::string& mouserNumber,
		int ordered, int received, const char* status = PartManager::OrderItemStatus::Pending)
	{
		PartManager::OrderLine line;
		line.partName = name;
		line.mouserPartNumber = mouserNumber;
		line.item.quantityOrdered = ordered;
		line.item.quantityReceived = received;
		line.item.status = status;
		// The same rule OrderRepository::lines() applies, so the fixture cannot drift from it.
		line.stageable = !mouserNumber.empty() && ordered > 0;
		return line;
	}

	TEST_FUNCTION(stagingSkipsWhatCannotOrArrivedAlready)
	{
		TEST_START;

		std::vector<PartManager::OrderLine> lines;
		lines.push_back(makeLine("RC0603-4K7", "603-RC0603", 40, 0));
		// Never linked to Mouser: sending part.mpn instead would build a cart Mouser rejects.
		lines.push_back(makeLine("HOMEBREW-1", "", 3, 0));
		// Already in. Re-staging it is how you end up with twice the parts and no idea why.
		lines.push_back(makeLine("CAP-0805", "603-CAP", 10, 10,
			PartManager::OrderItemStatus::Arrived));

		const PartManager::StagingPlan plan = PartManager::planStaging(lines);
		TEST_COMPARE(plan.items.size(), static_cast<size_t>(1));
		TEST_COMPARE(plan.items[0].mouserPartNumber, std::string("603-RC0603"));
		TEST_COMPARE(plan.items[0].quantity, 40);

		// The unorderable one is *named*, not silently dropped — a cart that came back short with
		// no explanation is the failure this whole struct exists to prevent.
		TEST_COMPARE(plan.skippedPartNames.size(), static_cast<size_t>(1));
		TEST_COMPARE(plan.skippedPartNames[0], std::string("HOMEBREW-1"));

		// The summary has to say so too, or the count only lives in a dialog nobody re-reads.
		const QString summary = PartManager::orderStatusSummary(lines);
		TEST_ASSERT_M(summary.contains(QStringLiteral("Mouser")),
			"the footer must mention the unstageable line: " + summary.toStdString());
	}

	TEST_FUNCTION(stagingAsksOnlyForTheOutstandingRemainder)
	{
		TEST_START;

		// A partial shipment: 40 ordered, 15 turned up. Re-staging must ask for the missing 25,
		// not the original 40 — the other 15 are already on the shelf.
		std::vector<PartManager::OrderLine> lines;
		lines.push_back(makeLine("RC0603-4K7", "603-RC0603", 40, 15));

		const PartManager::StagingPlan plan = PartManager::planStaging(lines);
		TEST_COMPARE(plan.items.size(), static_cast<size_t>(1));
		TEST_COMPARE(plan.items[0].quantity, 25);

		// Over-delivered lines (Mouser ships full reels) leave nothing outstanding, so they must
		// not appear as a zero- or negative-quantity request.
		std::vector<PartManager::OrderLine> overDelivered;
		overDelivered.push_back(makeLine("RC0603-4K7", "603-RC0603", 40, 50));
		TEST_COMPARE(PartManager::planStaging(overDelivered).items.size(), static_cast<size_t>(0));
	}

	TEST_FUNCTION(statusLabelsSurviveAnUnknownValue)
	{
		TEST_START;

		TEST_COMPARE(PartManager::orderStatusLabel(PartManager::OrderStatus::Draft),
			QStringLiteral("Draft"));
		TEST_COMPARE(PartManager::orderItemStatusLabel(PartManager::OrderItemStatus::Backordered),
			QStringLiteral("Backordered"));
		// Both columns are free-form TEXT, so a row written by a future version has to render as
		// itself rather than vanish into an empty cell.
		TEST_COMPARE(PartManager::orderStatusLabel("awaiting_customs"),
			QStringLiteral("awaiting_customs"));
		TEST_COMPARE(PartManager::orderItemStatusLabel("substituted"),
			QStringLiteral("substituted"));
	}
};

TEST_INSTANTIATE(TST_OrderController);
