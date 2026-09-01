// @file PartManager_PartlistManagerDialog.h
// @brief The partlist overview (`partlist-manager.svg`, §4) — pick a BOM, or start one.
//
// The mockup draws this as a whole ribbon tab; it is a dialog here because
// nothing else in §4 exists yet to fill a tab with, and a dialog reaches the
// same three actions (new, open, delete) with no main-window surgery. Promoting
// it to a tab later is a re-parent, not a rewrite — the table and its filling
// are already self-contained.
//
// "Import CSV/BOM" from the mockup is deliberately absent: that is the second
// half of item 8 and lands as its own slice.
// @see docs/design/ARCHITECTURE.md §4, §12b
// @see PartManager_PartlistEditorDialog.h, PartManager_PartlistController.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include <QDialog>
#include <vector>

namespace Ui { class PartlistManagerDialog; }

namespace PartManager
{

	class PartlistManagerDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit PartlistManagerDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~PartlistManagerDialog() override;

	private slots:
		// Re-reads every list from the database and refills the table, keeping the filter.
		void reload();
		// Creates an empty list and opens the editor on it straight away.
		void newPartlist();
		// Opens the editor on the selected row; also the double-click handler.
		void openSelected();
		// Deletes the selected list and its items, after confirming.
		void deleteSelected();
		// Open/Delete need a selected row.
		void updateButtons();

	private:
		// The selected row's partlist, nullptr when nothing is selected.
		const Partlist* selectedPartlist() const;
		// Refills the table from m_partlists, honoring the filter box.
		void showPartlists();

		Ui::PartlistManagerDialog* m_ui;
		PartlistController m_controller;
		// The lists behind the visible rows — the table only shows those matching the filter, so
		// a row index is not a vector index; the row carries its partlist id instead.
		std::vector<Partlist> m_partlists;
	};

}
