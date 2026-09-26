#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "persistence/PartManager_PartTypeRepository.h"
#include "ui/PartManager_PartTypePickerDialog.h"

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <filesystem>

// The §2b category tree as a picker. What is checked here is everything the old flat combo could
// not do and therefore had no test: that the tree really has the shape `parent_type_id` describes,
// that a preselected id survives a round trip, that "no category" is a first-class answer rather
// than a cancel, that filtering does not make a matching child unreachable by hiding its parent,
// and that the parent-type caller cannot build a cycle.
//
// Runs against a throwaway database in %TEMP%, never the user's own. Nothing here touches the
// network or Mouser.
class TST_PartTypePickerDialogGui : public UnitTest::Test
{
	TEST_CLASS(TST_PartTypePickerDialogGui)
public:
	TST_PartTypePickerDialogGui()
		: Test("TST_PartTypePickerDialogGui")
	{
		ADD_TEST(TST_PartTypePickerDialogGui::theTreeMirrorsTheParentChildStructure);
		ADD_TEST(TST_PartTypePickerDialogGui::aPreselectedTypeComesBackOut);
		ADD_TEST(TST_PartTypePickerDialogGui::noCategoryYieldsZero);
		ADD_TEST(TST_PartTypePickerDialogGui::theFilterKeepsAMatchingDescendantReachable);
		ADD_TEST(TST_PartTypePickerDialogGui::excludingASubtreeRulesOutEveryCycle);
		ADD_TEST(TST_PartTypePickerDialogGui::thePathSpellsTheWholeChain);
	}

private:

	// The three types every test below navigates, on top of whatever a fresh database seeds:
	// a root, its child and its grandchild, named so nothing a built-in template is called can
	// collide with them.
	struct Fixture
	{
		std::unique_ptr<PartManager::DatabaseHandle> handle;
		int rootId = 0;
		int childId = 0;
		int grandchildId = 0;
	};

	static Fixture makeFixture(const std::string& name, std::string& outError)
	{
		Fixture fixture;
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_PartTypePicker_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		fixture.handle = PartManager::DatabaseHandle::createNew(parent.string(), "Picker", outError);
		if (!fixture.handle || !fixture.handle->isOpen())
		{
			return fixture;
		}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		SQLiteWrapper::SQLite& db = fixture.handle->connection();
		PartManager::PartType root;
		root.name = "Zorb";
		fixture.rootId = PartManager::PartTypeRepository::insertType(db, root);

		PartManager::PartType child;
		child.name = "Zorb Flange";
		child.parentTypeId = fixture.rootId;
		fixture.childId = PartManager::PartTypeRepository::insertType(db, child);

		PartManager::PartType grandchild;
		grandchild.name = "Zorb Flange Washer";
		grandchild.parentTypeId = fixture.childId;
		fixture.grandchildId = PartManager::PartTypeRepository::insertType(db, grandchild);
#endif
		return fixture;
	}

	// The tree item carrying `typeId`, hidden or not. Null when the picker never built one, which
	// is exactly what the exclusion test wants to see.
	static QTreeWidgetItem* itemFor(QTreeWidget* tree, int typeId)
	{
		for (QTreeWidgetItemIterator it(tree); *it; ++it)
		{
			if ((*it)->data(0, Qt::UserRole).toInt() == typeId)
			{
				return *it;
			}
		}
		return nullptr;
	}

	TEST_FUNCTION(theTreeMirrorsTheParentChildStructure)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		Fixture fixture = makeFixture("structure", error);
		TEST_ASSERT_M(fixture.handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT_M(fixture.grandchildId != 0, "the fixture types were not inserted");

		PartManager::PartTypePickerDialog dialog(fixture.handle.get(), 0);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("typeTree", &dialog);
		TEST_ASSERT_M(tree != nullptr, "the picker no longer has a typeTree:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		QTreeWidgetItem* root = itemFor(tree, fixture.rootId);
		QTreeWidgetItem* child = itemFor(tree, fixture.childId);
		QTreeWidgetItem* grandchild = itemFor(tree, fixture.grandchildId);
		TEST_ASSERT_M(root && child && grandchild, "not every fixture type reached the tree");

		// The whole point of the dialog: the nesting is the database's, not an alphabetical list.
		TEST_ASSERT_M(root->parent() == nullptr, "a type with no parent must be a top-level item");
		TEST_ASSERT_M(child->parent() == root, "the child must hang under its parent type");
		TEST_ASSERT_M(grandchild->parent() == child, "the grandchild must hang under the child");
		TEST_COMPARE(child->text(0), QString("Zorb Flange"));

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(aPreselectedTypeComesBackOut)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		Fixture fixture = makeFixture("preselect", error);
		TEST_ASSERT_M(fixture.handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT_M(fixture.grandchildId != 0, "the fixture types were not inserted");

		PartManager::PartTypePickerDialog dialog(fixture.handle.get(), fixture.grandchildId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("typeTree", &dialog);
		QDialogButtonBox* buttons = UnitTest::Gui::find<QDialogButtonBox>("buttonBox", &dialog);
		TEST_ASSERT_M(tree && buttons, "the picker no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		QTreeWidgetItem* current = tree->currentItem();
		TEST_ASSERT_M(current != nullptr, "a preselected type must leave the tree with a selection");
		TEST_COMPARE(current->data(0, Qt::UserRole).toInt(), fixture.grandchildId);
		// A selection nobody can see is the same as no selection, so every ancestor has to be open.
		TEST_ASSERT_M(current->parent() != nullptr && current->parent()->isExpanded(),
			"the preselected type's parent was left collapsed");
		TEST_ASSERT_M(current->parent()->parent() != nullptr
			&& current->parent()->parent()->isExpanded(),
			"the preselected type's grandparent was left collapsed");

		QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
		TEST_ASSERT_M(ok != nullptr && ok->isEnabled(),
			"OK must be enabled while a real type is selected");
		TEST_ASSERT(UnitTest::Gui::click(ok));
		TEST_COMPARE(dialog.selectedTypeId(), fixture.grandchildId);
	}

	TEST_FUNCTION(noCategoryYieldsZero)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		Fixture fixture = makeFixture("nocategory", error);
		TEST_ASSERT_M(fixture.handle != nullptr, "createNew failed: " + error);

		PartManager::PartTypePickerDialog dialog(fixture.handle.get(), fixture.childId);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QDialogButtonBox* buttons = UnitTest::Gui::find<QDialogButtonBox>("buttonBox", &dialog);
		TEST_ASSERT_M(buttons != nullptr, "the picker no longer has a buttonBox");

		// Nothing is selected is a *cancel*; "No category" is an answer. New Part depends on being
		// able to go back to "(none — select a category)" on purpose, so this button has to accept.
		QPushButton* noCategory = nullptr;
		for (QAbstractButton* button : buttons->buttons())
		{
			if (buttons->buttonRole(button) == QDialogButtonBox::ResetRole)
			{
				noCategory = qobject_cast<QPushButton*>(button);
			}
		}
		TEST_ASSERT_M(noCategory != nullptr, "the picker offers no way to choose no category at all");
		TEST_ASSERT(UnitTest::Gui::click(noCategory));

		TEST_COMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
		TEST_COMPARE(dialog.selectedTypeId(), 0);
	}

	TEST_FUNCTION(theFilterKeepsAMatchingDescendantReachable)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		Fixture fixture = makeFixture("filter", error);
		TEST_ASSERT_M(fixture.handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT_M(fixture.grandchildId != 0, "the fixture types were not inserted");

		PartManager::PartTypePickerDialog dialog(fixture.handle.get(), 0);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QLineEdit* filter = UnitTest::Gui::find<QLineEdit>("filterEdit", &dialog);
		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("typeTree", &dialog);
		TEST_ASSERT_M(filter && tree, "the picker no longer has the widgets this test drives:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());

		filter->setText(QStringLiteral("Washer"));

		QTreeWidgetItem* root = itemFor(tree, fixture.rootId);
		QTreeWidgetItem* child = itemFor(tree, fixture.childId);
		QTreeWidgetItem* grandchild = itemFor(tree, fixture.grandchildId);
		TEST_ASSERT_M(root && child && grandchild, "filtering must hide items, never delete them");

		// Only the grandchild's own name matches. Hiding its ancestors because *they* do not match
		// would hide it too — a filter that removes the one hit is the bug this guards.
		TEST_ASSERT_M(!grandchild->isHidden(), "the matching type was hidden by its own filter");
		TEST_ASSERT_M(!child->isHidden(), "an ancestor of a match must stay visible");
		TEST_ASSERT_M(!root->isHidden(), "the root above a match must stay visible");
		TEST_ASSERT_M(child->isExpanded(), "a branch kept alive by a descendant must be opened");

		// And a branch with no hit anywhere under it goes. The seeded Resistor template is one.
		bool sawAHiddenBranch = false;
		for (int i = 0; i < tree->topLevelItemCount(); ++i)
		{
			sawAHiddenBranch = sawAHiddenBranch || tree->topLevelItem(i)->isHidden();
		}
		TEST_ASSERT_M(sawAHiddenBranch, "the filter narrowed nothing at all");

		// Clearing it puts everything back, or the filter is a one-way door.
		filter->clear();
		TEST_ASSERT_M(!root->isHidden() && !child->isHidden() && !grandchild->isHidden(),
			"clearing the filter must restore the whole tree");

		UnitTest::Gui::closeWindow(&dialog);
	}

	TEST_FUNCTION(excludingASubtreeRulesOutEveryCycle)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		Fixture fixture = makeFixture("exclude", error);
		TEST_ASSERT_M(fixture.handle != nullptr, "createNew failed: " + error);
		TEST_ASSERT_M(fixture.grandchildId != 0, "the fixture types were not inserted");

		// What the type template editor's parent field asks for: a parent for "Zorb Flange".
		// Offering the type itself, or the washer below it, would make the §2b walk run in a circle.
		PartManager::PartTypePickerDialog dialog(fixture.handle.get(), 0, fixture.childId, nullptr);
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("typeTree", &dialog);
		TEST_ASSERT_M(tree != nullptr, "the picker no longer has a typeTree");

		TEST_ASSERT_M(itemFor(tree, fixture.childId) == nullptr,
			"a type must not be offered as its own parent");
		TEST_ASSERT_M(itemFor(tree, fixture.grandchildId) == nullptr,
			"a descendant must not be offered as a parent — that is a cycle");
		TEST_ASSERT_M(itemFor(tree, fixture.rootId) != nullptr,
			"excluding a subtree must not take the rest of the branch with it");

		UnitTest::Gui::closeWindow(&dialog);
	}

	// The label the New Part button and the parent-type button both wear. No widgets involved —
	// this is the function that turns an id into the inherited context the old combo threw away.
	TEST_FUNCTION(thePathSpellsTheWholeChain)
	{
		TEST_START;

		std::vector<PartManager::PartType> types;
		PartManager::PartType capacitor;
		capacitor.id = 1;
		capacitor.name = "Capacitor";
		types.push_back(capacitor);
		PartManager::PartType ceramic;
		ceramic.id = 2;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = 1;
		types.push_back(ceramic);

		TEST_COMPARE(PartManager::partTypePath(types, 1), QString("Capacitor"));
		TEST_COMPARE(PartManager::partTypePath(types, 2),
			QString::fromUtf8("Capacitor \xE2\x80\xBA Ceramic Capacitor"));
		TEST_ASSERT_M(PartManager::partTypePath(types, 0).isEmpty(),
			"no type at all has no path");
		TEST_ASSERT_M(PartManager::partTypePath(types, 99).isEmpty(),
			"an id this database does not have must not invent a name");

		// A broken parent chain must stop rather than hang the dialog that shows it.
		types[0].parentTypeId = 2;
		TEST_ASSERT_M(!PartManager::partTypePath(types, 2).isEmpty(),
			"a parent cycle must still produce something to show");
	}
};

TEST_INSTANTIATE(TST_PartTypePickerDialogGui);
