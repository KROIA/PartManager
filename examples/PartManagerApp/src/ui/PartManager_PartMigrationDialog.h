// @file PartManager_PartMigrationDialog.h
// @brief Bulk migration of a pasted part-number list into the database (§4, §5) — paste, then work the list.
//
// **Not PartlistImportDialog**, which already exists and means the other
// direction: that one reads a BOM and produces a *partlist* ("this board needs
// these parts"), this one reads whatever the user's old inventory was and
// produces *parts* ("these are already in my drawer, put them in the
// database"). Same clipboard, opposite destination — hence a name that says
// migration rather than import, so neither screen can be mistaken for the other
// in a stack trace or a translation file.
//
// Two pages of one QStackedWidget, because the two halves ask completely
// different things of the user. Page one is the same live-preview mapping screen
// PartlistImportDialog uses (paste, delimiter, header flag, six column combos,
// re-previewed on every change); page two is a worklist with one row per pasted
// line, which the user walks down part by part. Nothing about page two is a
// batch operation: §6 is explicit that a part is never invented silently, so
// every Pending row still goes through NewPartDialog and is confirmed by hand.
//
// The Mouser article number is its own mapped column, separate from the
// manufacturer part number, because the two are different identifiers stored in
// different tables — and because knowing one lets Add Part fetch the vendor's
// data for that row before the form opens, which is the whole difference between
// migrating a list of order codes and retyping it.
//
// The parsing, the mapping guess and the already-in-the-database matching all
// live in core/import (PartListMigration) so they are testable without widgets.
// MigrationStatus arrives here as an ascii key (migrationStatusKey()) — core does
// not translate, so the display strings are this dialog's job.
//
// §10 in-progress: the worklist is not persisted anywhere. Closing with rows
// still Pending asks first, and says what the answer means — the parts already
// created are real rows and stay, only the unhandled remainder is lost.
// @see docs/design/ARCHITECTURE.md §3, §4, §5, §6, §10, §12b
// @see PartManager_PartListMigration.h, PartManager_PartlistImportDialog.h, PartManager_NewPartDialog.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include "controllers/PartManager_StockController.h"
#include "import/PartManager_PartListMigration.h"
#include "mouser/PartManager_MouserSearchService.h"
#include <QDialog>
#include <QString>
#include <vector>

class QComboBox;

namespace Ui { class PartMigrationDialog; }

namespace PartManager
{

	class PartMigrationDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit PartMigrationDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~PartMigrationDialog() override;

		// How many parts the worklist actually created. The caller reloads its views only when
		// this is non-zero — a migration the user walked out of halfway changed nothing to show.
		int createdCount() const;

	protected:
		// Every way out of the dialog funnels through here, which is where the §10 "N parts not
		// yet handled" question belongs — the window's X has to ask it too, not just Close.
		void done(int result) override;

	private slots:
		// Re-parses the pasted text with the current delimiter and header flag, re-guesses the
		// mapping and repaints the preview. The mapping is guessed again on every re-parse for the
		// same reason PartlistImportDialog does it: a delimiter change re-splits the header, so the
		// column indices the user had picked no longer mean the same thing.
		void reparse();
		// Re-builds the preview rows for the current mapping, without re-splitting the text.
		void refreshPreview();
		// Resolves the pasted rows against the inventory and switches to the worklist.
		void goToWorklist();
		// Back to the paste page; confirms first when re-parsing would throw away handled rows.
		void goToPaste();

		// §6: looks the selected row up on Mouser and creates the part through the prefill flow.
		void searchSelectedOnMouser();
		// Opens New Part for the selected row. When the row carries a Mouser article number that
		// number is looked up first, so the form arrives with the vendor's own data on it; the
		// pasted line alone is the fallback, not the plan. Never looks a bare manufacturer number
		// up — that is what the search button is for, and a silent request per row would make a
		// bulk flow unpredictable and slow in a way nothing on screen explains.
		void addSelectedPart();
		// Opens the part editor on an Existing row's match.
		void openSelectedPart();
		// §3: books the pasted stock count onto an Existing row's part, after a confirmation.
		void bookSelectedStock();
		// Marks the selected row as deliberately not migrated.
		void skipSelected();
		// Which row actions the selected row's status allows.
		void updateButtons();

	private:
		// Fills one mapping combo with "(not used)" plus every header, and selects `current`.
		void fillColumnCombo(QComboBox* combo, int current);
		// Reads the six combos back into a mapping.
		MigrationColumnMapping currentMapping() const;
		// Looks `mouserPartNumber` up and maps the hit to a prefill; false when there was no hit,
		// no key, or no network, with the reason for the worklist's status line in `outMessage`.
		// MouserClient is synchronous, so this really does freeze the dialog for up to its timeout
		// — same as MouserSearchDialog, and said the same way: a wait cursor and a status line
		// before the call rather than a window that just stops responding.
		bool fetchMouserPrefill(const std::string& mouserPartNumber, MouserPartPrefill& outPrefill,
			QString& outMessage);
		// The worklist row the user has selected, or -1.
		int selectedRow() const;
		// Repaints the whole worklist table plus the progress line.
		void refreshWorklist();
		// Repaints one worklist row in place, so handling a row does not reset the selection.
		void writeWorklistRow(int row);
		// Rows that need nothing more from the user: Created, Existing and Skipped.
		int handledCount() const;
		// Moves the selection to the next Pending row after `row`, wrapping to the top. Leaves it
		// alone when nothing is Pending any more.
		void selectNextPending(int row);
		// Shared tail of both create paths: records the new part on the row, repaints, and opens
		// the editor when the check box asks for it. `datasheetUrl` is the Mouser one, empty for a
		// manual add.
		void noteCreated(int row, int partId, const QString& datasheetUrl);

		Ui::PartMigrationDialog* m_ui;
		PartlistController m_controller;     // allParts() / allSellerLinks(), and the handle
		StockController m_stock;
		CsvTable m_table;
		std::vector<MigrationRow> m_rows;
		// What the Status column says beyond the status itself — "5 booked in" after a restock.
		// Parallel to m_rows, so a row's extra history is not squeezed into MigrationStatus.
		std::vector<QString> m_statusNotes;
		std::vector<Part> m_parts;           // the inventory, re-read after every create
		std::vector<PartSellerLink> m_sellerLinks;
		int m_createdCount = 0;
		bool m_loading = false;              // guards the combo-filling pass
	};

}
