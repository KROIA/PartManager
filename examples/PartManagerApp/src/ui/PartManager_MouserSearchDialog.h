// @file PartManager_MouserSearchDialog.h
// @brief The embedded Mouser search browser (`mouser-search.svg`, §6) — find a part, hand it on.
//
// One search box rather than the usual part-number/keyword radio pair: a part
// number search is tried first and a keyword search only runs when that came
// back empty, which is the same answer a user would get after flipping the
// switch themselves.
//
// MouserClient is synchronous, so a search really does freeze the dialog for up
// to its timeout — the status line says so and the cursor changes, rather than
// leaving the window looking hung. The dialog does no database work at all; it
// produces a MouserPartPrefill and NewPartDialog decides what to do with it.
// @see docs/design/ARCHITECTURE.md §6, §12b
// @see PartManager_MouserSearchDialog.cpp, PartManager_NewPartDialog.h
#pragma once

#include "mouser/PartManager_MouserClient.h"
#include "mouser/PartManager_MouserSearchService.h"
#include <QDialog>
#include <vector>

namespace Ui { class MouserSearchDialog; }

namespace PartManager
{

	class MouserSearchDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit MouserSearchDialog(QWidget* parent = nullptr);
		~MouserSearchDialog() override;

		// The row "Use this part" was pressed on. Only meaningful after exec() returned Accepted.
		const MouserPartPrefill& selectedPrefill() const;

		// Opens on an already-known query and searches it straight away — how the CSV import
		// arrives here, with the unmatched row's part number in hand. Does nothing without an
		// API key; the constructor has already said so on the status line.
		void searchFor(const QString& query);

	private slots:
		// Runs the search and refills the table. Blocking, see the header note.
		void search();
		// Turns the selected row into a prefill and closes.
		void useSelected();
		// Opens the selected row's ProductDetailUrl in the system browser (§6).
		void openOnMouser();
		// Both footer buttons need a selected row that actually has a URL behind it.
		void updateButtons();

	private:
		// Fills the table from m_results, already ranked.
		void showResults();
		// The MouserPartDto behind the selected row, nullptr when nothing is selected.
		const MouserPartDto* selectedDto() const;

		Ui::MouserSearchDialog* m_ui;
		MouserClient m_client;
		std::vector<MouserPartDto> m_results;
		MouserPartPrefill m_prefill;
	};

}
