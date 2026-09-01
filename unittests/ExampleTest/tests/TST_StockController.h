#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_StockController.h"
#include "controllers/PartManager_PartEditorController.h"
#include "persistence/PartManager_PartRepository.h"
#include <filesystem>

// What the ribbon's Restock/Take Out buttons and the part editor's quantity field call, minus the
// widgets: the running total a history table renders, and the transactions the three write paths
// leave behind on a throwaway database in %TEMP% — never the user's own.
//
// The point of the database cases is that no quantity change is a silent write to the cached
// `part.stock_qty`: each one has to show up as a `stock_transaction` row (§3).
class TST_StockController : public UnitTest::Test
{
	TEST_CLASS(TST_StockController)
public:
	TST_StockController()
		: Test("TST_StockController")
	{
		ADD_TEST(TST_StockController::runningQuantitiesFollowTheLog);
		ADD_TEST(TST_StockController::reasonLabelsCoverTheVocabulary);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_StockController::restockThenTakeOutIsLogged);
		ADD_TEST(TST_StockController::editorQuantityBecomesAManualAdjustment);
		ADD_TEST(TST_StockController::overDrawGoesNegativeRatherThanFailing);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// A fresh database plus one part in it, so each case starts from an empty log.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		int& outPartId, std::string& outError)
	{
		std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_StockController_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);

		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Stock", outError);
		if (!handle)
		{
			return handle;
		}

		PartManager::PartEditorController editor(handle.get());
		PartManager::Part part;
		part.partTypeId = editor.types().front().id;
		part.name = "LM358";
		outPartId = editor.createPart(part);
		return handle;
	}

	// The cached column, read straight from the table the rest of the app displays.
	static int cachedQuantity(PartManager::DatabaseHandle& handle, int partId)
	{
		PartManager::Part part;
		PartManager::PartRepository::findPart(handle.connection(), partId, part);
		return part.stockQty;
	}
#endif

	// Tests
	TEST_FUNCTION(runningQuantitiesFollowTheLog)
	{
		TEST_START;

		auto transaction = [](int delta)
		{
			PartManager::StockTransaction row;
			row.deltaQty = delta;
			return row;
		};

		// Opening balance, a restock, two take-outs — the last one draws past empty.
		const std::vector<PartManager::StockTransaction> history{
			transaction(10), transaction(5), transaction(-12), transaction(-6) };
		const std::vector<int> running = PartManager::runningQuantities(history);

		TEST_COMPARE(running.size(), static_cast<size_t>(4));
		TEST_COMPARE(running[0], 10);
		TEST_COMPARE(running[1], 15);
		TEST_COMPARE(running[2], 3);
		TEST_ASSERT_M(running[3] == -3, "the running total must be allowed to go negative");

		// An empty log has no rows at all rather than a single zero.
		TEST_ASSERT(PartManager::runningQuantities({}).empty());
	}

	TEST_FUNCTION(reasonLabelsCoverTheVocabulary)
	{
		TEST_START;

		TEST_ASSERT(!PartManager::stockReasonLabel(PartManager::StockReason::Restock).isEmpty());
		TEST_ASSERT(!PartManager::stockReasonLabel(PartManager::StockReason::CheckoutPartlist).isEmpty());
		TEST_ASSERT(!PartManager::stockReasonLabel(PartManager::StockReason::ManualAdjust).isEmpty());
		TEST_ASSERT(!PartManager::stockReasonLabel(PartManager::StockReason::Initial).isEmpty());
		// `reason` is free-form TEXT: a string this build does not know is shown, not dropped.
		TEST_COMPARE(PartManager::stockReasonLabel("something_new").toStdString(),
			std::string("something_new"));
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// The ribbon's two Stock buttons: the quantity has to land on the right number AND leave one
	// transaction each, since the log is what the history view and §3's reports read.
	TEST_FUNCTION(restockThenTakeOutIsLogged)
	{
		TEST_START;

		int partId = 0;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("restock", partId, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT_M(partId != 0, "the part under test could not be created");

		PartManager::StockController controller(handle.get());
		TEST_COMPARE(controller.quantity(partId), 0);

		TEST_ASSERT(controller.restock(partId, 100, "order 4711 arrived"));
		TEST_ASSERT(controller.takeOut(partId, 30, "amplifier build"));
		TEST_COMPARE(controller.quantity(partId), 70);
		// The cache the Home table reads must agree with the log without anyone refreshing it.
		TEST_COMPARE(cachedQuantity(*handle, partId), 70);

		std::vector<PartManager::StockTransaction> history = controller.history(partId);
		TEST_COMPARE(history.size(), static_cast<size_t>(2));
		TEST_COMPARE(history[0].deltaQty, 100);
		TEST_COMPARE(history[0].reason, std::string(PartManager::StockReason::Restock));
		TEST_COMPARE(history[0].note, std::string("order 4711 arrived"));
		TEST_COMPARE(history[1].deltaQty, -30);
		TEST_COMPARE(history[1].note, std::string("amplifier build"));
		// No reason given: a take-out still defaults to the partlist checkout.
		TEST_COMPARE(history[1].reason, std::string(PartManager::StockReason::CheckoutPartlist));

		// A broken part is a loss, not a checkout - the reason the dialog picks has to reach the log,
		// otherwise the history says the parts went into a build they never went into.
		TEST_ASSERT(controller.takeOut(partId, 2, "dropped on the floor", PartManager::StockReason::Loss));
		history = controller.history(partId);
		TEST_COMPARE(history.back().reason, std::string(PartManager::StockReason::Loss));
		TEST_COMPARE(history.back().deltaQty, -2);
		TEST_COMPARE(controller.quantity(partId), 68);

		// Oldest first, so the running total the editor's table shows is in shelf order.
		std::vector<int> running = PartManager::runningQuantities(history);
		TEST_COMPARE(running.back(), controller.quantity(partId));

		// A zero or negative quantity is not an event; it must not reach the log either way.
		TEST_ASSERT(!controller.restock(partId, 0));
		TEST_ASSERT(!controller.takeOut(partId, -5));
		TEST_COMPARE(controller.history(partId).size(), static_cast<size_t>(3));
	}

	// The part editor's quantity field: an absolute target, logged as a correction rather than
	// written onto part.stock_qty behind the log's back.
	TEST_FUNCTION(editorQuantityBecomesAManualAdjustment)
	{
		TEST_START;

		int partId = 0;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("editor", partId, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::StockController controller(handle.get());
		TEST_ASSERT(controller.restock(partId, 40, ""));

		// "I counted the shelf, there are 37" — one transaction of -3, not a silent write.
		TEST_ASSERT(controller.setQuantity(partId, 37, "recount"));
		TEST_COMPARE(controller.quantity(partId), 37);
		TEST_COMPARE(cachedQuantity(*handle, partId), 37);

		std::vector<PartManager::StockTransaction> history = controller.history(partId);
		TEST_COMPARE(history.size(), static_cast<size_t>(2));
		TEST_COMPARE(history.back().deltaQty, -3);
		TEST_COMPARE(history.back().reason, std::string(PartManager::StockReason::ManualAdjust));

		// Committing the same number again (the field commits on every focus loss) must not
		// churn the log with zero-delta rows, and must still report success.
		TEST_ASSERT(controller.setQuantity(partId, 37, "recount"));
		TEST_COMPARE(controller.history(partId).size(), static_cast<size_t>(2));

		// The old behaviour this replaces: PartRepository still writes stock_qty, so a save of the
		// part record must not be able to move the number away from the log.
		PartManager::PartEditorController editor(handle.get());
		PartManager::Part part;
		TEST_ASSERT(editor.loadPart(partId, part));
		TEST_COMPARE(part.stockQty, 37);
	}

	// §3 is explicit that taking out more than the shelf holds is recorded, not refused.
	TEST_FUNCTION(overDrawGoesNegativeRatherThanFailing)
	{
		TEST_START;

		int partId = 0;
		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("overdraw", partId, error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::StockController controller(handle.get());
		TEST_ASSERT(controller.restock(partId, 5, ""));
		TEST_ASSERT_M(controller.takeOut(partId, 8, "taken without logging, apparently"),
			"an over-draw must be recorded, not rejected");
		TEST_COMPARE(controller.quantity(partId), -3);
		TEST_COMPARE(cachedQuantity(*handle, partId), -3);

		// And the way back is a correction, which is what the editor's field writes.
		TEST_ASSERT(controller.setQuantity(partId, 0, "recount"));
		TEST_COMPARE(controller.quantity(partId), 0);
		TEST_COMPARE(controller.history(partId).size(), static_cast<size_t>(3));
	}
#endif

};

TEST_INSTANTIATE(TST_StockController);
