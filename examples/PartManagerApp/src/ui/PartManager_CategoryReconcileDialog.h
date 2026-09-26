// @file PartManager_CategoryReconcileDialog.h
// @brief The two loose ends a category import leaves behind (§2b, §11) — both optional.
//
// An import answers "what should this database's categories look like". It does
// not answer the two questions that only become askable afterwards:
//
//  1. **Local categories nothing matched.** A category that exists here and not
//     in the source is never touched by a merge — matching is by path (§2b) and
//     an absent path is not a deletion. Often that is exactly right. Sometimes it
//     means the two databases spelled the same idea differently, and the local
//     one now sits beside its imported twin. Tab one lets the user point it at
//     another category and move its parts across, part by part, through the
//     existing MovePartDialog — which is the only code that knows what a move
//     costs. Nothing is done unless asked: "keep it as it is" is a real answer
//     and the default one.
//
//  2. **Parts a merge left incomplete (§11).** An incoming category can declare
//     an attribute *required* that the local one did not, and every part already
//     filed there is then missing a value it is supposed to have. The merge does
//     not refuse and does not invent a value; it lets the parts through and the
//     gap shows up here. The list is re-read from the database every time, never
//     taken from the plan: the plan describes an import that has since happened,
//     parts may have been edited in between, and a stale list would send the user
//     to fill in a field that is already filled.
//
// Neither tab is a gate. The dialog can be closed at any point, both lists half
// done, and re-opened later from the type template editor — which is why it takes
// its unmatched ids as a parameter rather than a MergePlan: reopened cold, there
// is no plan any more, and tab two still works because it never needed one.
// @see docs/design/ARCHITECTURE.md §2, §2b, §11, §12b
// @see PartManager_CategoryImportDialog.h, PartManager_MovePartDialog.h, PartManager_PartTypePickerDialog.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include "database/PartManager_DatabaseHandle.h"
#include <QDialog>
#include <QString>
#include <vector>

class QTreeWidgetItem;

namespace Ui { class CategoryReconcileDialog; }

namespace PartManager
{

	class CategoryReconcileDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `unmatchedLocalTypeIds` comes straight from the MergePlan that was applied. Empty is a
		// perfectly good value — reopened later from the type template editor there is no plan to
		// take it from, and tab two is then the whole dialog.
		CategoryReconcileDialog(DatabaseHandle* handle,
			const std::vector<int>& unmatchedLocalTypeIds, QWidget* parent = nullptr);
		~CategoryReconcileDialog() override;

		// How many parts the user has moved out of unmatched categories in this session, and how
		// many §11 gaps are still open. Both for the caller's status line and for tests.
		int movedPartCount() const;
		int incompletePartCount() const;

	private slots:
		// Opens the existing type picker on the selected unmatched category and, if a target comes
		// back, walks its parts into it.
		void mapSelectedCategory();
		// Marks the selected row settled without touching anything — the explicit no-op, so that
		// "I looked at this and it is fine" and "I have not looked yet" are different states.
		void keepSelectedCategory();
		// Opens the part editor on the selected incomplete part, then re-scans.
		void editSelectedPart();
		// Re-reads the §11 list from the database. See the header for why it is never cached.
		void refreshIncompleteParts();

	private:
		// Fills tab one from the ids handed in, skipping any that no longer exist.
		void fillUnmatchedCategories();
		// The type id on the selected row of tab one, NoParentType when nothing is selected.
		int selectedUnmatchedTypeId() const;
		// Moves every part of `sourceTypeId` into `targetTypeId`, driving MovePartDialog only for
		// the parts that actually lose something (isCleanMove) — a clean part must not cost a
		// dialog, and forty clean parts must not cost forty. Returns how many moved; `outStopped`
		// says the user cancelled out of one of the dialogs, which stops the rest.
		int movePartsOfType(int sourceTypeId, int targetTypeId, bool& outStopped);
		// Both tabs' "how much is left" lines, and the progress bar tab two shrinks.
		void updateProgress();

		Ui::CategoryReconcileDialog* m_ui;
		DatabaseHandle* m_handle;
		PartEditorController m_editor;
		std::vector<int> m_unmatchedTypeIds;
		std::vector<PartType> m_types;
		// How many parts were incomplete when this dialog first looked. The progress bar measures
		// against that rather than against a moving total, so fixing one part always moves it
		// forward — and creating an unrelated incomplete part meanwhile does not move it back.
		int m_initialIncompleteCount = 0;
		int m_incompleteCount = 0;
		int m_movedPartCount = 0;
	};

}
