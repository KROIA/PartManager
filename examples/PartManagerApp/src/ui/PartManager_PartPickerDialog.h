// @file PartManager_PartPickerDialog.h
// @brief "Pick a part" for the partlist panel when the component browser is not on screen (§4, §7).
//
// Dragging a row out of the browser is still the primary way to fill a BOM, but
// the browser is a dock now and can be hidden, floated or covered — and then the
// panel has no way in at all. This is that second way: the same parts, searched
// by name/MPN/manufacturer/description instead of navigated by category.
//
// Deliberately *not* the browser dock reused as a dialog. That widget is wired
// into the main window (tag filters, the §7c preview, the KiCad previews, the
// saved §7b column layout) and lifting it out would be a refactor of the whole
// Home tab to gain a picker that needs one column and a filter box.
//
// Built in code rather than in a .ui: a filter line, a table and a button box.
// @see docs/design/ARCHITECTURE.md §4, §7b
// @see PartManager_PartlistPanel.h, PartManager_PartlistController.h
#pragma once

#include "controllers/PartManager_PartlistController.h"
#include <QDialog>
#include <vector>

class QLineEdit;
class QTableWidget;
class QPushButton;

namespace PartManager
{

	class PartPickerDialog : public QDialog
	{
		Q_OBJECT
	public:
		// Reads through the caller's controller, so the picker sees the same open database
		// without opening a second connection. The controller is borrowed, never owned.
		explicit PartPickerDialog(PartlistController& controller, QWidget* parent = nullptr);

		// The part the user settled on, NoPartId when they cancelled.
		int selectedPartId() const;

	private:
		// Rebuilds the table from m_parts, keeping only what `filter` matches.
		void applyFilter(const QString& filter);

		std::vector<Part> m_parts;
		QLineEdit* m_filterEdit = nullptr;
		QTableWidget* m_table = nullptr;
		QPushButton* m_okButton = nullptr;
	};

}
