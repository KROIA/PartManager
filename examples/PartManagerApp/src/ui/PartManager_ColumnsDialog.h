// @file PartManager_ColumnsDialog.h
// @brief The "Customize Columns..." dialog (§7b) — reorder, show/hide, reset one category's table.
//
// Layout lives in PartManager_ColumnsDialog.ui; only the item list is built in
// code, because it is per-category data. It edits a `std::vector<PartColumn>`
// and nothing else — the caller persists the result through
// MainWindowController, so no repository call lives in ui/.
//
// Reordering is the list widget's own internal drag-and-drop and visibility is
// the item's check box, so there are no Up/Down/Show/Hide buttons to keep in
// sync with the selection. "Restore Defaults" closes the dialog with
// resetRequested() set instead of returning a layout — dropping the saved rows
// is what "default" means (§7b), not writing a second layout that looks like one.
//
// `name` is shown greyed and non-draggable: the table hangs the part id and the
// §2d tag chips off column 0, so it stays first and visible.
// @see docs/design/ARCHITECTURE.md §7b, §12b
// @see PartManager_MainWindowController.h
#pragma once

#include "controllers/PartManager_MainWindowController.h"
#include <QDialog>
#include <vector>

namespace Ui { class ColumnsDialog; }

namespace PartManager
{

	class ColumnsDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `columns` is the category's full column set, hidden ones included
		// (MainWindowController::allColumnsFor()).
		ColumnsDialog(const QString& categoryName, const std::vector<PartColumn>& columns,
			QWidget* parent = nullptr);
		~ColumnsDialog() override;

		// The edited layout, in the order the list shows it. Only meaningful after exec()
		// returned Accepted and resetRequested() is false.
		std::vector<PartColumn> columns() const;

		// True when the user pressed "Restore Defaults": discard the layout instead of saving one.
		bool resetRequested() const;

	private:
		Ui::ColumnsDialog* m_ui;
		// The dialog's input, indexed by the item's Qt::UserRole — the list item only carries a
		// label and a check state, everything else about a column rides here.
		std::vector<PartColumn> m_columns;
		bool m_resetRequested = false;
	};

}
