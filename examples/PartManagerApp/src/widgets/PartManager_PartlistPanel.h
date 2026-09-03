// @file PartManager_PartlistPanel.h
// @brief The §4 partlist screen as a panel inside the main window, not a dialog.
//
// This replaces the old PartlistManagerDialog + PartlistEditorDialog pair. Both
// were popups, and the editor's part picker was a combo box over every part in
// the database — which is unusable once there are more than a few dozen. Living
// under the main window's part table solves that without a second browser: the
// browser is already on screen, so a line is added by **dragging a row into this
// panel**. The list selector at the top is what the manager dialog used to be.
//
// **One row per part.** Adding a part the list already carries merges the rows —
// quantities summed, designators (the BMKs on the board) merged — rather than
// appending a second one, which would measure its own shortfall against the same
// untouched stock and under-order the build. mergeDuplicateItems() enforces it on
// load too, so an imported BOM obeys the same rule.
//
// §10 autosave, unchanged from the dialog it replaces: header fields write on a
// debounce flushed when the panel is hidden or the window closes, line edits
// write immediately (they are discrete events, not typing). No Save button.
//
// "Order Missing Parts" is §4's "Check Stock" + "Checkout" in one button: it
// diffs the whole list against stock and raises a `mouser_order` draft. **The
// needed quantity is summed per part before stock is subtracted**, so a BOM that
// lists one resistor on three lines orders the combined shortfall once — adding
// up the per-line "Still needed" column shown in this grid would under-order
// (each line was measured against the same untouched stock).
// @see docs/design/ARCHITECTURE.md §4, §7, §10, §12b
// @see PartManager_PartlistController.h, PartManager_MainWindow.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include <QWidget>
#include <vector>

class QMimeData;
class QTableWidgetItem;
class QTimer;

namespace Ui { class PartlistPanel; }

namespace PartManager
{

	class PartlistPanel : public QWidget
	{
		Q_OBJECT
	public:
		explicit PartlistPanel(DatabaseHandle* handle, QWidget* parent = nullptr);
		~PartlistPanel() override;

		// The mime type a part table has to put on the clipboard for a drop to land here. It is
		// Qt's own item-view format, so the source needs nothing beyond setDragEnabled(true).
		static const char* const PartMimeType;

		// The part id inside a dropped item-view payload, 0 when there is none. Public because
		// it is the one piece of this worth asserting on its own — the drag itself is Qt's.
		static int droppedPartId(const QMimeData* mime);

		// Re-reads the list of partlists, keeping the current one selected when it still exists.
		void reloadPartlists();
		// Opens one particular list, adding it to the selector if the panel had not read it yet.
		void showPartlist(int partlistId);
		// Creates an empty list and opens it. Returns its id, NoPartlistId on failure.
		int createPartlist();
		// Runs the §5 CSV/BOM import wizard and opens whatever it produced.
		void importPartlist();
		// Writes out a still-pending debounced header edit. Called when the panel is hidden and
		// from the main window's closeEvent — a QWidget has no done() to hang it off.
		void flushPendingEdits();

	signals:
		// The panel wants to be collapsed (its Hide button).
		void hideRequested();
		// Stock moved — confirming an order arrival restocks, so the part table is stale.
		void stockChanged();

	protected:
		// Accepts a part dragged out of the main window's table. The drop is taken on the panel
		// rather than on the grid so it lands anywhere in the panel: the grid is empty on a new
		// list, and aiming at an empty table is exactly the case that has to work.
		void dragEnterEvent(QDragEnterEvent* event) override;
		void dragMoveEvent(QDragMoveEvent* event) override;
		void dropEvent(QDropEvent* event) override;
		void hideEvent(QHideEvent* event) override;
		void closeEvent(QCloseEvent* event) override;
		// The grid's own drag and drop, taken off the table's viewport rather than by subclassing
		// QTableWidget. Two drops land there: a row dragged within the grid (reorder) and a part
		// dragged in from the browser (add). Qt's own QTableWidget::InternalMove cannot do the
		// first one here — it moves *items*, and half of every row is a cell widget it leaves
		// behind — so the move is done on m_items and the grid is rebuilt from it.
		bool eventFilter(QObject* watched, QEvent* event) override;

	private slots:
		// §10: writes the header fields back to the existing record.
		void autosaveHeader();
		// Restarts the header debounce; the line grid does not use it.
		void scheduleHeaderSave();
		// Writes the current lines and re-reads them, so the computed columns follow.
		void saveLines();
		// Opens the part picker and adds whatever comes back — the way in when the component
		// browser is hidden, floated or covered and there is nothing to drag out of.
		void addLine();
		// A designator cell was typed into.
		void onCellChanged(QTableWidgetItem* item);
		// The row's right-click menu: open the part in the editor, on Mouser, or its datasheet.
		// The three things anyone checking a BOM line wants, without hunting the part down in the
		// browser first — the panel may well be the only thing on screen.
		void showRowMenu(const QPoint& position);
		// Enables what needs a selected row.
		void updateButtons();
		// Opens the project link in the system browser.
		void openProjectLink();
		// Deletes the whole list after confirming.
		void deletePartlist();
		// §4 checkout: diffs the list against stock and raises a draft order for the shortfall,
		// then hands the user straight to the order view. Unresolved lines cannot be ordered and
		// are named rather than silently left out.
		void orderShortfall();

	private:
		// Re-reads the partlist and its lines and rebuilds everything.
		void reload();
		// Rebuilds the whole grid from m_items, including the per-row widgets.
		void showLines(const std::vector<PartlistLine>& lines);
		// Rewrites only the computed columns and the unresolved highlight, without touching the
		// per-row widgets — what a quantity or part change needs, and it keeps focus where it is.
		void refreshComputedColumns(const std::vector<PartlistLine>& lines);
		// Adds `partId` as a line, or bumps the existing line's quantity when the list already
		// carries that part — dragging the same resistor twice means "two of them", not two rows
		// that each measure themselves against the same untouched stock.
		void addPart(int partId);
		// Points an existing row at `partId`, merging it into the row that already holds that part
		// if there is one. How an unresolved import row is repaired now that the Part column is
		// text rather than a combo box.
		void assignPart(int row, int partId);
		// Drops one row. The per-row ✕ button, so it takes the row rather than reading a selection.
		void removeRow(int row);
		// Moves row `from` to position `to` and writes the new order — the grid's drag reorder.
		void moveRow(int from, int to);
		// Writes m_items back after collapsing rows that ended up on the same part.
		void saveMergedItems();

		Ui::PartlistPanel* m_ui;
		PartlistController m_controller;
		Partlist m_partlist;
		// The rows as the grid currently holds them, one entry per visible row in order.
		std::vector<PartlistItem> m_items;
		QTimer* m_saveTimer;
		// Guards the widget-filling passes, which would otherwise look like user edits.
		bool m_loading = false;
	};

}
