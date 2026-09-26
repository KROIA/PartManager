#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "ui/PartManager_MovePartDialog.h"
#include "widgets/PartManager_AttributeFormWidget.h"

#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <filesystem>

// The generated attribute form's "Unrecognised values" footnote (§2, §2b, §10, §11).
//
// A part's values live in `part.attributes` keyed by attribute key; what its type declares lives in
// `part_type_attribute` resolved through §2b inheritance. The two can diverge — a category import
// or a move leaves a part holding a key nothing declares — and until this footnote existed such a
// value was in the database and invisible: no row, no editor, no way to delete it.
//
// The four things that matter are all here: it appears with the key and the value, it is *absent*
// (not merely empty) when there is nothing to report, deleting a line takes the key out of the
// emitted JSON, and MovePartDialog still drops what the target category does not declare — that
// drop is the move's documented effect, so the opt-in must not have resurrected it.
class TST_AttributeFormGui : public UnitTest::Test
{
	TEST_CLASS(TST_AttributeFormGui)
public:
	TST_AttributeFormGui()
		: Test("TST_AttributeFormGui")
	{
		ADD_TEST(TST_AttributeFormGui::anUndeclaredKeyIsShownWithItsValue);
		ADD_TEST(TST_AttributeFormGui::aConsistentPartHasNoSectionAtAll);
		ADD_TEST(TST_AttributeFormGui::deletingALineRemovesTheKeyFromTheJson);
		ADD_TEST(TST_AttributeFormGui::theDefaultStillDropsUndeclaredKeys);
		ADD_TEST(TST_AttributeFormGui::movingAPartStillDropsWhatTheTargetDoesNotDeclare);
	}

private:

	// The one attribute the forms below declare — a §2a dimension, the most common case.
	static std::vector<PartManager::PartTypeAttribute> resistanceOnly()
	{
		PartManager::PartTypeAttribute resistance;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.unit = "\xCE\xA9";
		resistance.datatype = PartManager::AttributeDataType::Dimension;
		return { resistance };
	}

	// A part holding one declared value and one the type knows nothing about.
	static QString jsonWithStrayKey()
	{
		return QStringLiteral("{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"},"
			"\"dielectric\":\"X7R\"}");
	}

	TEST_FUNCTION(anUndeclaredKeyIsShownWithItsValue)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::AttributeFormWidget form;
		form.setUnrecognisedValuesVisible(true);
		form.setAttributes(resistanceOnly());
		form.setValuesJson(jsonWithStrayKey());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&form));

		TEST_COMPARE(form.unrecognisedKeys(), QStringList{ QStringLiteral("dielectric") });

		QWidget* section = UnitTest::Gui::find<QWidget>("unrecognisedSection", &form);
		TEST_ASSERT_M(section != nullptr,
			"the footnote was not built for a part carrying an undeclared key:\n"
			+ UnitTest::Gui::dumpWidgetTree(&form).toStdString());

		// The key alone would not settle "am I willing to lose this" — the stored value has to be
		// on screen too, which is the whole point of showing it rather than hiding it.
		QString shown;
		for (QLabel* label : section->findChildren<QLabel*>())
		{
			shown += label->text() + QStringLiteral("\n");
		}
		TEST_ASSERT_M(shown.contains(QStringLiteral("dielectric")),
			"the undeclared key is not named: " + shown.toStdString());
		TEST_ASSERT_M(shown.contains(QStringLiteral("X7R")),
			"the stored value is not shown: " + shown.toStdString());

		// And a line saying what these are — without it the section reads as a defect report.
		QLabel* explanation = UnitTest::Gui::find<QLabel>("unrecognisedExplanation", &form);
		TEST_ASSERT_M(explanation && !explanation->text().isEmpty(),
			"the section has no explanatory line");

		// Kept, not dropped: the value survives a round trip through the serializer.
		const QJsonObject written =
			QJsonDocument::fromJson(form.valuesJson().toUtf8()).object();
		TEST_COMPARE(written.value(QStringLiteral("dielectric")).toString(), QString("X7R"));
		TEST_ASSERT_M(written.contains(QStringLiteral("resistance")),
			"the declared value was lost while the undeclared one was carried");

		UnitTest::Gui::closeWindow(&form);
	}

	TEST_FUNCTION(aConsistentPartHasNoSectionAtAll)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::AttributeFormWidget form;
		form.setUnrecognisedValuesVisible(true);
		form.setAttributes(resistanceOnly());
		form.setValuesJson(QStringLiteral("{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"}}"));
		TEST_ASSERT(UnitTest::Gui::showAndWait(&form));

		TEST_ASSERT_M(form.unrecognisedKeys().isEmpty(), "a consistent part reported stray keys");

		// Absent, not hidden and not empty: an exception reporter that is always on screen is a
		// permanent fixture, and the one requirement here is that nothing about a healthy part's
		// form changed at all.
		TEST_ASSERT_M(UnitTest::Gui::find<QWidget>("unrecognisedSection", &form) == nullptr,
			"the footnote was built for a part that has nothing to report:\n"
			+ UnitTest::Gui::dumpWidgetTree(&form).toStdString());
		TEST_ASSERT_M(UnitTest::Gui::find<QLabel>("unrecognisedHeading", &form) == nullptr,
			"an empty section heading was left behind");

		// One declared attribute is one QFormLayout row, which is two items — no spacer, no
		// placeholder row, so the layout cannot have shifted.
		QFormLayout* layout = qobject_cast<QFormLayout*>(form.layout());
		TEST_ASSERT(layout != nullptr);
		TEST_COMPARE(layout->rowCount(), 1);

		UnitTest::Gui::closeWindow(&form);
	}

	TEST_FUNCTION(deletingALineRemovesTheKeyFromTheJson)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::AttributeFormWidget form;
		form.setUnrecognisedValuesVisible(true);
		form.setAttributes(resistanceOnly());
		form.setValuesJson(jsonWithStrayKey());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&form));

		// Deleting has to be the user's own act, so it is driven through the button rather than by
		// calling anything — and it has to announce itself, or §10's autosave never writes it away.
		int committed = 0;
		QObject::connect(&form, &PartManager::AttributeFormWidget::valueCommitted,
			&form, [&committed]() { ++committed; });

		QToolButton* remove = UnitTest::Gui::find<QToolButton>("unrecognisedRemoveButton", &form);
		TEST_ASSERT_M(remove != nullptr,
			"the undeclared value has no delete button:\n"
			+ UnitTest::Gui::dumpWidgetTree(&form).toStdString());
		TEST_COMPARE(remove->property("attributeKey").toString(), QString("dielectric"));
		TEST_ASSERT(UnitTest::Gui::click(remove));

		TEST_ASSERT_M(committed > 0, "deleting a value did not reach the autosave trigger");
		TEST_ASSERT_M(form.unrecognisedKeys().isEmpty(), "the deleted key is still held");

		const QJsonObject written =
			QJsonDocument::fromJson(form.valuesJson().toUtf8()).object();
		TEST_ASSERT_M(!written.contains(QStringLiteral("dielectric")),
			"the deleted key is still written: " + form.valuesJson().toStdString());
		TEST_ASSERT_M(written.contains(QStringLiteral("resistance")),
			"deleting the stray value took the real one with it");

		// The last line went, so the whole footnote goes with it — back to a form that looks
		// exactly like a consistent part's.
		TEST_ASSERT_M(UnitTest::Gui::find<QWidget>("unrecognisedSection", &form) == nullptr,
			"an empty footnote was left on screen after the last line was deleted");

		UnitTest::Gui::closeWindow(&form);
	}

	// The default, which is what every caller but the part editor gets: undeclared keys are neither
	// shown nor carried. This is the mechanism MovePartDialog's drop rests on, asserted at the
	// widget itself so a future change to the opt-in cannot quietly flip it.
	TEST_FUNCTION(theDefaultStillDropsUndeclaredKeys)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		PartManager::AttributeFormWidget form;
		form.setAttributes(resistanceOnly());
		form.setValuesJson(jsonWithStrayKey());

		TEST_ASSERT_M(!form.unrecognisedValuesVisible(),
			"the opt-in must stay off unless a caller asks for it");
		TEST_ASSERT_M(form.unrecognisedKeys().isEmpty(),
			"the form collected stray keys nobody asked it to keep");
		TEST_ASSERT_M(UnitTest::Gui::find<QWidget>("unrecognisedSection", &form) == nullptr,
			"the footnote appeared without the opt-in");

		const QJsonObject written =
			QJsonDocument::fromJson(form.valuesJson().toUtf8()).object();
		TEST_ASSERT_M(!written.contains(QStringLiteral("dielectric")),
			"the default serializer emitted a key its attribute list does not declare: "
			+ form.valuesJson().toStdString());
		TEST_ASSERT_M(written.contains(QStringLiteral("resistance")),
			"the declared value was lost");
	}

	// A fresh seeded database for one test. Null on failure, with the reason in outError.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_AttributeFormGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "AttributeForm", outError);
	}

	// The end of the same story, through the real dialog: a deliberate move into a category that
	// does not declare `dielectric` still drops it. The footnote exists so a value is never lost
	// *silently* — a move the user confirmed in front of a list of what goes is not that.
	TEST_FUNCTION(movingAPartStillDropsWhatTheTargetDoesNotDeclare)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("move", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		PartManager::PartType ceramic;
		PartManager::PartType capacitor;
		for (const PartManager::PartType& type : controller.types())
		{
			if (type.name == "Ceramic Capacitor") { ceramic = type; }
			if (type.name == "Capacitor") { capacitor = type; }
		}
		TEST_ASSERT_M(ceramic.id != 0 && capacitor.id != 0,
			"the seeded §2b capacitor types are missing");

		// `dielectric` is Ceramic Capacitor's own attribute; Capacitor, its parent, does not
		// declare it. `capacitance` is inherited and required, and carries over filled, so the
		// dialog's Move button is enabled without any typing.
		PartManager::Part part;
		part.name = "C0603 100n X7R";
		part.partTypeId = ceramic.id;
		part.attributes = "{\"capacitance\":{\"value\":1e-07,\"unit\":\"F\"},"
			"\"dielectric\":\"X7R\"}";

		TEST_ASSERT_M(!PartManager::MovePartDialog::isCleanMove(controller, part, capacitor.id),
			"a move that drops a value must never be treated as clean");

		PartManager::MovePartDialog dialog(controller, part, capacitor, nullptr);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QPushButton* move = qobject_cast<QPushButton*>(
			UnitTest::Gui::findWidgetByText(PartManager::MovePartDialog::tr("Move part"), &dialog));
		TEST_ASSERT_M(move != nullptr,
			"the dialog no longer has the Move button this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());
		TEST_ASSERT_M(move->isEnabled(),
			"Move was disabled although every required field of the target carries over");
		TEST_ASSERT(UnitTest::Gui::click(move));

		const PartManager::Part& moved = dialog.movedPart();
		TEST_COMPARE(moved.partTypeId, capacitor.id);
		const QJsonObject written = QJsonDocument::fromJson(
			QString::fromStdString(moved.attributes).toUtf8()).object();
		TEST_ASSERT_M(!written.contains(QStringLiteral("dielectric")),
			"the move stopped dropping the value the target does not declare: " + moved.attributes);
		TEST_ASSERT_M(written.contains(QStringLiteral("capacitance")),
			"the shared value did not carry across the move: " + moved.attributes);
	}
};

TEST_INSTANTIATE(TST_AttributeFormGui);
