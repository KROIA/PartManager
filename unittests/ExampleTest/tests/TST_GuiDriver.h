#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

// Proves the GUI driver itself, on throwaway widgets rather than on this app's screens — the point
// is that the driver works, not that PartManager's dialogs do.
//
// Every lookup goes through objectName, and every position comes from the widget at run time, so
// none of this breaks when a layout moves. Renaming a widget breaks it, which is the trade.
class TST_GuiDriver : public UnitTest::Test
{
	TEST_CLASS(TST_GuiDriver)
public:
	TST_GuiDriver()
		: Test("TST_GuiDriver")
	{
		ADD_TEST(TST_GuiDriver::syntheticClickAndTypeReachTheWidget);
		ADD_TEST(TST_GuiDriver::widgetsAreFoundByObjectNameNotByPosition);
		ADD_TEST(TST_GuiDriver::hiddenOrDisabledWidgetsRefuseTheClick);
		ADD_TEST(TST_GuiDriver::modalDialogIsDrivenFromTheEventLoop);
		ADD_TEST(TST_GuiDriver::realMouseInputHitsTheWidgetItAimsAt);
	}

private:

	// One throwaway window: a button that counts clicks, a line edit, a check box.
	struct Fixture
	{
		QWidget window;
		QPushButton* button = nullptr;
		QLineEdit* edit = nullptr;
		QCheckBox* box = nullptr;
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

			layout->addWidget(button);
			layout->addWidget(edit);
			layout->addWidget(box);

			QObject::connect(button, &QPushButton::clicked, [this]() { ++clickCount; });

			window.resize(320, 180);
			window.show();
			UnitTest::Gui::waitFor([this]() { return window.isVisible(); }, 2000);
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

		TEST_ASSERT(UnitTest::Gui::click(fixture.box));
		TEST_ASSERT_M(fixture.box->isChecked(), "the check box did not toggle");

		// Per-character key events, so a validator or a textChanged handler sees what a typist types.
		TEST_ASSERT(UnitTest::Gui::type(fixture.edit, "4k7"));
		TEST_COMPARE(fixture.edit->text(), QString("4k7"));
	}

	TEST_FUNCTION(widgetsAreFoundByObjectNameNotByPosition)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());
		Fixture fixture;

		// Found through the top-level list, without the test holding a pointer to anything.
		QPushButton* button = UnitTest::Gui::find<QPushButton>("pressButton");
		TEST_ASSERT_M(button != nullptr, "pressButton was not found by objectName");
		TEST_ASSERT(button == fixture.button);

		// Moving the widget must not change a thing — this is what makes the tests layout-proof.
		fixture.window.layout()->removeWidget(fixture.button);
		static_cast<QVBoxLayout*>(fixture.window.layout())->insertWidget(2, fixture.button);
		UnitTest::Gui::waitFor([]() { return false; }, 50);

		TEST_ASSERT(UnitTest::Gui::click(button));
		TEST_COMPARE(fixture.clickCount, 1);

		TEST_ASSERT_M(UnitTest::Gui::find<QPushButton>("noSuchButton") == nullptr,
			"a missing objectName must return null, not something nearby");
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

	TEST_FUNCTION(modalDialogIsDrivenFromTheEventLoop)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		QDialog dialog;
		dialog.setObjectName("demoDialog");
		QVBoxLayout* layout = new QVBoxLayout(&dialog);
		QLineEdit* quantity = new QLineEdit(&dialog);
		quantity->setObjectName("quantityEdit");
		QPushButton* ok = new QPushButton("OK", &dialog);
		ok->setObjectName("okButton");
		layout->addWidget(quantity);
		layout->addWidget(ok);
		QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);

		// Registered BEFORE exec(), which never returns until something closes the dialog.
		UnitTest::Gui::onNextWindow("demoDialog", [](QWidget*)
			{
				QLineEdit* edit = UnitTest::Gui::find<QLineEdit>("quantityEdit");
				QPushButton* button = UnitTest::Gui::find<QPushButton>("okButton");
				if (edit && button)
				{
					UnitTest::Gui::type(edit, "12");
					UnitTest::Gui::click(button);
				}
			});

		const int result = dialog.exec();
		TEST_COMPARE(result, static_cast<int>(QDialog::Accepted));
		TEST_COMPARE(quantity->text(), QString("12"));
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

		UnitTest::Gui::click(fixture.edit);   // focus, synthetically
		fixture.edit->setFocus(Qt::OtherFocusReason);
		if (UnitTest::Gui::typeReal("100n"))
		{
			UnitTest::Gui::waitFor([&fixture]() { return fixture.edit->text().size() == 4; }, 2000);
			TEST_COMPARE(fixture.edit->text(), QString("100n"));
		}
	}
};

TEST_INSTANTIATE(TST_GuiDriver);
