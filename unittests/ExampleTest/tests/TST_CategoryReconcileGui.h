#pragma once

#include "UnitTest.h"
#include "UnitTest_Gui.h"

#include "controllers/PartManager_PartEditorController.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "ui/PartManager_CategoryReconcileDialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <filesystem>

// The tail an import leaves behind: local categories nothing incoming matched, and the two things
// the user may do about one of them. Both are optional — that is the point of the screen — so what
// is checked here is that neither costs more than it should. Re-filing a category whose parts all
// move cleanly must not put a dialog in front of each part, and "keep as it is" must write nothing
// at all rather than quietly normalising something.
//
// The §11 half of the dialog re-reads the database rather than a plan, so it needs no fixture of
// its own here; TST_PartTypeTransfer already covers who goes incomplete and why.
class TST_CategoryReconcileGui : public UnitTest::Test
{
	TEST_CLASS(TST_CategoryReconcileGui)
public:
	TST_CategoryReconcileGui()
		: Test("TST_CategoryReconcileGui")
	{
		ADD_TEST(TST_CategoryReconcileGui::aCleanMoveNeverAsks);
		ADD_TEST(TST_CategoryReconcileGui::keepingACategoryWritesNothing);
	}

private:

	static std::unique_ptr<PartManager::DatabaseHandle> makeDatabase(const std::string& name,
		std::string& outError)
	{
		const std::filesystem::path parent =
			std::filesystem::temp_directory_path() / ("PartManager_TST_CategoryReconcileGui_" + name);
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);
		return PartManager::DatabaseHandle::createNew(parent.string(), "Reconcile", outError);
	}

	// Two attribute-free root categories and one part in the first. Attribute-free on purpose: with
	// nothing to carry over and nothing required, every move out of `outSource` is a clean one.
	static bool buildFixture(PartManager::DatabaseHandle* handle, int& outSource, int& outTarget,
		int& outPartId)
	{
		PartManager::PartEditorController editor(handle);

		PartManager::PartType source;
		source.name = "Left Over";
		source.domain = "generic";
		outSource = editor.createType(source);

		PartManager::PartType target;
		target.name = "Where It Belongs";
		target.domain = "generic";
		outTarget = editor.createType(target);

		PartManager::Part part;
		part.partTypeId = outSource;
		part.name = "the one part";
		part.attributes = "{}";
		outPartId = editor.createPart(part);

		return outSource != PartManager::NoParentType && outTarget != PartManager::NoParentType
			&& outPartId != 0;
	}

	static int typeIdOfPart(PartManager::DatabaseHandle* handle, int partId)
	{
		PartManager::Part part;
		if (!PartManager::PartRepository::findPart(handle->connection(), partId, part))
		{
			return PartManager::NoParentType;
		}
		return part.partTypeId;
	}

	// Picks the first unmatched row, which is what both buttons act on.
	static QTreeWidgetItem* selectFirstUnmatched(PartManager::CategoryReconcileDialog& dialog)
	{
		QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("unmatchedTree", &dialog);
		if (tree == nullptr || tree->topLevelItemCount() == 0)
		{
			return nullptr;
		}
		tree->setCurrentItem(tree->topLevelItem(0));
		return tree->topLevelItem(0);
	}

	// Mapping a category is one picker followed by however many parts need asking about. A part
	// that loses nothing needs no asking — forty clean parts must not cost forty dialogs, which is
	// the whole reason MovePartDialog::isCleanMove() is static and callable without showing it.
	TEST_FUNCTION(aCleanMoveNeverAsks)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("cleanmove", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		int sourceId = 0;
		int targetId = 0;
		int partId = 0;
		TEST_ASSERT_M(buildFixture(handle.get(), sourceId, targetId, partId),
			"could not build the two categories and the part");

		PartManager::CategoryReconcileDialog dialog(handle.get(),
			std::vector<int>(1, sourceId));
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QPushButton* mapButton = UnitTest::Gui::find<QPushButton>("mapButton", &dialog);
		TEST_ASSERT_M(mapButton != nullptr, "the dialog has no map button:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());
		QTreeWidgetItem* row = selectFirstUnmatched(dialog);
		TEST_ASSERT_M(row != nullptr, "the unmatched category is not in the tree");
		TEST_ASSERT_M(mapButton->isEnabled(), "the map button must follow the selection");

		// Watching for the dialog that must not appear. It is armed *first* so it is polling before
		// the picker's exec() blocks — and it would rescue the run from hanging forever if the move
		// did turn out to need asking.
		bool wasAsked = false;
		UnitTest::Gui::onNextWindowOfClass(QStringLiteral("PartManager::MovePartDialog"),
			[&wasAsked](QWidget* window)
			{
				wasAsked = true;
				QDialog* asking = qobject_cast<QDialog*>(window);
				if (asking != nullptr)
				{
					asking->reject();
				}
			}, 3000);

		// The picker is the existing one, driven the way a user drives it: select the target in the
		// tree, then confirm. Selecting is what sets the answer; accept() is the OK button.
		QString pickerTrace = QStringLiteral("the picker never opened");
		UnitTest::Gui::onNextWindowOfClass(QStringLiteral("PartManager::PartTypePickerDialog"),
			[&pickerTrace](QWidget* window)
			{
				pickerTrace = QStringLiteral("opened, but its tree is not called typeTree");
				QTreeWidget* tree = UnitTest::Gui::find<QTreeWidget>("typeTree", window);
				QDialogButtonBox* box = UnitTest::Gui::find<QDialogButtonBox>("buttonBox", window);
				QDialog* picker = qobject_cast<QDialog*>(window);
				if (tree == nullptr || box == nullptr || picker == nullptr)
				{
					if (picker != nullptr)
					{
						picker->reject();
					}
					return;
				}
				pickerTrace = QStringLiteral("opened, but the target category is not in it:\n")
					+ UnitTest::Gui::dumpWidgetTree(window);
				QTreeWidgetItem* item = UnitTest::Gui::findTreeItem(tree, QStringList()
					<< QStringLiteral("Where It Belongs"));
				if (item == nullptr)
				{
					picker->reject();
					return;
				}
				// setCurrentItem rather than a synthetic click: the row's pixel rectangle is not
				// settled the instant the picker becomes visible, and the picker reads its answer
				// off itemSelectionChanged either way.
				tree->setCurrentItem(item);
				// OK, not accept(): the picker reads the tree's current item in its OK handler, so
				// closing it any other way would come back with "no category" — which is exactly the
				// answer that must not be mistaken for a choice.
				QPushButton* ok = box->button(QDialogButtonBox::Ok);
				if (ok == nullptr)
				{
					picker->reject();
					return;
				}
				pickerTrace = QStringLiteral("picked");
				ok->click();
			}, 3000);

		mapButton->click();

		TEST_ASSERT_M(!wasAsked,
			"a part that loses nothing by moving must be re-filed without a dialog");
		TEST_ASSERT_M(typeIdOfPart(handle.get(), partId) == targetId,
			"the part was not re-filed — " + pickerTrace.toStdString());
		TEST_COMPARE(dialog.movedPartCount(), 1);
		// The row says what happened to it — the list is the record of the decisions taken, and a
		// row that looks untouched after being emptied would be read as still open.
		TEST_ASSERT_M(!row->text(2).isEmpty(), "a mapped row must say so");
		TEST_COMPARE(row->text(1), QStringLiteral("0"));

		UnitTest::Gui::closeWindow(&dialog);
	}

	// "Keep as it is" is an answer, not a skip: it is on the screen with the same weight as mapping
	// because leaving a category alone is very often the right call. What it must never do is
	// touch anything — the only thing that changes is that the row stops asking.
	TEST_FUNCTION(keepingACategoryWritesNothing)
	{
		TEST_START;

		TEST_ASSERT(UnitTest::Gui::ensureApplication());

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle = makeDatabase("keep", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		int sourceId = 0;
		int targetId = 0;
		int partId = 0;
		TEST_ASSERT_M(buildFixture(handle.get(), sourceId, targetId, partId),
			"could not build the two categories and the part");

		PartManager::CategoryReconcileDialog dialog(handle.get(),
			std::vector<int>(1, sourceId));
		TEST_ASSERT(UnitTest::Gui::showAndWait(&dialog));

		QPushButton* keepButton = UnitTest::Gui::find<QPushButton>("keepButton", &dialog);
		TEST_ASSERT_M(keepButton != nullptr, "the dialog has no keep button:\n"
			+ UnitTest::Gui::dumpWidgetTree(&dialog).toStdString());
		QTreeWidgetItem* row = selectFirstUnmatched(dialog);
		TEST_ASSERT_M(row != nullptr, "the unmatched category is not in the tree");
		TEST_ASSERT_M(keepButton->isEnabled(), "the keep button must follow the selection");

		const size_t typesBefore =
			PartManager::PartTypeRepository::listTypes(handle->connection()).size();

		keepButton->click();

		TEST_COMPARE(typeIdOfPart(handle.get(), partId), sourceId);
		TEST_COMPARE(dialog.movedPartCount(), 0);
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(handle->connection()).size(),
			typesBefore);
		TEST_ASSERT_M(!row->text(2).isEmpty(), "a kept row must say it was looked at");

		UnitTest::Gui::closeWindow(&dialog);
	}
};

TEST_INSTANTIATE(TST_CategoryReconcileGui);
