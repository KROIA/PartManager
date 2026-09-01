#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "domain/PartManager_StockTransaction.h"
#include "ui/PartManager_StockDialog.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

// The GUI driver against a screen the user actually sees, rather than against widgets written for
// the test. Everything is addressed by the objectName Designer gave it, so this suite keeps passing
// if the dialog is re-laid-out, and fails loudly if a widget is renamed or removed.
class TST_StockDialogGui : public UnitTest::Test
{
	TEST_CLASS(TST_StockDialogGui)
public:
	TST_StockDialogGui()
		: Test("TST_StockDialogGui")
	{
		ADD_TEST(TST_StockDialogGui::takeOutCollectsQuantityNoteAndReason);
		ADD_TEST(TST_StockDialogGui::overdrawIsPreviewedAsNegativeRatherThanBlocked);
		ADD_TEST(TST_StockDialogGui::restockHidesTheReasonItDoesNotHave);
	}

private:

	TEST_FUNCTION(takeOutCollectsQuantityNoteAndReason)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		// 100 on the shelf, the same starting point the real Take Out button hands over.
		PartManager::StockDialog dialog("150120YS75000", 100, PartManager::StockDialog::Mode::TakeOut);
		dialog.setObjectName("stockDialogUnderTest");
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QSpinBox* quantity = UnitTest::Gui::find<QSpinBox>("quantitySpin", &dialog);
		QLineEdit* note = UnitTest::Gui::find<QLineEdit>("noteEdit", &dialog);
		QComboBox* reason = UnitTest::Gui::find<QComboBox>("reasonCombo", &dialog);
		TEST_ASSERT_M(quantity && note && reason,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		TEST_ASSERT(UnitTest::Gui::setSpinValue(quantity, 12));
		TEST_ASSERT(UnitTest::Gui::type(note, "amplifier build"));
		// A broken part is a loss, not a checkout — the picker exists so the history stops lying.
		TEST_ASSERT(UnitTest::Gui::selectComboText(reason, "Lost or broken"));

		TEST_COMPARE(dialog.quantity(), 12);
		TEST_COMPARE(dialog.note(), QString("amplifier build"));
		TEST_COMPARE(dialog.reason(), std::string(PartManager::StockReason::Loss));

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(overdrawIsPreviewedAsNegativeRatherThanBlocked)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::StockDialog dialog("1N914BWS", 5, PartManager::StockDialog::Mode::TakeOut);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QSpinBox* quantity = UnitTest::Gui::find<QSpinBox>("quantitySpin", &dialog);
		QLabel* result = UnitTest::Gui::find<QLabel>("resultLabel", &dialog);
		TEST_ASSERT_M(quantity && result, "quantitySpin / resultLabel are gone");

		TEST_ASSERT(UnitTest::Gui::setSpinValue(quantity, 3));
		TEST_ASSERT_M(result->text().contains("2"), "the preview did not restate the remaining stock");
		TEST_ASSERT_M(result->styleSheet().isEmpty(), "a positive result must not be painted as a warning");

		// §3 records the over-draw instead of refusing it, so the dialog warns and still accepts.
		TEST_ASSERT(UnitTest::Gui::setSpinValue(quantity, 8));
		TEST_ASSERT_M(result->text().contains("-3"), "an over-draw must preview the negative result");
		TEST_ASSERT_M(!result->styleSheet().isEmpty(), "a negative result must be visibly flagged");
		TEST_COMPARE(dialog.quantity(), 8);

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(restockHidesTheReasonItDoesNotHave)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::StockDialog dialog("MBRS330T3G", 100, PartManager::StockDialog::Mode::Restock);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QComboBox* reason = UnitTest::Gui::find<QComboBox>("reasonCombo", &dialog);
		TEST_ASSERT_M(reason != nullptr, "reasonCombo is gone");
		TEST_ASSERT_M(!reason->isVisible(), "restocking has one reason, so the picker must stay hidden");
		// Hidden widgets refuse input, which is exactly what a user would experience.
		TEST_ASSERT_M(!UnitTest::Gui::selectComboText(reason, "Lost or broken"),
			"a hidden picker accepted a selection");
		TEST_COMPARE(dialog.reason(), std::string(PartManager::StockReason::Restock));

		QSpinBox* quantity = UnitTest::Gui::find<QSpinBox>("quantitySpin", &dialog);
		QLabel* result = UnitTest::Gui::find<QLabel>("resultLabel", &dialog);
		TEST_ASSERT(UnitTest::Gui::setSpinValue(quantity, 5));
		TEST_ASSERT_M(result->text().contains("105"), "restocking 5 onto 100 should preview 105");

		UnitTest::Gui::closeWindow(&dialog);
	}
};

TEST_INSTANTIATE(TST_StockDialogGui);
