#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "controllers/PartManager_PartEditorController.h"
#include "import/PartManager_PartTypeTransfer.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "ui/PartManager_CategoryExportDialog.h"
#include "ui/PartManager_CategoryImportDialog.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <filesystem>
#include <fstream>
#include <map>

// The two screens of the §2b category transfer, at the seam between the widget-free planning in
// core/import and the wizard on top of it. planMerge()/applyMerge() have their own suite
// (TST_PartTypeTransfer) and none of their rules are re-checked here; what is checked is that the
// tree enforces §2b's "a child is not importable without its parent", that the wizard refuses the
// one source that can never work, that the controls on the plan page really are the plan rather
// than a picture of it, and — the one that matters most — that walking the wizard as far as the
// confirm page writes absolutely nothing.
//
// No network anywhere: a bundle is a file, and the second database is made in %TEMP% like the first.
class TST_CategoryTransferGui : public UnitTest::Test
{
	TEST_CLASS(TST_CategoryTransferGui)
public:
	TST_CategoryTransferGui()
		: Test("TST_CategoryTransferGui")
	{
		ADD_TEST(TST_CategoryTransferGui::tickingAChildTicksTheAncestorsItNeeds);
		ADD_TEST(TST_CategoryTransferGui::theOpenDatabaseIsRefusedAsASource);
		ADD_TEST(TST_CategoryTransferGui::aPlanRowIsTheDecisionAndNotAPictureOfIt);
		ADD_TEST(TST_CategoryTransferGui::theWizardWritesNothingBeforeApply);
	}

private:

	// A fresh seeded database for one test. Null on failure, with the reason in outError.
	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_CategoryTransferGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "Categories", outError);
	}

	// The id of the seeded type with this exact name, 0 when the seed no longer carries it.
	static int typeIdNamed(PartManager::DatabaseHandle* handle, const std::string& name)
	{
		for (const PartManager::PartType& type :
			PartManager::PartTypeRepository::listTypes(handle->connection()))
		{
			if (type.name == name)
			{
				return type.id;
			}
		}
		return PartManager::NoParentType;
	}

	// Builds the source side of an import: the "Resistor" the seed already put in both databases,
	// but with a different description and one extra *required* attribute. That is deliberately the
	// awkward case — a field conflict and a §11 consequence on the same category.
	// Returns the path of the written `.pmcat`, empty on failure.
	static QString writeBundleFile(const std::string& name, std::string& outError)
	{
		std::unique_ptr<PartManager::DatabaseHandle> source = makeDatabase(name + "_source", outError);
		if (source == nullptr)
		{
			return QString();
		}
		const int resistorId = typeIdNamed(source.get(), "Resistor");
		if (resistorId == PartManager::NoParentType)
		{
			outError = "the seeded database has no Resistor type";
			return QString();
		}

		PartManager::PartType resistor;
		PartManager::PartTypeRepository::findType(source->connection(), resistorId, resistor);
		resistor.description = "the other database's own words";
		PartManager::PartTypeRepository::updateType(source->connection(), resistor);

		PartManager::PartTypeAttribute band;
		band.partTypeId = resistorId;
		band.key = "tolerance_band";
		band.label = "Tolerance band";
		band.datatype = PartManager::AttributeDataType::Text;
		band.required = true;
		if (PartManager::PartTypeRepository::insertAttribute(source->connection(), band) == 0)
		{
			outError = "insertAttribute failed on the source database";
			return QString();
		}

		const PartManager::PartTypeBundle bundle =
			PartManager::bundleFromDatabase(source->connection(), std::vector<int>(1, resistorId));
		const std::filesystem::path path =
			std::filesystem::temp_directory_path()
			/ ("PartManager_TST_CategoryTransferGui_" + name + ".pmcat");
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			outError = "could not write " + path.string();
			return QString();
        }
		const std::string json = PartManager::toJson(bundle);
		file.write(json.data(), static_cast<std::streamsize>(json.size()));
		file.close();
		return QString::fromStdString(path.string());
	}

	// Points the wizard's source page at a bundle file. The path field is read-only for the user —
	// only the file dialog fills it — so the radio pair is toggled to make reloadSource() run,
	// which is the same door browseForBundleFile() goes through.
	static bool selectBundleFile(PartManager::CategoryImportDialog& dialog, const QString& path)
	{
		QLineEdit* fileEdit = UnitTest::Gui::find<QLineEdit>("fileEdit", &dialog);
		QRadioButton* fileRadio = UnitTest::Gui::find<QRadioButton>("fileRadio", &dialog);
		QRadioButton* databaseRadio = UnitTest::Gui::find<QRadioButton>("databaseRadio", &dialog);
		if (fileEdit == nullptr || fileRadio == nullptr || databaseRadio == nullptr)
		{
			return false;
		}
		fileEdit->setText(path);
		databaseRadio->setChecked(true);
		fileRadio->setChecked(true);
		return true;
	}

	// The index of the plan entry for one incoming path, -1 when the plan has no such row. A path
	// is a list of segments, so the caller names the segments.
	static int entryIndexForPath(const PartManager::MergePlan& plan,
		const std::vector<std::string>& path)
	{
		for (size_t index = 0; index < plan.entries.size(); ++index)
		{
			if (plan.entries[index].path == path)
			{
				return static_cast<int>(index);
			}
		}
		return -1;
	}

	// Everything an import is allowed to change, in one comparable value: how many categories there
	// are, and for every part, which category it is in and what it holds.
	struct DatabaseShape
	{
		size_t typeCount = 0;
		std::map<int, std::pair<int, std::string>> parts;   // id -> (partTypeId, attributes)

		bool operator==(const DatabaseShape& other) const
		{
			return typeCount == other.typeCount && parts == other.parts;
		}
	};

	static DatabaseShape shapeOf(PartManager::DatabaseHandle* handle)
	{
		DatabaseShape shape;
		shape.typeCount = PartManager::PartTypeRepository::listTypes(handle->connection()).size();
		for (const PartManager::Part& part : PartManager::PartRepository::listParts(handle->connection()))
		{
			shape.parts[part.id] = std::make_pair(part.partTypeId, part.attributes);
		}
		return shape;
	}

	// §2b: a subtype means what it means because of what it hangs under, so a bundle carrying a
	// child without its parent is not importable at all. The export tree enforces that on the way
	// in rather than letting the import refuse it later.
	TEST_FUNCTION(tickingAChildTicksTheAncestorsItNeeds)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("export", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		// Two children, so a single ticked child leaves the parent genuinely partial — "checked"
		// would not tell a cascade apart from a half-selection.
		PartManager::PartEditorController editor(handle.get());
		PartManager::PartType parent;
		parent.name = "Bracketry";
		parent.domain = "mechanical";
		parent.id = editor.createType(parent);
		TEST_ASSERT_M(parent.id != PartManager::NoParentType, "createType failed for the parent");

		PartManager::PartType childA;
		childA.name = "Angle Bracket";
		childA.parentTypeId = parent.id;
		TEST_ASSERT_M(editor.createType(childA) != PartManager::NoParentType, "createType failed");
		PartManager::PartType childB;
		childB.name = "Flat Bracket";
		childB.parentTypeId = parent.id;
		TEST_ASSERT_M(editor.createType(childB) != PartManager::NoParentType, "createType failed");

		PartManager::CategoryExportDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("categoryTree", &dialog);
		QPushButton* selectNone = UnitTest::Gui::find<QPushButton>("selectNoneButton", &dialog);
		TEST_ASSERT_M(tree && selectNone,
			"the export dialog no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		selectNone->click();
		QTreeWidgetItem* parentItem = UnitTest::Gui::findTreeItem(tree, QStringList()
			<< QStringLiteral("Bracketry"));
		QTreeWidgetItem* childItem = UnitTest::Gui::findTreeItem(tree, QStringList()
			<< QStringLiteral("Bracketry") << QStringLiteral("Angle Bracket"));
		TEST_ASSERT_M(parentItem && childItem, "the category forest is not in the tree:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());
		TEST_COMPARE(static_cast<int>(parentItem->checkState(0)), static_cast<int>(Qt::Unchecked));

		childItem->setCheckState(0, Qt::Checked);

		TEST_COMPARE(static_cast<int>(parentItem->checkState(0)),
			static_cast<int>(Qt::PartiallyChecked));
		// And the tick is not cosmetic: the parent has to be in the file the child goes into.
		const std::vector<int> selected = dialog.selectedTypeIds();
		TEST_ASSERT_M(std::find(selected.begin(), selected.end(), parent.id) != selected.end(),
			"a ticked child must drag its parent into the export (ARCHITECTURE.md §2b)");

		UnitTest::Gui::closeWindow(&dialog);
	}

	// Importing a database into itself is not a merge that happens to do nothing — planMerge()
	// cannot even express it, since every incoming category would match itself. The page says so
	// instead of producing an empty plan the user would have to interpret.
	TEST_FUNCTION(theOpenDatabaseIsRefusedAsASource)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("selfimport", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::CategoryImportDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QComboBox* databaseCombo = UnitTest::Gui::find<QComboBox>("databaseCombo", &dialog);
		QRadioButton* databaseRadio = UnitTest::Gui::find<QRadioButton>("databaseRadio", &dialog);
		QLabel* errorLabel = UnitTest::Gui::find<QLabel>("sourceErrorLabel", &dialog);
		QPushButton* next = UnitTest::Gui::find<QPushButton>("nextButton", &dialog);
		TEST_ASSERT_M(databaseCombo && databaseRadio && errorLabel && next,
			"the import wizard no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		// The drop-down is filled from the §1b registry, which this throwaway database is not in —
		// and it leaves the open database out anyway. Putting it in by hand is what gets the
		// *second* guard under test: the one that refuses the choice rather than hiding it.
		databaseRadio->setChecked(true);
		databaseCombo->addItem(QStringLiteral("itself"),
			QString::fromStdString(handle->pmdbPath()));
		databaseCombo->setCurrentIndex(databaseCombo->count() - 1);

		TEST_ASSERT_M(!errorLabel->text().isEmpty(),
			"picking the open database must be refused in words, not silently ignored");
		TEST_ASSERT_M(!next->isEnabled(),
			"Next must stay dead while the source is the database being written into");

		UnitTest::Gui::closeWindow(&dialog);
	}

	// The controls on the plan page write straight into the MergePlan applyMerge() is handed —
	// there is no apply-time translation step where the screen and the plan could disagree. Checked
	// through the dialog's own plan(), which is the same object.
	TEST_FUNCTION(aPlanRowIsTheDecisionAndNotAPictureOfIt)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("plan", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		const int localResistor = typeIdNamed(handle.get(), "Resistor");
		TEST_ASSERT_M(localResistor != PartManager::NoParentType, "no seeded Resistor type");

		// Two parts filed under the category the incoming one adds a required field to. This is the
		// number the row has to name: §11's consequence is counted in parts, not in categories.
		PartManager::PartEditorController editor(handle.get());
		for (int i = 0; i < 2; ++i)
		{
			PartManager::Part part;
			part.partTypeId = localResistor;
			part.name = "Resistor " + std::to_string(i);
			TEST_ASSERT_M(editor.createPart(part) != 0, "createPart failed");
		}
		// A value to disagree about. Without this the two descriptions are the seed's own and match.
		PartManager::PartType local;
		PartManager::PartTypeRepository::findType(handle->connection(), localResistor, local);
		local.description = "this database's own words";
		PartManager::PartTypeRepository::updateType(handle->connection(), local);

		const QString bundlePath = writeBundleFile("plan", error);
		TEST_ASSERT_M(!bundlePath.isEmpty(), "could not build the bundle: " + error);

		PartManager::CategoryImportDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));
		TEST_ASSERT_M(selectBundleFile(dialog, bundlePath),
			"the source page no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		QPushButton* next = UnitTest::Gui::find<QPushButton>("nextButton", &dialog);
		QTreeWidget* planTree = UnitTest::Gui::find<QTreeWidget>("planTree", &dialog);
		TEST_ASSERT_M(next && planTree, "the plan page is gone");
		TEST_ASSERT_M(next->isEnabled(), "the bundle was not read: "
			+ UnitTest::Gui::find<QLabel>("sourceErrorLabel", &dialog)->text().toStdString());
		next->click();

		const int entry = entryIndexForPath(dialog.plan(), { "Resistor" });
		TEST_ASSERT_M(entry >= 0, "the plan has no row for the incoming Resistor");
		TEST_ASSERT_M(dialog.plan().entries[entry].action == PartManager::TypeMergeAction::MergeInto,
			"a category that exists on both sides must default to being merged");

		// §11 on the row, with the count: "two parts here go incomplete" is the consequence that
		// outlives the import, so it has to be readable before OK and not discovered afterwards.
		QTreeWidgetItem* row = UnitTest::Gui::findTreeItem(planTree, QStringList()
			<< QStringLiteral("Resistor"));
		TEST_ASSERT_M(row != nullptr, "the plan tree has no Resistor row");
		TEST_ASSERT_M(!dialog.plan().entries[entry].newRequiredAttributeKeys.empty(),
			"the incoming Resistor was supposed to carry a new required attribute");
		TEST_COMPARE(dialog.plan().entries[entry].affectedPartCount, 2);
		const QString summary = row->text(2);
		TEST_ASSERT_M(summary.contains(QStringLiteral("2")),
			"the row must say how many parts go incomplete: " + summary.toStdString());
		TEST_ASSERT_M(summary.contains(QStringLiteral("tolerance_band")),
			"the row must name the attribute that did it: " + summary.toStdString());

		// The conflict control. Whichever way core defaulted it, the *other* way is what has to be
		// seen landing — picking the value that is already there would prove nothing.
		TEST_ASSERT_M(!dialog.plan().entries[entry].conflicts.empty(),
			"the two descriptions were supposed to disagree");
		QComboBox* conflict = UnitTest::Gui::find<QComboBox>(
			QStringLiteral("conflictCombo%1_0").arg(entry), &dialog);
		TEST_ASSERT_M(conflict != nullptr, "the field conflict has no resolution control:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());
		const bool wasKeepLocal = dialog.plan().entries[entry].conflicts[0].resolution
			== PartManager::FieldResolution::KeepLocal;
		const PartManager::FieldResolution wanted = wasKeepLocal
			? PartManager::FieldResolution::TakeIncoming
			: PartManager::FieldResolution::KeepLocal;
		TEST_ASSERT_M(UnitTest::Gui::selectComboText(conflict, wasKeepLocal
			? PartManager::CategoryImportDialog::tr("Take incoming")
			: PartManager::CategoryImportDialog::tr("Keep local")),
			"the other resolution is not on offer");
		TEST_ASSERT_M(dialog.plan().entries[entry].conflicts[0].resolution == wanted,
			"the conflict combo did not write through to the plan");

		// The action control, the same way.
		QComboBox* action = UnitTest::Gui::find<QComboBox>(
			QStringLiteral("actionCombo%1").arg(entry), &dialog);
		TEST_ASSERT_M(action != nullptr, "the entry row has no action control");
		TEST_ASSERT_M(UnitTest::Gui::selectComboText(action,
			PartManager::CategoryImportDialog::tr("Skip")), "Skip is not on offer");
		TEST_ASSERT_M(dialog.plan().entries[entry].action == PartManager::TypeMergeAction::Skip,
			"the action combo did not write through to the plan");
		// A skipped row makes nobody incomplete, so the §11 sentence has to go with it.
		TEST_ASSERT_M(!row->text(2).contains(QStringLiteral("tolerance_band")),
			"a skipped row must not keep warning about a consequence it no longer has: "
			+ row->text(2).toStdString());

		// The wizard is mid-plan and nothing was applied, so closing asks the §10 question.
		UnitTest::Gui::onNextMessageBox("Yes");
		UnitTest::Gui::closeWindow(&dialog);
	}

	// The promise the whole wizard rests on: reviewing is free. Everything up to and including the
	// confirm page reads and plans; only Apply writes. Asserted on the two things an import could
	// possibly touch — the `part_type` rows, and every part's category and attributes.
	TEST_FUNCTION(theWizardWritesNothingBeforeApply)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("dryrun", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		const int localResistor = typeIdNamed(handle.get(), "Resistor");
		TEST_ASSERT_M(localResistor != PartManager::NoParentType, "no seeded Resistor type");

		PartManager::PartEditorController editor(handle.get());
		PartManager::Part part;
		part.partTypeId = localResistor;
		part.name = "4k7 0603";
		part.attributes = "{}";
		TEST_ASSERT_M(editor.createPart(part) != 0, "createPart failed");

		const QString bundlePath = writeBundleFile("dryrun", error);
		TEST_ASSERT_M(!bundlePath.isEmpty(), "could not build the bundle: " + error);

		const DatabaseShape before = shapeOf(handle.get());
		TEST_ASSERT_M(!before.parts.empty(), "the fixture wrote no parts to watch");

		PartManager::CategoryImportDialog dialog(handle.get());
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));
		TEST_ASSERT_M(selectBundleFile(dialog, bundlePath), "the source page is gone");

		QPushButton* next = UnitTest::Gui::find<QPushButton>("nextButton", &dialog);
		QStackedWidget* stack = UnitTest::Gui::find<QStackedWidget>("stack", &dialog);
		TEST_ASSERT_M(next && stack, "the wizard no longer has a footer and a stack");
		TEST_ASSERT_M(next->isEnabled(), "the bundle was not read");

		next->click();                        // source -> plan (this is where planMerge() runs)
		TEST_COMPARE(stack->currentIndex(), 1);
		TEST_ASSERT_M(shapeOf(handle.get()) == before,
			"planning the merge wrote to the database");

		next->click();                        // plan -> confirm
		TEST_COMPARE(stack->currentIndex(), 2);
		// The last page before Apply. Its button says "Apply" precisely because nothing has been
		// applied yet — if that is not still true here, the wizard's whole §10 promise is broken.
		TEST_COMPARE(next->text(), PartManager::CategoryImportDialog::tr("Apply"));
		TEST_ASSERT_M(!dialog.applied(), "the wizard thinks it already applied");
		TEST_ASSERT_M(shapeOf(handle.get()) == before,
			"reaching the confirm page wrote to the database");

		UnitTest::Gui::onNextMessageBox("Yes");
		UnitTest::Gui::closeWindow(&dialog);

		TEST_ASSERT_M(shapeOf(handle.get()) == before,
			"cancelling out of the wizard left something behind");
	}
};

TEST_INSTANTIATE(TST_CategoryTransferGui);
