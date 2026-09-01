#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "mouser/PartManager_MouserSearchService.h"
#include "ui/PartManager_NewPartDialog.h"

#include <QComboBox>
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

		QComboBox* type = UnitTest::Gui::find<QComboBox>("typeCombo", &dialog);
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

		TEST_COMPARE(type->currentText(), QString("Resistor"));
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
		PartManager::MouserPartPrefill prefill = resistorPrefill();
		prefill.suggestedTypeName.clear();
		prefill.part.attributes = "{}";

		PartManager::NewPartDialog dialog(handle.get());
		dialog.setPrefill(prefill);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QLabel* header = UnitTest::Gui::find<QLabel>("headerLabel", &dialog);
		QLineEdit* name = UnitTest::Gui::find<QLineEdit>("nameEdit", &dialog);
		TEST_ASSERT_M(header && name, "headerLabel / nameEdit are gone");

		TEST_ASSERT_M(header->text().contains("did not map"),
			"an unmapped category must be called out: " + header->text().toStdString());
		// Everything that does not depend on the category still gets filled in.
		TEST_COMPARE(name->text(), QString("RC0603FR-074K7L"));

		UnitTest::Gui::closeWindow(&dialog);
	}
};

TEST_INSTANTIATE(TST_NewPartDialogGui);
