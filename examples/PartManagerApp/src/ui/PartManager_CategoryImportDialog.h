// @file PartManager_CategoryImportDialog.h
// @brief The reading half of the category transfer (§2, §2b) — source, reviewable plan, apply.
//
// Three pages of one QStackedWidget, the same shape PartMigrationDialog uses,
// because the three questions are genuinely different: *where from*, *what
// exactly would happen*, *do it*. Folding them onto one screen would put a plan
// nobody has read yet next to the button that executes it.
//
// **The middle page is the whole point.** core/import's planMerge() writes
// nothing at all; it produces a MergePlan the user may edit — an action per
// incoming category, a winner per disagreeing field — and applyMerge() then does
// exactly what the edited plan says. This dialog is that editor. Nothing it shows
// is a preview of something already decided.
//
// §11 gets pulled to the front rather than left in a tooltip: an incoming
// category carrying a new *required* attribute makes every part already filed
// under it incomplete the moment the merge lands, and `affectedPartCount` says
// how many. That is the consequence a user most needs before pressing Apply, not
// after, so it is painted onto the row.
//
// **A §9a snapshot is taken before applyMerge(), unconditionally.** The merge is
// transactional (PartManager_Transaction.h) and a failed apply rolls back whole,
// so the backup is not there for the failure case — it is there for the succeeded
// merge the user then regrets, which no transaction can undo. Making that a tick
// box would mean offering to skip the only recovery path there is.
//
// §10: nothing is written before Apply, so a wizard closed halfway costs the user
// only the reviewing. That is still worth a question once a plan exists, and the
// question lives in done() so the window's ✕ and Esc ask it too. After a
// successful apply it stops asking — the work is committed and there is nothing
// left to lose.
// @see docs/design/ARCHITECTURE.md §1b, §1c, §2, §2b, §9a, §10, §11
// @see PartManager_PartTypeTransfer.h, PartManager_Transaction.h, PartManager_CategoryReconcileDialog.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "import/PartManager_PartTypeTransfer.h"
#include <QDialog>
#include <QString>
#include <vector>

class QComboBox;
class QTreeWidgetItem;

namespace Ui { class CategoryImportDialog; }

namespace PartManager
{

	class CategoryImportDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit CategoryImportDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~CategoryImportDialog() override;

		// True once applyMerge() has committed. The caller reloads its category tree only then —
		// a wizard walked out of halfway changed nothing to show.
		bool applied() const;

		// The plan as it was applied, so the caller can hand its `unmatchedLocalTypeIds` to the
		// reconciliation dialog without planning a second time against a database that has since
		// moved on. Meaningless unless applied() is true.
		const MergePlan& plan() const;

		// The bundle currently loaded on the source page, for a test that needs to know what was
		// read without reaching through the widgets.
		const PartTypeBundle& bundle() const;

	protected:
		// Every way out funnels through here — see the §10 note in the header for why the question
		// cannot live on a Cancel button alone.
		void done(int result) override;

	private slots:
		// Re-reads the source the radio buttons and the pickers currently describe. Called on
		// every change rather than behind a "load" button: the page's whole job is to end up with
		// a bundle, and a second button between the file picker and the result is one click that
		// never has another answer.
		void reloadSource();
		// Asks for a `.pmcat` file.
		void browseForBundleFile();
		// Asks for a `.pmdb`/`.db` file and selects it in the known-databases drop-down.
		void browseForDatabaseFile();
		// Forward: source → plan (runs planMerge), plan → confirm, confirm → apply.
		void goNext();
		// Back one page. From the plan page this throws the edited plan away, because the source
		// may be changed and a plan is only meaningful against the bundle it was made from.
		void goBack();
		// Row-level convenience: the selected entry's every conflict resolved one way. Per-field
		// decisions on a category with twenty attributes are not a thing anyone does twice.
		void keepAllLocalOnSelectedRow();
		void takeAllIncomingOnSelectedRow();
		// Opens the reconciliation dialog on the applied plan (Deliverable 3's half of §11).
		void openReconciliation();

	private:
		// Fills the known-databases drop-down from DatabaseRegistry (§1b), leaving the open
		// database out: importing a database into itself is a no-op the plan cannot express.
		void fillKnownDatabases();
		// The `.pmdb`/`.db` path the database half of the page currently names, empty when none.
		QString selectedDatabasePath() const;
		// True when `path` resolves to the database this dialog is writing into. Compared on the
		// canonical file path, not the string: the registry's entry and a hand-picked file can
		// spell the same database two different ways.
		bool isOpenDatabase(const QString& path) const;
		// Puts one message on the page's error line and clears the loaded bundle. The text comes
		// from core verbatim — fromJson()/bundleFromDatabaseFile() already phrase their refusals
		// for a human, and re-wording them here would only lose the detail.
		void failSource(const QString& message);

		// Rebuilds the plan tree from m_plan. One entry per top-level row; a MergeInto row with
		// conflicts gets one child per conflict.
		void refreshPlanTree();
		// Paints one entry row — colour, summary text and the §11 marking (see header).
		void writeEntryRow(QTreeWidgetItem* item, int entryIndex);
		// The entry row the user has selected, whether they clicked it or one of its conflicts, or
		// -1. The convenience buttons act on a whole category, so a selected field still means its
		// category.
		int selectedEntryIndex() const;
		// Sets every conflict of one entry to `resolution` and repaints its controls.
		void resolveWholeEntry(int entryIndex, FieldResolution resolution);
		// The three counts page three states in plain language, plus the §11 total.
		void planCounts(int& outAdded, int& outMerged, int& outSkipped, int& outAffected) const;
		// Which buttons the current page offers, and what Next is called there.
		void updateFooter();
		// Takes the §9a snapshot and runs applyMerge() (in that order — see the header).
		void applyPlan();

		Ui::CategoryImportDialog* m_ui;
		DatabaseHandle* m_handle;
		PartTypeBundle m_bundle;
		MergePlan m_plan;
		// Whether the source page has produced a readable bundle. Not `!m_bundle.nodes.empty()`:
		// a valid bundle carrying no categories is still a successfully read file, and the
		// difference between "nothing loaded" and "loaded, and it was empty" is the error line.
		bool m_bundleLoaded = false;
		// Whether the plan page is showing a plan. Drives the §10 question in done().
		bool m_planLoaded = false;
		bool m_applied = false;
		// Guards the combo-filling passes, which would otherwise write their own initial values
		// back into the plan as if the user had chosen them.
		bool m_loading = false;
		// One action combo per plan entry, parallel to m_plan.entries, so a repaint can restore
		// what the row shows without searching the tree for it.
		std::vector<QComboBox*> m_actionCombos;
	};

}
