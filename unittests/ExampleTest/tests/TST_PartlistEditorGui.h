#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_StockRepository.h"
#include "widgets/PartManager_PartlistPanel.h"

#include <QComboBox>
#include <QDataStream>
#include <QDropEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <filesystem>

// The §4 partlist panel driven through its widgets. The arithmetic is TST_PartlistRepository's
// job; what is checked here is the wiring the repository cannot see — that a multiplier change
// repaints the computed columns, that a line added through the button reaches the database, that
// the §10 autosave really writes without a Save button, and that a part dropped in from the main
// window's table becomes a line.
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
		ADD_TEST(TST_PartlistEditorGui::droppingAPartAddsALineAndASecondDropStacks);
	}

private:

	// Columns of the panel's grid, in the order it builds them.
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

	static int makePart(PartManager::DatabaseHandle& handle, const std::string& name, int stock)
	{
		// createNew() seeds the type templates, so there is always one to hang the part off.
		std::vector<PartManager::PartType> types =
			PartManager::PartTypeRepository::listTypes(handle.connection());
		PartManager::Part part;
		part.partTypeId = types.empty() ? 0 : types.front().id;
		part.name = name;
		part.mpn = name + "-MPN";
		const int partId = PartManager::PartRepository::insertPart(handle.connection(), part);
		PartManager::StockRepository::restock(handle.connection(), partId, stock, "test fixture");
		return partId;
	}

	// One part with a known quantity, plus a one-line partlist pointing at it.
	static int makeListWithOneLine(PartManager::DatabaseHandle& handle, int quantityPerUnit,
		int stock, int multiplier)
	{
		PartManager::PartlistController controller(&handle);
		const int partId = makePart(handle, "RC0603-4K7", stock);

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

	// The payload Qt's item views put on a drag — exactly what the part table produces once
	// setDragEnabled(true) is on, and what PartlistPanel::droppedPartId() has to decode.
	static QMimeData* partPayload(int partId)
	{
		QByteArray encoded;
		QDataStream stream(&encoded, QIODevice::WriteOnly);
		QMap<int, QVariant> roles;
		roles.insert(Qt::DisplayRole, QStringLiteral("a part"));
		roles.insert(Qt::UserRole, partId);
		stream << 0 << 0 << roles;

		QMimeData* mime = new QMimeData();
		mime->setData(PartManager::PartlistPanel::PartMimeType, encoded);
		return mime;
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
		PartManager::PartlistPanel panel(handle.get());
		panel.showPartlist(listId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&panel));

		QTableWidget* table = UnitTest::Gui::find<QTableWidget>("itemTable", &panel);
		QSpinBox* multiplier = UnitTest::Gui::find<QSpinBox>("multiplierSpin", &panel);
		QLabel* status = UnitTest::Gui::find<QLabel>("statusLabel", &panel);
		TEST_ASSERT_M(table && multiplier && status,
			"the panel no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&panel).toStdString());

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

		UnitTest::Gui::closeWindow(&panel);
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
		PartManager::PartlistPanel panel(handle.get());
		panel.showPartlist(listId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&panel));

		QTableWidget* table = UnitTest::Gui::find<QTableWidget>("itemTable", &panel);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &panel);
		QPushButton* addLine = UnitTest::Gui::find<QPushButton>("addLineButton", &panel);
		QPushButton* removeLine = UnitTest::Gui::find<QPushButton>("removeLineButton", &panel);
		TEST_ASSERT_M(table && name && addLine && removeLine, "the panel's widgets are gone");

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

		// §10: no Save button anywhere, so closing the panel is what has to flush the header.
		// clearAndType, not type: the field already holds the list's name and type() appends.
		TEST_ASSERT(UnitTest::Gui::clearAndType(name, "PSU Rev D"));
		UnitTest::Gui::closeWindow(&panel);

		PartManager::Partlist stored;
		TEST_ASSERT(controller.load(listId, stored));
		TEST_COMPARE(stored.name, std::string("PSU Rev D"));
	}

	TEST_FUNCTION(droppingAPartAddsALineAndASecondDropStacks)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("drop", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartlistController controller(handle.get());
		const int capacitor = makePart(*handle, "C0603-100n", 500);

		PartManager::Partlist partlist;
		partlist.name = "Dropped";
		const int listId = controller.create(partlist);

		PartManager::PartlistPanel panel(handle.get());
		panel.showPartlist(listId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&panel));

		QTableWidget* table = UnitTest::Gui::find<QTableWidget>("itemTable", &panel);
		TEST_ASSERT_M(table != nullptr, "the panel has no itemTable");
		TEST_COMPARE(table->rowCount(), 0);

		// The payload decodes on its own first, so a failure below is about the panel and not
		// about the encoding.
		// event() is called directly rather than through QCoreApplication::sendEvent():
		// QApplication::notify routes drag-and-drop through the platform drag manager and drops
		// a synthetic, non-spontaneous QDropEvent on the floor. What a real drag ends in is this
		// call, so this is the honest way to reach the handler from a test.
		{
			QMimeData* mime = partPayload(capacitor);
			TEST_COMPARE(PartManager::PartlistPanel::droppedPartId(mime), capacitor);
			QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
			static_cast<QObject*>(&panel)->event(&drop);
			TEST_ASSERT_M(drop.isAccepted(), "the panel refused a well-formed part payload");
			delete mime;
		}
		std::vector<PartManager::PartlistLine> lines = controller.lines(listId);
		TEST_COMPARE(lines.size(), static_cast<size_t>(1));
		TEST_COMPARE(table->rowCount(), 1);
		TEST_COMPARE(lines[0].item.partId, capacitor);
		TEST_COMPARE(lines[0].item.quantityPerUnit, 1);

		// The same part again means "two of them", not a second row measuring its own shortfall
		// against the same untouched stock.
		{
			QMimeData* mime = partPayload(capacitor);
			QDropEvent drop(QPointF(10, 10), Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
			static_cast<QObject*>(&panel)->event(&drop);
			delete mime;
		}
		lines = controller.lines(listId);
		TEST_COMPARE(table->rowCount(), 1);
		TEST_COMPARE(lines.size(), static_cast<size_t>(1));
		TEST_COMPARE(lines[0].item.quantityPerUnit, 2);

		UnitTest::Gui::closeWindow(&panel);
	}
};

TEST_INSTANTIATE(TST_PartlistEditorGui);
