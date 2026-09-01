// @file PartManager_PartlistImportDialog.h
// @brief Generic CSV/BOM import into a new partlist (§4, §5) — the column-mapping screen.
//
// The parsing, the mapping guess and the part matching all live in
// core/import (BomCsvImport) so they are testable without widgets; this dialog
// is the file picker, the four mapping combos and the preview grid on top of
// them. Everything re-previews live, so the user sees what a mapping change
// does before anything is written.
//
// Nothing reaches the database until Import is pressed — this is a §10
// in-progress step, so there is no autosave and Cancel discards.
//
// A row that matches no part imports as an **unresolved** line (§4), never as a
// newly invented typeless part. Decided 2026-09-01: when a Mouser key is
// present the dialog offers to look the row up and create the part properly
// through the §6 prefill flow; without a key it just reports the row.
// @see docs/design/ARCHITECTURE.md §4, §5, §10, §12b
// @see PartManager_BomCsvImport.h, PartManager_PartlistEditorDialog.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include "import/PartManager_BomCsvImport.h"
#include <QDialog>
#include <QString>
#include <vector>

class QComboBox;

namespace Ui { class PartlistImportDialog; }

namespace PartManager
{

	class PartlistImportDialog : public QDialog
	{
		Q_OBJECT
	public:
		PartlistImportDialog(const PartlistController& controller, QWidget* parent = nullptr);
		~PartlistImportDialog() override;

		// The partlist Import created, NoPartlistId while the dialog was cancelled or the
		// insert failed. The caller opens the editor on it.
		int createdPartlistId() const;

	private slots:
		// Picks a file and reads it; the mapping is guessed and the preview built.
		void chooseFile();
		// Re-reads the already-chosen file with the delimiter the user picked.
		void reparse();
		// Re-matches and repaints the preview for the current mapping.
		void refreshPreview();
		// §6: looks the selected unmatched row up on Mouser and offers to create the part.
		void lookUpSelectedOnMouser();
		// Writes the partlist and its lines.
		void importNow();
		// The Mouser button needs an unmatched row selected.
		void updateButtons();

	private:
		// Fills one mapping combo with "(not used)" plus every header, and selects `current`.
		void fillColumnCombo(QComboBox* combo, int current);
		// Reads the four combos back into a mapping.
		BomColumnMapping currentMapping() const;
		// The preview row the user has selected, or -1.
		int selectedRow() const;

		Ui::PartlistImportDialog* m_ui;
		PartlistController m_controller;
		QString m_csvText;              // the file as read, so a delimiter change needs no re-read
		CsvTable m_table;
		std::vector<BomRow> m_rows;
		std::vector<Part> m_parts;      // the inventory, re-read after a Mouser-created part
		int m_createdPartlistId = NoPartlistId;
		bool m_loading = false;         // guards the combo-filling pass
	};

}
