#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "controllers/PartManager_PartEditorController.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartMigrationDialog.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <filesystem>

// The §4/§5 pasted-list migration, at the seam between the parsing in core/import and the two
// screens on top of it: PartListMigration's own rules are TST_PartListMigration's job, so what is
// checked here is that a paste really lands on the mapping combos, that the guess survives the
// trip through the widgets, and that Next turns the preview into a worklist whose rows carry the
// status core resolved them to.
//
// Nothing here touches Mouser: creating a part needs a key and a network, and the migration's own
// bookkeeping is testable without ever leaving the paste.
//
// Runs against a throwaway database in %TEMP%, never the user's own.
class TST_PartMigrationDialogGui : public UnitTest::Test
{
	TEST_CLASS(TST_PartMigrationDialogGui)
public:
	TST_PartMigrationDialogGui()
		: Test("TST_PartMigrationDialogGui")
	{
		ADD_TEST(TST_PartMigrationDialogGui::aPastedListMapsItselfAndBecomesAWorklist);
		ADD_TEST(TST_PartMigrationDialogGui::aManualAddDoesNotClaimToComeFromMouser);
		ADD_TEST(TST_PartMigrationDialogGui::aMouserOnlyListIsAWorkableList);
	}

private:

	// A fresh seeded database for one test. Null on failure, with the reason in outError.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_PartMigrationDialogGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "Migration", outError);
	}

	TEST_FUNCTION(aPastedListMapsItselfAndBecomesAWorklist)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("worklist", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// One of the two pasted numbers is already in the drawer, which is the case the worklist
		// has to tell apart from the one it offers to create.
		PartManager::PartEditorController editor(handle.get());
		const std::vector<PartManager::PartType> types = editor.types();
		TEST_ASSERT_M(!types.empty(), "the seeded database declares no part types");
		PartManager::Part existingPart;
		existingPart.partTypeId = types.front().id;
		existingPart.name = "Already owned resistor";
		existingPart.mpn = "RC0603FR-074K7L";
		existingPart.id = editor.createPart(existingPart);
		TEST_ASSERT_M(existingPart.id != 0, "createPart failed");

		PartManager::PartMigrationDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QPlainTextEdit* paste = UnitTest::Gui::find<QPlainTextEdit>("pasteEdit", &dialog);
		QComboBox* mpnCombo = UnitTest::Gui::find<QComboBox>("mpnCombo", &dialog);
		QComboBox* stockCombo = UnitTest::Gui::find<QComboBox>("stockCombo", &dialog);
		QComboBox* manufacturerCombo = UnitTest::Gui::find<QComboBox>("manufacturerCombo", &dialog);
		QTableWidget* preview = UnitTest::Gui::find<QTableWidget>("previewTable", &dialog);
		QPushButton* next = UnitTest::Gui::find<QPushButton>("nextButton", &dialog);
		QStackedWidget* stack = UnitTest::Gui::find<QStackedWidget>("stack", &dialog);
		QTableWidget* worklist = UnitTest::Gui::find<QTableWidget>("worklistTable", &dialog);
		QLabel* progress = UnitTest::Gui::find<QLabel>("progressLabel", &dialog);
		TEST_ASSERT_M(paste && mpnCombo && stockCombo && manufacturerCombo && preview && next
			&& stack && worklist && progress,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		// Header plus two rows, tab separated — a column block copied out of a spreadsheet, which
		// is the shape the feature exists for.
		paste->setPlainText(QStringLiteral(
			"Part Number\tQty\tManufacturer\n"
			"RC0603FR-074K7L\t25\tYAGEO\n"
			"GRM188R71C104KA01D\t100\tMurata\n"));

		// The guess has to survive being written into the combos: the combos carry the column
		// index as user data, and reading it back is what the mapping is actually built from.
		TEST_COMPARE(mpnCombo->currentData().toInt(), 0);
		TEST_COMPARE(stockCombo->currentData().toInt(), 1);
		TEST_COMPARE(manufacturerCombo->currentData().toInt(), 2);
		TEST_COMPARE(preview->rowCount(), 2);
		TEST_ASSERT_M(next->isEnabled(), "Next must be usable once a part-number column is mapped");

		next->click();
		TEST_COMPARE(stack->currentIndex(), 1);
		TEST_COMPARE(worklist->rowCount(), 2);

		// Column 1 is the part number, column 0 the translated status.
		TEST_ASSERT_M(worklist->item(0, 1) && worklist->item(1, 1), "the worklist has no cells");
		TEST_COMPARE(worklist->item(0, 1)->text(), QString("RC0603FR-074K7L"));
		TEST_COMPARE(worklist->item(1, 1)->text(), QString("GRM188R71C104KA01D"));
		TEST_COMPARE(worklist->item(0, 0)->text(),
			PartManager::PartMigrationDialog::tr("Already there"));
		TEST_COMPARE(worklist->item(1, 0)->text(),
			PartManager::PartMigrationDialog::tr("To do"));
		// The stock column is the pasted one, not the database's — it is what a create or a
		// booking would use. Column 2 is the Mouser number, which this paste does not carry.
		TEST_COMPARE(worklist->item(0, 2)->text(), QString());
		TEST_COMPARE(worklist->item(0, 3)->text(), QString("25"));

		// The already-owned row needs nothing from the user, so it is handled the moment the
		// worklist appears; the other one is the work.
		TEST_ASSERT_M(progress->text().contains(QStringLiteral("1")),
			"the progress line must count the already-present row as handled: "
			+ progress->text().toStdString());

		// Skip is the one row action that needs neither a network nor a second dialog, so it is
		// also the one that can be driven here — and leaving nothing Pending is what lets the
		// dialog close without its §10 "not yet handled" question, which no test can answer.
		QPushButton* skip = UnitTest::Gui::find<QPushButton>("skipButton", &dialog);
		TEST_ASSERT_M(skip != nullptr, "skipButton is gone");
		TEST_ASSERT_M(skip->isEnabled(),
			"the worklist must open with the first Pending row selected");
		skip->click();
		TEST_COMPARE(worklist->item(1, 0)->text(),
			PartManager::PartMigrationDialog::tr("Skipped"));
		TEST_COMPARE(dialog.createdCount(), 0);

		UnitTest::Gui::closeWindow(&dialog);
	}

	// "Add Part…" falling back to the pasted row — no key, no hit, or no network — opens the same
	// New Part form a Mouser hit does, but nothing on it came from a vendor, so the banner above it
	// must not say it did. Driving the button itself would mean a modal dialog no test can answer
	// (and, with a key set, a network call), so what is checked here is the seam the button uses:
	// the prefill the migration builds by hand, with PrefillSource::ImportedList.
	TEST_FUNCTION(aManualAddDoesNotClaimToComeFromMouser)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("manualadd", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// Exactly what PartMigrationDialog::addSelectedManually() builds for a pasted row.
		PartManager::MouserPartPrefill prefill;
		prefill.part.mpn = "GRM188R71C104KA01D";
		prefill.part.manufacturer = "Murata";
		prefill.part.description = "0.1uF 16V X7R 0603";
		prefill.part.name = prefill.part.mpn;
		// The row knew a Mouser number and the lookup did not answer: the number is still the one
		// fact about the row that is certainly true, so it has to reach the form anyway. Leaving
		// this field empty for exactly the lists that carry Mouser numbers was the reported bug.
		prefill.mouserPartNumber = "81-GRM188R71C104KA1D";

		PartManager::NewPartDialog dialog(handle.get());
		dialog.setPrefill(prefill, PartManager::NewPartDialog::PrefillSource::ImportedList);
		dialog.setListDefaults(100, QString("left drawer"));
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QLabel* header = UnitTest::Gui::find<QLabel>("headerLabel", &dialog);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &dialog);
		QPlainTextEdit* description = UnitTest::Gui::find<QPlainTextEdit>("descriptionEdit", &dialog);
		QSpinBox* stock = UnitTest::Gui::find<QSpinBox>("stockSpin", &dialog);
		QLineEdit* mouser = UnitTest::Gui::find<QLineEdit>("mouserEdit", &dialog);
		TEST_ASSERT_M(header && name && description && stock && mouser,
			"the New Part dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		TEST_ASSERT_M(!header->text().contains(QStringLiteral("Mouser")),
			"a manual add must not claim a vendor it never talked to: " + header->text().toStdString());
		TEST_ASSERT_M(header->text().contains(
			PartManager::NewPartDialog::tr("Filled in from the imported list — nothing here was "
				"checked against a catalogue, so correct anything that is wrong before creating "
				"the part.")),
			"the imported-list wording is missing: " + header->text().toStdString());

		// §11's name gate is already satisfied, so the user is not looking at a disabled Create on
		// a form they have nothing to fix on.
		TEST_COMPARE(name->text(), QString("GRM188R71C104KA01D"));
		TEST_COMPARE(mouser->text(), QString("81-GRM188R71C104KA1D"));
		// setListDefaults: the shelf count goes to the spin box (so createPart() writes the §3
		// opening balance), the note onto the end of the description.
		TEST_COMPARE(stock->value(), 100);
		TEST_ASSERT_M(description->toPlainText().contains(QStringLiteral("left drawer")),
			"the pasted note was dropped instead of being kept on the description: "
			+ description->toPlainText().toStdString());

		UnitTest::Gui::closeWindow(&dialog);
	}

	// The list the Mouser column exists for: order codes and nothing else. It has to map itself to
	// the *Mouser* combo rather than to the part-number one, Next has to accept it even though no
	// manufacturer part number was mapped at all, and the worklist has to show the number — which
	// is also what Add Part would hand to the lookup. The lookup itself needs a key and a network,
	// so it is not driven here.
	TEST_FUNCTION(aMouserOnlyListIsAWorkableList)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("mouseronly", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartMigrationDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QPlainTextEdit* paste = UnitTest::Gui::find<QPlainTextEdit>("pasteEdit", &dialog);
		QComboBox* mpnCombo = UnitTest::Gui::find<QComboBox>("mpnCombo", &dialog);
		QComboBox* mouserCombo = UnitTest::Gui::find<QComboBox>("mouserCombo", &dialog);
		QPushButton* next = UnitTest::Gui::find<QPushButton>("nextButton", &dialog);
		QTableWidget* worklist = UnitTest::Gui::find<QTableWidget>("worklistTable", &dialog);
		TEST_ASSERT_M(paste && mpnCombo && mouserCombo && next && worklist,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		paste->setPlainText(QStringLiteral(
			"Mouser Part Number\tQty\n"
			"595-LM358DR\t10\n"));

		TEST_COMPARE(mouserCombo->currentData().toInt(), 0);
		TEST_ASSERT_M(mpnCombo->currentData().toInt() == PartManager::NoMigrationColumn,
			"a Mouser header must not be taken for the manufacturer part number");
		TEST_ASSERT_M(next->isEnabled(),
			"a list of nothing but Mouser numbers is a complete list and must be usable");

		next->click();
		TEST_COMPARE(worklist->rowCount(), 1);
		TEST_ASSERT_M(worklist->item(0, 2) != nullptr, "the worklist has no Mouser column");
		TEST_COMPARE(worklist->item(0, 1)->text(), QString());
		TEST_COMPARE(worklist->item(0, 2)->text(), QString("595-LM358DR"));

		// Nothing left Pending, so closing asks no question this test could not answer.
		QPushButton* skip = UnitTest::Gui::find<QPushButton>("skipButton", &dialog);
		TEST_ASSERT_M(skip && skip->isEnabled(), "the Mouser-only row is not selectable work");
		skip->click();

		UnitTest::Gui::closeWindow(&dialog);
	}
};

TEST_INSTANTIATE(TST_PartMigrationDialogGui);
