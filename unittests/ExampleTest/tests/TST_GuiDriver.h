#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

// Proves the GUI driver itself, on throwaway widgets rather than on this app's screens — the point
// is that the driver works, not that PartManager's dialogs do.
//
// Every lookup goes through objectName or visible text, and every position comes from the widget at
// run time, so none of this breaks when a layout moves. Renaming a widget breaks it; that is the
// trade, and it is the right one.
class TST_GuiDriver : public UnitTest::Test
{
	TEST_CLASS(TST_GuiDriver)
public:
	TST_GuiDriver()
		: Test("TST_GuiDriver")
	{
		ADD_TEST(TST_GuiDriver::syntheticClickAndTypeReachTheWidget);
		ADD_TEST(TST_GuiDriver::widgetsAreFoundByObjectNameTextAndClass);
		ADD_TEST(TST_GuiDriver::hiddenOrDisabledWidgetsRefuseTheClick);
		ADD_TEST(TST_GuiDriver::commonWidgetsAreDrivenThroughTheirUi);
		ADD_TEST(TST_GuiDriver::tablesAndTreesAreAddressedByWhatTheyShow);
		ADD_TEST(TST_GuiDriver::modalDialogAndMessageBoxAreDrivenFromTheEventLoop);
		ADD_TEST(TST_GuiDriver::menusKeySequencesAndDragging);
		ADD_TEST(TST_GuiDriver::realMouseInputHitsTheWidgetItAimsAt);
	}

private:

	// One throwaway window holding one of everything the driver claims to handle.
	struct Fixture
	{
		QWidget window;
		QPushButton* button = nullptr;
		QLineEdit* edit = nullptr;
		QCheckBox* box = nullptr;
		QComboBox* combo = nullptr;
		QSpinBox* spin = nullptr;
		QSlider* slider = nullptr;
		QTableWidget* table = nullptr;
		QTreeWidget* tree = nullptr;
		QListWidget* list = nullptr;
		QTabWidget* tabs = nullptr;
		int clickCount = 0;

		Fixture()
		{
			window.setObjectName("guiTestWindow");
			QVBoxLayout* layout = new QVBoxLayout(&window);

			button = new QPushButton("Press me", &window);
			button->setObjectName("pressButton");
			edit = new QLineEdit(&window);
			edit->setObjectName("textEdit");
			box = new QCheckBox("Check me", &window);
			box->setObjectName("checkBox");
			combo = new QComboBox(&window);
			combo->setObjectName("unitCombo");
			combo->addItems({ "Ohm", "Farad", "Henry" });
			spin = new QSpinBox(&window);
			spin->setObjectName("quantitySpin");
			spin->setRange(-1000, 1000);
			slider = new QSlider(Qt::Horizontal, &window);
			slider->setObjectName("levelSlider");
			slider->setRange(0, 100);

			table = new QTableWidget(3, 2, &window);
			table->setObjectName("partTable");
			table->setSelectionBehavior(QAbstractItemView::SelectRows);
			const char* names[] = { "Resistor", "Capacitor", "MOSFET" };
			const char* stock[] = { "10", "20", "30" };
			for (int row = 0; row < 3; ++row)
			{
				table->setItem(row, 0, new QTableWidgetItem(QString::fromLatin1(names[row])));
				table->setItem(row, 1, new QTableWidgetItem(QString::fromLatin1(stock[row])));
			}

			tree = new QTreeWidget(&window);
			tree->setObjectName("categoryTree");
			QTreeWidgetItem* transistor = new QTreeWidgetItem(tree, QStringList("Transistor (2)"));
			new QTreeWidgetItem(transistor, QStringList("MOSFET (2)"));
			new QTreeWidgetItem(tree, QStringList("Diode (2)"));

			list = new QListWidget(&window);
			list->setObjectName("tagList");
			list->addItems({ "SMD", "THT", "Obsolete" });

			tabs = new QTabWidget(&window);
			tabs->setObjectName("tabs");
			tabs->addTab(new QWidget(tabs), "Home");
			tabs->addTab(new QWidget(tabs), "Parts");

			for (QWidget* widget : QList<QWidget*>{ button, edit, box, combo, spin, slider,
				table, tree, list, tabs })
			{
				layout->addWidget(widget);
			}
			QObject::connect(button, &QPushButton::clicked, [this]() { ++clickCount; });

			window.resize(420, 720);
			UnitTest::Gui::showAndWait(&window);
		}

		~Fixture()
		{
			window.close();
		}
	};

	TEST_FUNCTION(syntheticClickAndTypeReachTheWidget)
	{
		TEST_START;

		TEST_ASSERT_M(UnitTest::Gui::ensureApplication(), "no QApplication could be created");
		Fixture fixture;

		TEST_ASSERT_M(UnitTest::Gui::click(fixture.button), "click was refused");
		TEST_COMPARE(fixture.clickCount, 1);

		// Per-character key events, so a validator or a textChanged handler sees what a typist types.
		TEST_ASSERT(UnitTest::Gui::type(fixture.edit, "4k7"));
		TEST_COMPARE(fixture.edit->text(), QString("4k7"));

		// Replacing the contents the way a user does: select all, then type over it.
		TEST_ASSERT(UnitTest::Gui::clearAndType(fixture.edit, "100n"));
		TEST_COMPARE(fixture.edit->text(), QString("100n"));

		TEST_ASSERT(UnitTest::Gui::keyClick(fixture.edit, Qt::Key_Backspace));
		TEST_COMPARE(fixture.edit->text(), QString("100"));
	}

	TEST_FUNCTION(widgetsAreFoundByObjectNameTextAndClass)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		Fixture fixture;

		QPushButton* byName = UnitTest::Gui::find<QPushButton>("pressButton");
		TEST_ASSERT_M(byName == fixture.button, "objectName lookup found the wrong widget");

		// For widgets nobody named — what the user reads is the address.
		QWidget* byText = UnitTest::Gui::findWidgetByText("Press me");
		TEST_ASSERT_M(byText == fixture.button, "text lookup found the wrong widget");

		const QList<QWidget*> combos = UnitTest::Gui::findChildrenOfClass("QComboBox", &fixture.window);
		TEST_COMPARE(combos.size(), 1);

		// Moving the widget must not change a thing — this is what makes the tests layout-proof.
		fixture.window.layout()->removeWidget(fixture.button);
		static_cast<QVBoxLayout*>(fixture.window.layout())->insertWidget(4, fixture.button);
		UnitTest::Gui::waitFor([]() { return false; }, 50);

		TEST_ASSERT(UnitTest::Gui::click(byName));
		TEST_COMPARE(fixture.clickCount, 1);

		TEST_ASSERT_M(UnitTest::Gui::find<QPushButton>("noSuchButton") == nullptr,
			"a missing objectName must return null, not something nearby");

		// The thing you actually want printed when a lookup fails.
		const QString tree = UnitTest::Gui::dumpWidgetTree(&fixture.window);
		TEST_ASSERT_M(tree.contains("pressButton") && tree.contains("QTableWidget"),
			"the widget dump should name the widgets it walked");
	}

	TEST_FUNCTION(hiddenOrDisabledWidgetsRefuseTheClick)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		Fixture fixture;

		// A test that "clicks" a button the user could not have clicked is worse than no test.
		fixture.button->setEnabled(false);
		TEST_ASSERT_M(!UnitTest::Gui::click(fixture.button), "a disabled button accepted a click");
		TEST_COMPARE(fixture.clickCount, 0);

		fixture.button->setEnabled(true);
		fixture.button->hide();
		UnitTest::Gui::waitFor([&fixture]() { return !fixture.button->isVisible(); }, 1000);
		TEST_ASSERT_M(!UnitTest::Gui::click(fixture.button), "a hidden button accepted a click");
		TEST_COMPARE(fixture.clickCount, 0);

		fixture.button->show();
		UnitTest::Gui::waitFor([&fixture]() { return fixture.button->isVisible(); }, 1000);
		TEST_ASSERT(UnitTest::Gui::click(fixture.button));
		TEST_COMPARE(fixture.clickCount, 1);
	}

	TEST_FUNCTION(commonWidgetsAreDrivenThroughTheirUi)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		Fixture fixture;

		// setChecked() clicks rather than calling the setter, so `toggled` handlers actually run.
		int toggles = 0;
		QObject::connect(fixture.box, &QCheckBox::toggled, [&toggles](bool) { ++toggles; });
		TEST_ASSERT(UnitTest::Gui::setChecked(fixture.box, true));
		TEST_ASSERT(fixture.box->isChecked());
		TEST_COMPARE(toggles, 1);
		// Already in the wanted state: no second toggle, and still not a failure.
		TEST_ASSERT(UnitTest::Gui::setChecked(fixture.box, true));
		TEST_COMPARE(toggles, 1);

		TEST_ASSERT(UnitTest::Gui::selectComboText(fixture.combo, "Henry"));
		TEST_COMPARE(fixture.combo->currentText(), QString("Henry"));
		TEST_ASSERT_M(!UnitTest::Gui::selectComboText(fixture.combo, "Weber"),
			"selecting an item that does not exist must fail");

		// Typed and committed, so the validator and editingFinished both run.
		int editingFinished = 0;
		QObject::connect(fixture.spin, &QSpinBox::editingFinished, [&editingFinished]() { ++editingFinished; });
		TEST_ASSERT(UnitTest::Gui::setSpinValue(fixture.spin, 42));
		TEST_COMPARE(fixture.spin->value(), 42);
		TEST_ASSERT_M(editingFinished > 0, "the spin box was set without ever finishing an edit");

		// Negative values matter here: PartManager lets stock go below zero on purpose.
		TEST_ASSERT(UnitTest::Gui::setSpinValue(fixture.spin, -7));
		TEST_COMPARE(fixture.spin->value(), -7);

		TEST_ASSERT(UnitTest::Gui::setSliderValue(fixture.slider, 60));
		TEST_COMPARE(fixture.slider->value(), 60);

		TEST_ASSERT(UnitTest::Gui::selectTab(fixture.tabs, "Parts"));
		TEST_COMPARE(fixture.tabs->currentIndex(), 1);
	}

	TEST_FUNCTION(tablesAndTreesAreAddressedByWhatTheyShow)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		Fixture fixture;

		TEST_COMPARE(UnitTest::Gui::rowCount(fixture.table), 3);
		TEST_COMPARE(UnitTest::Gui::columnCount(fixture.table), 2);
		TEST_COMPARE(UnitTest::Gui::cellText(fixture.table, 2, 0), QString("MOSFET"));

		// A test should never hard-code a row index it cannot see on screen.
		TEST_COMPARE(UnitTest::Gui::rowWithText(fixture.table, "Capacitor"), 1);
		TEST_COMPARE(UnitTest::Gui::rowWithText(fixture.table, "Nothing"), -1);

		TEST_ASSERT(UnitTest::Gui::clickRowWithText(fixture.table, "MOSFET"));
		TEST_COMPARE(fixture.table->currentRow(), 2);

		int doubleClicked = 0;
		QObject::connect(fixture.table, &QTableWidget::doubleClicked, [&doubleClicked](const QModelIndex&)
			{ ++doubleClicked; });
		TEST_ASSERT(UnitTest::Gui::doubleClickCell(fixture.table, 0, 0));
		TEST_COMPARE(doubleClicked, 1);

		// A tree by visible path; the "(2)" count suffix must not have to be spelled out.
		TEST_ASSERT(UnitTest::Gui::clickTreeItem(fixture.tree, { "Transistor", "MOSFET" }));
		TEST_ASSERT_M(fixture.tree->currentItem() != nullptr, "no tree item ended up current");
		TEST_COMPARE(fixture.tree->currentItem()->text(0), QString("MOSFET (2)"));
		TEST_ASSERT_M(UnitTest::Gui::findTreeItem(fixture.tree, { "Transistor", "BJT" }) == nullptr,
			"a path that does not exist must return null");

		TEST_ASSERT(UnitTest::Gui::clickListItem(fixture.list, "Obsolete"));
		TEST_ASSERT_M(fixture.list->currentItem() != nullptr, "no list item ended up current");
		TEST_COMPARE(fixture.list->currentItem()->text(), QString("Obsolete"));
	}

	TEST_FUNCTION(modalDialogAndMessageBoxAreDrivenFromTheEventLoop)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		QDialog dialog;
		dialog.setObjectName("demoDialog");
		QVBoxLayout* layout = new QVBoxLayout(&dialog);
		QLineEdit* quantity = new QLineEdit(&dialog);
		quantity->setObjectName("quantityEdit");
		QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
			&dialog);
		buttons->setObjectName("buttonBox");
		layout->addWidget(quantity);
		layout->addWidget(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

		// Registered BEFORE exec(), which never returns until something closes the dialog.
		UnitTest::Gui::onNextWindow("demoDialog", [](QWidget*)
			{
				QLineEdit* edit = UnitTest::Gui::find<QLineEdit>("quantityEdit");
				QWidget* ok = UnitTest::Gui::findWidgetByText("OK");
				if (edit && ok)
				{
					UnitTest::Gui::type(edit, "12");
					UnitTest::Gui::click(ok);
				}
			});

		TEST_COMPARE(dialog.exec(), static_cast<int>(QDialog::Accepted));
		TEST_COMPARE(quantity->text(), QString("12"));

		// The everyday case: a QMessageBox nobody named and nobody holds a pointer to.
		UnitTest::Gui::onNextMessageBox("Yes");
		const int answer = QMessageBox::question(nullptr, "Remove", "Remove this from the list?",
			QMessageBox::Yes | QMessageBox::No);
		TEST_COMPARE(answer, static_cast<int>(QMessageBox::Yes));

		UnitTest::Gui::onNextMessageBox("No");
		TEST_COMPARE(QMessageBox::question(nullptr, "Remove", "Remove this from the list?",
			QMessageBox::Yes | QMessageBox::No), static_cast<int>(QMessageBox::No));
	}

	TEST_FUNCTION(menusKeySequencesAndDragging)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		QWidget window;
		window.setObjectName("menuWindow");
		QVBoxLayout* layout = new QVBoxLayout(&window);
		QMenuBar* menuBar = new QMenuBar(&window);
		QMenu* fileMenu = menuBar->addMenu("&File");
		QMenu* exportMenu = fileMenu->addMenu("Export");
		QAction* csvAction = exportMenu->addAction("CSV...");
		csvAction->setObjectName("exportCsvAction");
		QAction* saveAction = fileMenu->addAction("Save");
		saveAction->setShortcut(QKeySequence("Ctrl+S"));
		layout->setMenuBar(menuBar);

		QSlider* slider = new QSlider(Qt::Horizontal, &window);
		slider->setObjectName("dragSlider");
		slider->setRange(0, 100);
		slider->setValue(0);
		layout->addWidget(slider);
		window.resize(400, 160);
		UnitTest::Gui::showAndWait(&window);

		int csvTriggered = 0;
		QObject::connect(csvAction, &QAction::triggered, [&csvTriggered]() { ++csvTriggered; });
		int saved = 0;
		QObject::connect(saveAction, &QAction::triggered, [&saved]() { ++saved; });

		// A nested menu by the text the user reads, without opening anything by hand.
		TEST_ASSERT(UnitTest::Gui::triggerMenuPath(menuBar, { "File", "Export", "CSV..." }));
		TEST_COMPARE(csvTriggered, 1);
		TEST_ASSERT_M(!UnitTest::Gui::triggerMenuPath(menuBar, { "File", "Nope" }),
			"a menu path that does not exist must fail rather than trigger something else");

		// By objectName, ignoring where it happens to live in the menu tree.
		QAction* found = UnitTest::Gui::findAction("exportCsvAction", &window);
		TEST_ASSERT_M(found == csvAction, "findAction did not find the action by objectName");

		// Dragging: press, interpolated moves, release. A slider integrates the movement, so a
		// single jump would not move it at all.
		const int before = slider->value();
		TEST_ASSERT(UnitTest::Gui::drag(slider, QPoint(5, slider->height() / 2),
			QPoint(slider->width() - 5, slider->height() / 2)));
		TEST_ASSERT_M(slider->value() > before, "dragging the slider did not move it");

		window.close();
	}

	// The other layer: a genuine OS-level click, aimed with the widget's own geometry. Slower and
	// disturbable by anything else on screen, so it exists to prove reachability, not for bulk use.
	TEST_FUNCTION(realMouseInputHitsTheWidgetItAimsAt)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		if (!UnitTest::Gui::isAvailable())
		{
			TEST_MESSAGE("no screen available, real-input layer not exercised");
			return;
		}
		Fixture fixture;
		fixture.window.move(80, 80);
		UnitTest::Gui::waitFor([]() { return false; }, 200);

		if (!UnitTest::Gui::clickReal(fixture.button))
		{
			// Not a failure: the platform simply has no injection backend yet.
			TEST_MESSAGE("real input unavailable on this platform, skipped");
			return;
		}
		UnitTest::Gui::waitFor([&fixture]() { return fixture.clickCount > 0; }, 2000);
		TEST_COMPARE(fixture.clickCount, 1);

		// The check box is the case that proves the aiming: a layout stretches it across the row,
		// but only the indicator-and-label rect reacts, so a centred click would miss.
		if (UnitTest::Gui::clickReal(fixture.box))
		{
			UnitTest::Gui::waitFor([&fixture]() { return fixture.box->isChecked(); }, 2000);
			TEST_ASSERT_M(fixture.box->isChecked(), "the real click missed the check box's click rect");
		}

		UnitTest::Gui::click(fixture.edit);
		fixture.edit->setFocus(Qt::OtherFocusReason);
		if (UnitTest::Gui::typeReal("100n"))
		{
			UnitTest::Gui::waitFor([&fixture]() { return fixture.edit->text().size() == 4; }, 2000);
			TEST_COMPARE(fixture.edit->text(), QString("100n"));
		}
	}
};

TEST_INSTANTIATE(TST_GuiDriver);
