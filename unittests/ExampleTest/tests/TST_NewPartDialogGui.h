#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "mouser/PartManager_MouserSearchService.h"
#include "ui/PartManager_NewPartDialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <filesystem>

// §6's create-part-from-Mouser flow, at the seam where the two halves meet: MouserSearchService
// produces a prefill, NewPartDialog has to land it on the generated form. The mapping itself is
// TST_MouserSearchService's job and the search call needs a key and a network, so neither is
// repeated here — what is checked is that a prefill actually reaches the widgets, including the
// runtime-generated attribute rows, which is the part that silently breaks when the form is
// rebuilt in the wrong order.
//
// Runs against a throwaway database in %TEMP%, never the user's own.
class TST_NewPartDialogGui : public UnitTest::Test
{
	TEST_CLASS(TST_NewPartDialogGui)
public:
	TST_NewPartDialogGui()
		: Test("TST_NewPartDialogGui")
	{
		ADD_TEST(TST_NewPartDialogGui::mouserPrefillReachesEveryField);
		ADD_TEST(TST_NewPartDialogGui::anUnmappedCategoryLeavesTheTypeToTheUser);
		ADD_TEST(TST_NewPartDialogGui::theMouserButtonsFollowTheArticleNumberField);
		ADD_TEST(TST_NewPartDialogGui::openOnMouserUsesTheSameUrlTheEditorWould);
	}

private:

	// A fresh seeded database for one test. Null on failure, with the reason in outError.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_NewPartDialogGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "NewPart", outError);
	}

	// The shape MouserSearchService::toPrefill() hands over for a plain 4k7 0603 resistor.
	static PartManager::MouserPartPrefill resistorPrefill()
	{
		PartManager::MouserPartPrefill prefill;
		prefill.mouserPartNumber = "603-RC0603FR-074K7L";
		prefill.suggestedTypeName = "Resistor";
		prefill.datasheetUrl = "https://example.invalid/rc0603.pdf";
		prefill.productDetailUrl = "https://www.mouser.com/ProductDetail/rc0603";
		prefill.part.name = "RC0603FR-074K7L";
		prefill.part.manufacturer = "YAGEO";
		prefill.part.mpn = "RC0603FR-074K7L";
		prefill.part.package = "0603";
		prefill.part.description = "Thick Film Resistors - SMD 4.7K OHM 1%";
		prefill.part.attributes = "{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"}}";
		prefill.unmappedAttributes = { "Operating Temperature" };
		return prefill;
	}

	TEST_FUNCTION(mouserPrefillReachesEveryField)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("prefill", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::NewPartDialog dialog(handle.get());
		dialog.setPrefill(resistorPrefill());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		// The category is a button opening PartTypePickerDialog now, not a combo. Its label is the
		// chosen type's full path and it carries the id as a property, which is the same fact the
		// combo's currentData() used to carry -- so the assertions below lose nothing.
		QPushButton* type = UnitTest::Gui::find<QPushButton>("typeButton", &dialog);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &dialog);
		QLineEdit* manufacturer = UnitTest::Gui::find<QLineEdit>("manufacturerEdit", &dialog);
		QLineEdit* mpn = UnitTest::Gui::find<QLineEdit>("mpnEdit", &dialog);
		QLineEdit* package = UnitTest::Gui::find<QLineEdit>("packageEdit", &dialog);
		QPlainTextEdit* description = UnitTest::Gui::find<QPlainTextEdit>("descriptionEdit", &dialog);
		QLabel* header = UnitTest::Gui::find<QLabel>("headerLabel", &dialog);
		QPushButton* create = UnitTest::Gui::find<QPushButton>("createButton", &dialog);
		TEST_ASSERT_M(type && name && manufacturer && mpn && package && description && header && create,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		TEST_COMPARE(type->text(), QString("Resistor"));
		TEST_ASSERT_M(type->property("partTypeId").toInt() != 0,
			"the matched Mouser category did not reach the type button");
		TEST_COMPARE(name->text(), QString("RC0603FR-074K7L"));
		TEST_COMPARE(manufacturer->text(), QString("YAGEO"));
		TEST_COMPARE(mpn->text(), QString("RC0603FR-074K7L"));
		TEST_COMPARE(package->text(), QString("0603"));
		TEST_ASSERT_M(description->toPlainText().contains("4.7K"),
			"the Mouser description did not reach the form");

		// Anything Mouser published that we could not place has to be named, or the user has no
		// way of knowing a field was left for them.
		TEST_ASSERT_M(header->text().contains("Operating Temperature"),
			"the unmapped Mouser attribute was not reported: " + header->text().toStdString());

		// The only required attribute the seeded Resistor template declares is `resistance`, so
		// an enabled Create is proof the attributes JSON landed on the generated rows — which it
		// only does when the type combo was set before the values were written.
		TEST_ASSERT_M(create->isEnabled(),
			"Create stayed disabled, so the prefilled resistance never reached the attribute form");

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(anUnmappedCategoryLeavesTheTypeToTheUser)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("nocategory", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// §6 is explicit that a wrong template must never be attached silently, so an
		// undecidable category has to say so rather than pick the first entry in the combo.
		// Genuinely undecidable now means the *description* names no type either: matchPartType()
		// reads that too, and "Thick Film Resistors" in the fixture below would place the part
		// correctly — which is the new behaviour, not a hole in this test.
		PartManager::MouserPartPrefill prefill = resistorPrefill();
		prefill.suggestedTypeName.clear();
		prefill.mouserCategory = "Evaluation Boards";
		prefill.part.description = "Evaluation board, 3.3 V, 16-pin";
		prefill.part.attributes = "{}";

		PartManager::NewPartDialog dialog(handle.get());
		dialog.setPrefill(prefill);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QLabel* header = UnitTest::Gui::find<QLabel>("headerLabel", &dialog);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &dialog);
		QPushButton* type = UnitTest::Gui::find<QPushButton>("typeButton", &dialog);
		QPushButton* create = UnitTest::Gui::find<QPushButton>("createButton", &dialog);
		QLabel* validation = UnitTest::Gui::find<QLabel>("validationLabel", &dialog);
		TEST_ASSERT_M(header && name && type && create && validation,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		TEST_ASSERT_M(header->text().contains("did not map"),
			"an unmapped category must be called out: " + header->text().toStdString());
		// Everything that does not depend on the category still gets filled in.
		TEST_COMPARE(name->text(), QString("RC0603FR-074K7L"));

		// The button stays on "(none)", which carries type id 0 — and Create stays disabled while
		// it does, so a part can no longer be filed under whatever type sorted first.
		TEST_COMPARE(type->property("partTypeId").toInt(), 0);
		TEST_COMPARE(type->text(), PartManager::NewPartDialog::tr("(none — select a category)"));
		TEST_ASSERT_M(!create->isEnabled(),
			"Create must stay disabled until a real category is picked");
		TEST_ASSERT_M(validation->text().contains(PartManager::NewPartDialog::tr("Part type")),
			"the missing-field list has to name the part type, or a disabled Create explains nothing");

		UnitTest::Gui::closeWindow(&dialog);
	}

	// The two §6 buttons beside the Mouser P/N field. Neither of them is *pressed* here: "Fetch…"
	// would need an API key and a network round trip, which a unit test must never take. What is
	// checked is the half that breaks silently — that both follow the field live, so the dialog
	// never offers to open or look up a number that is not there.
	TEST_FUNCTION(theMouserButtonsFollowTheArticleNumberField)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("mouserbuttons", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::NewPartDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QLineEdit* mouser = UnitTest::Gui::find<QLineEdit>("mouserEdit", &dialog);
		QPushButton* open = UnitTest::Gui::find<QPushButton>("openMouserButton", &dialog);
		QPushButton* fetch = UnitTest::Gui::find<QPushButton>("fetchMouserButton", &dialog);
		TEST_ASSERT_M(mouser && open && fetch,
			"the dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		// A blank form has no article number, so there is nothing to open and nothing to look up.
		TEST_ASSERT_M(mouser->text().isEmpty(), "the blank form must start with an empty Mouser P/N");
		TEST_ASSERT_M(!open->isEnabled(), "Open on Mouser must be disabled without an article number");
		TEST_ASSERT_M(!fetch->isEnabled(), "Fetch must be disabled without an article number");

		// textChanged, not editingFinished: the button has to be live while the user is still typing.
		mouser->setText("595-LM358DR");
		TEST_ASSERT_M(open->isEnabled(), "Open on Mouser stayed disabled with a number in the field");
		TEST_ASSERT_M(fetch->isEnabled(), "Fetch stayed disabled with a number in the field");

		// Whitespace is not an article number.
		mouser->setText("   ");
		TEST_ASSERT_M(!open->isEnabled(), "whitespace must not enable Open on Mouser");
		TEST_ASSERT_M(!fetch->isEnabled(), "whitespace must not enable Fetch");

		mouser->clear();
		TEST_ASSERT_M(!open->isEnabled(), "clearing the field must disable Open on Mouser again");
		TEST_ASSERT_M(!fetch->isEnabled(), "clearing the field must disable Fetch again");

		UnitTest::Gui::closeWindow(&dialog);
	}

	// What the Open button hands to the browser, asserted through the one function it is allowed
	// to build a URL with. A hand-typed number has no product page, so it gets a search; a number
	// that still matches the prefill opens the prefill's own ProductDetailUrl (§6).
	TEST_FUNCTION(openOnMouserUsesTheSameUrlTheEditorWould)
	{
		TEST_START;

		// A typed number: no stored URL, so mouserPageUrl() has to fall back to a search that
		// actually carries the number.
		const std::string typed = PartManager::PartEditorController::mouserPageUrl(
			"595-LM358DR", std::string());
		TEST_ASSERT_M(typed.find("mouser.") != std::string::npos,
			"a typed number must still produce a mouser.com URL: " + typed);
		TEST_ASSERT_M(typed.find("595-LM358DR") != std::string::npos,
			"the search URL lost the article number: " + typed);

		// A fetched or prefilled part: its own page wins over a constructed search, which is what
		// the Open button passes the prefill's productDetailUrl for.
		const PartManager::MouserPartPrefill prefill = resistorPrefill();
		const std::string stored = PartManager::PartEditorController::mouserPageUrl(
			prefill.mouserPartNumber, prefill.productDetailUrl);
		TEST_COMPARE(stored, prefill.productDetailUrl);

		// And with no number at all there is no URL to open — which is why the button is disabled
		// in that state rather than opening mouser.com's front page.
		TEST_ASSERT_M(PartManager::PartEditorController::mouserPageUrl(
			std::string(), std::string()).empty(),
			"an empty article number must not produce a URL");
	}
};

TEST_INSTANTIATE(TST_NewPartDialogGui);
