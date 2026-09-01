// @file PartManager_PartlistEditorDialog.h
// @brief The manual partlist editor (`partlist-editor.svg`, §4) — the BOM's header and its lines.
//
// §10 autosave, like the part editor: header fields write on a debounce flushed
// on close, line edits write immediately (they are discrete events, not typing).
// No Save button anywhere.
//
// The line grid is built in code rather than in Designer because each row needs
// a live part picker and a spin box wired to a specific line — the frame around
// it (header form, table, footer) is in the .ui. `Qty total`, `In stock` and
// `Shortfall` are computed by PartlistRepository::lines() and never editable;
// changing the multiplier re-reads them for every row at once.
//
// **Not here, all of it item 9's:** "Check Stock", "Take Out Parts" and
// "Checkout" from the mockup. They push shortfall rows into a `mouser_order`
// draft, and that table does not exist yet. The shortfall number they need is
// already shown per line.
// @see docs/design/ARCHITECTURE.md §4, §10, §12b
// @see PartManager_PartlistController.h, PartManager_PartlistManagerDialog.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include <QDialog>
#include <vector>

class QTableWidgetItem;
class QTimer;

namespace Ui { class PartlistEditorDialog; }

namespace PartManager
{

	class PartlistEditorDialog : public QDialog
	{
		Q_OBJECT
	public:
		// Borrows the manager's controller rather than building a second one — both talk to the
		// same DatabaseHandle, which neither of them owns.
		PartlistEditorDialog(const PartlistController& controller, int partlistId,
			QWidget* parent = nullptr);
		~PartlistEditorDialog() override;

	protected:
		// Every way out of a QDialog funnels through here, so it is the one place a still-pending
		// debounced header write has to be flushed.
		void done(int result) override;

	private slots:
		// §10: writes the header fields back to the existing record.
		void autosaveHeader();
		// Restarts the header debounce; the line grid does not use it.
		void scheduleHeaderSave();
		// Writes the current lines and re-reads them, so the computed columns follow.
		void saveLines();
		// Appends an empty, unresolved line.
		void addLine();
		// Drops the selected line.
		void removeLine();
		// A designator cell was typed into.
		void onCellChanged(QTableWidgetItem* item);
		// Remove Line needs a selected row.
		void updateButtons();
		// Opens the project link in the system browser.
		void openProjectLink();

	private:
		// Re-reads the partlist and its lines and rebuilds everything.
		void reload();
		// Rebuilds the whole grid from m_items, including the per-row widgets.
		void showLines(const std::vector<PartlistLine>& lines);
		// Rewrites only the computed columns and the unresolved highlight, without touching the
		// per-row widgets — what a quantity or part change needs, and it keeps focus where it is.
		void refreshComputedColumns(const std::vector<PartlistLine>& lines);

		Ui::PartlistEditorDialog* m_ui;
		PartlistController m_controller;
		Partlist m_partlist;
		// The rows as the grid currently holds them, one entry per visible row in order.
		std::vector<PartlistItem> m_items;
		QTimer* m_saveTimer;
		// Guards the widget-filling passes, which would otherwise look like user edits.
		bool m_loading = false;
	};

}
