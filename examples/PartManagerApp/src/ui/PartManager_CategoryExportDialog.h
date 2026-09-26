// @file PartManager_CategoryExportDialog.h
// @brief Writes a chosen part of the local category forest out as a `.pmcat` bundle (§2, §2b).
//
// §1b keeps databases fully independent, so the category tree a user spent an
// evening shaping in one of them cannot be pointed at from another — it has to
// travel as data. This dialog is the writing half of that trip; core/import's
// PartTypeTransfer does the actual bundling and the JSON.
//
// The tree is the one the Home tab and PartTypePickerDialog already show — same
// buildCategoryTree() forest, same TypeIconPainter glyphs at the same
// CategoryGlyphSize — because "which categories do I want" is a question the user
// answers by recognising their own tree, not by reading a list.
//
// **Ticking a child ticks its ancestors.** §2b makes a subtype meaningless
// without the parent it inherits from, and bundleFromDatabase() silently pulls
// every ancestor in for exactly that reason. A dialog that let a child be ticked
// alone would be offering a choice the core then overrules behind the user's
// back, so the tick marks move here instead, where they can be seen. A parent
// that is in the bundle but whose children are not shows as partially checked:
// that is a real, useful state, not an in-between one.
//
// Success is a status line rather than a modal. The dialog stays open because
// exporting a second selection to a second file is a normal thing to want, and a
// message box that has to be dismissed before the tree is visible again is in the
// way of it.
// @see docs/design/ARCHITECTURE.md §1b, §2, §2b, §10
// @see PartManager_PartTypeTransfer.h, PartManager_CategoryImportDialog.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_PartType.h"
#include <QDialog>
#include <QString>
#include <vector>

class QPushButton;
class QTreeWidgetItem;

namespace Ui { class CategoryExportDialog; }

namespace PartManager
{

	struct CategoryNode;

	class CategoryExportDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit CategoryExportDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~CategoryExportDialog() override;

		// How many categories the last successful export wrote, 0 when none has happened. The
		// dialog stays open after an export, so this is a running answer rather than a result.
		int exportedCount() const;
		// Where that export went, empty when none has happened.
		const QString& exportedPath() const;

		// Every category currently ticked, ancestors included — what bundleFromDatabase() would
		// be asked for. Public so a test can read the selection without a file dialog in the way.
		std::vector<int> selectedTypeIds() const;

	private slots:
		// Keeps the tick marks consistent: a box the user turned on turns its whole subtree on and
		// pulls its ancestors in, a box turned off takes its subtree with it. Re-entrant by nature
		// (every setCheckState() comes back here), so it is guarded rather than clever.
		void onItemChanged(QTreeWidgetItem* item, int column);
		// Asks for a file, bundles the ticked categories and writes the JSON.
		void onExport();

	private:
		// Fills the tree from the forest builder, ticking everything — the common case is "all of
		// it", and starting from nothing would mean the user ticks their whole catalogue by hand.
		void addTypeItem(const CategoryNode& node, QTreeWidgetItem* parent);
		// Re-derives one item's state from its children: unchecked when nothing below it is in,
		// checked when everything is, partially checked in between (see the header for why that
		// middle state means "in the bundle, but not all of my children are").
		void refreshAncestors(QTreeWidgetItem* item);
		// Every ticked item's type id, in tree order.
		void collectSelected(QTreeWidgetItem* item, std::vector<int>& out) const;
		// Export is pointless with nothing ticked, and the count on the button is the one number
		// the user wants before pressing it.
		void updateExportState();

		Ui::CategoryExportDialog* m_ui;
		DatabaseHandle* m_handle;
		std::vector<PartType> m_types;
		QPushButton* m_exportButton = nullptr;
		// Guards the itemChanged cascade — see onItemChanged().
		bool m_updating = false;
		int m_exportedCount = 0;
		QString m_exportedPath;
	};

}
