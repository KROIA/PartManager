#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "ui/PartManager_PartlistEditorDialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <filesystem>

// The §4 editor driven through its widgets. The arithmetic is TST_PartlistRepository's job; what
// is checked here is the wiring the repository cannot see — that a multiplier change repaints the
// computed columns, that a line added through the button reaches the database, and that the §10
// autosave really writes without a Save button.
//
// Runs against a throwaway database in %TEMP%, never the user's own.
class TST_PartlistEditorGui : public UnitTest::Test
{
	TEST_CLASS(TST_PartlistEditorGui)
public:
	TST_PartlistEditorGui()
		: Test("TST_PartlistEditorGui")
	{
		ADD_TEST(TST_PartlistEditorGui::multiplierRepaintsTheComputedColumns);
		ADD_TEST(TST_PartlistEditorGui::addedLinesAndHeaderEditsAreSavedWithoutASaveButton);
	}

private:

	// Columns of PartlistEditorDialog's grid, in the order it builds them.
	enum Column { Designators = 0, PartColumn, QtyPerUnit, QtyTotal, InStock, Shortfall };

	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_PartlistEditorGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "Partlists", outError);
	}

	// One part with a known quantity, plus a one-line partlist pointing at it.
	static int makeListWithOneLine(PartManager::DatabaseHandle& handle, int quantityPerUnit,
		int stock, int multiplier)
	{
		PartManager::PartlistController controller(&handle);

		// createNew() seeds the type templates, so there is always one to hang the part off.
		std::vector<PartManager::PartType> types =
			PartManager::PartTypeRepository::listTypes(handle.connection());
		PartManager::Part part;
		part.partTypeId = types.empty() ? 0 : types.front().id;
		part.name = "RC0603-4K7";
		part.mpn = "RC0603FR-074K7L";
		const int partId = PartManager::PartRepository::insertPart(handle.connection(), part);
		PartManager::StockRepository::restock(handle.connection(), partId, stock, "test fixture");

		PartManager::Partlist partlist;
		partlist.name = "PSU Rev C";
		partlist.multiplier = multiplier;
		const int listId = controller.create(partlist);

		PartManager::PartlistItem item;
		item.partId = partId;
		item.designators = "R1,R2,R5";
		item.quantityPerUnit = quantityPerUnit;
		controller.saveItems(listId, { item });
		return listId;
	}

	TEST_FUNCTION(multiplierRepaintsTheComputedColumns)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("multiplier", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// 3 per board, 12 on the shelf, one board — covered.
		const int listId = makeListWithOneLine(*handle, 3, 12, 1);

		PartManager::PartlistController controller(handle.get());
		PartManager::PartlistEditorDialog dialog(controller, listId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTableWidget* table = UnitTest::Gui::find<QTableWidget>("itemTable", &dialog);
		QSpinBox* multiplier = UnitTest::Gui::find<QSpinBox>("multiplierSpin", &dialog);
		QLabel* status = UnitTest::Gui::find<QLabel>("statusLabel", &dialog);
		TEST_ASSERT_M(table && multiplier && status,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		TEST_COMPARE(table->rowCount(), 1);
		TEST_COMPARE(table->item(0, Designators)->text(), QString("R1,R2,R5"));
		TEST_COMPARE(table->item(0, QtyTotal)->text(), QString("3"));
		TEST_COMPARE(table->item(0, InStock)->text(), QString("12"));
		TEST_COMPARE(table->item(0, Shortfall)->text(), QString("0"));
		TEST_ASSERT_M(status->text().isEmpty(), "a covered list must produce no status line");

		// Five boards need 15 and there are 12 — the same edit the user makes before ordering.
		TEST_ASSERT(UnitTest::Gui::setSpinValue(multiplier, 5));
		TEST_COMPARE(table->item(0, QtyTotal)->text(), QString("15"));
		TEST_COMPARE(table->item(0, Shortfall)->text(), QString("3"));
		TEST_ASSERT_M(status->text().contains("short"),
			"the footer must report the shortfall: " + status->text().toStdString());

		// The multiplier saves immediately rather than on the header debounce, because every
		// number on screen depends on it.
		PartManager::Partlist stored;
		TEST_ASSERT(controller.load(listId, stored));
		TEST_COMPARE(stored.multiplier, 5);

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(addedLinesAndHeaderEditsAreSavedWithoutASaveButton)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("autosave", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);
		const int listId = makeListWithOneLine(*handle, 1, 100, 1);

		PartManager::PartlistController controller(handle.get());
		PartManager::PartlistEditorDialog dialog(controller, listId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTableWidget* table = UnitTest::Gui::find<QTableWidget>("itemTable", &dialog);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &dialog);
		QPushButton* addLine = UnitTest::Gui::find<QPushButton>("addLineButton", &dialog);
		QPushButton* removeLine = UnitTest::Gui::find<QPushButton>("removeLineButton", &dialog);
		TEST_ASSERT_M(table && name && addLine && removeLine, "the editor's widgets are gone");

		TEST_ASSERT(UnitTest::Gui::click(addLine));
		TEST_COMPARE(table->rowCount(), 2);
		// The new line points at nothing yet, which is §4's unresolved state — it has to be
		// stored that way, not silently attached to the first part in the picker.
		std::vector<PartManager::PartlistLine> lines = controller.lines(listId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(2));
		TEST_ASSERT_M(!lines[1].resolved, "a freshly added line must start unresolved");

		// Removing needs a selection; Add Line already selected the row it appended.
		TEST_ASSERT_M(removeLine->isEnabled(), "the appended row should be selected and removable");
		TEST_ASSERT(UnitTest::Gui::click(removeLine));
		TEST_COMPARE(table->rowCount(), 1);
		TEST_COMPARE(controller.lines(listId).size(), static_cast<size_t>(1));

		// §10: no Save button anywhere, so closing the dialog is what has to flush the header.
		// clearAndType, not type: the field already holds the list's name and type() appends.
		TEST_ASSERT(UnitTest::Gui::clearAndType(name, "PSU Rev D"));
		UnitTest::Gui::closeWindow(&dialog);

		PartManager::Partlist stored;
		TEST_ASSERT(controller.load(listId, stored));
		TEST_COMPARE(stored.name, std::string("PSU Rev D"));
	}
};

TEST_INSTANTIATE(TST_PartlistEditorGui);
