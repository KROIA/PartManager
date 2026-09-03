// @file PartManager_ManageTagsDialog.h
// @brief The §2d Manage Tags dialog — the two-level tag tree: categories over tags.
//
// One tree, both levels: a category is a heading over tags that answer the same
// question (*Bus protocols* over I2C/SPI/UART), and Rename/Colour/Delete act on
// whichever of the two is selected. Tags belonging to no category are gathered
// under a pseudo-node at the bottom — it is not a row in `tag_category`, only a
// place to see and re-parent them from.
//
// Recolouring a category recolours its tags into the new family's ramp, which is
// the point of having families at all; a tag can still be given its own colour
// afterwards and keeps it.
//
// **Gradient** is the second way to colour a family, for the ramps a single base
// colour cannot express — a lifecycle reading green through amber to red. It
// spreads the members evenly between the *first and last tag's own* colours, so
// the two ends are chosen with the ordinary colour picker and the middle is
// derived. That makes the order inside a category meaningful, which is what Up /
// Down are for; they rewrite `tag.sort_order` for the whole group, and every list
// in the app orders by it, so the ramp reads the same in the chips, the "+ Tag"
// menu and the filter drop-down.
//
// Reachable from the Parts ribbon tab's *Manage* group. Every action writes
// through immediately (there is no OK/Cancel over the tree), which matches §10:
// these are all already-existing records the moment they are created.
// @see docs/design/ARCHITECTURE.md §2d, §10
// @see PartManager_PartEditorController.h, PartManager_TagCategory.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>

class QTreeWidgetItem;
namespace Ui { class ManageTagsDialog; }

namespace PartManager
{

	class ManageTagsDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit ManageTagsDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~ManageTagsDialog() override;

	private slots:
		void onNewTag();
		void onNewCategory();
		void onRename();
		void onRecolour();
		void onMoveToCategory();
		void onMoveUp();
		void onMoveDown();
		void onRecalculateGradient();
		void onDelete();

	private:
		// Re-reads the vocabulary into the tree and re-enables/disables the per-selection buttons.
		// Categories stay expanded across a refresh so an edit does not collapse the tree.
		void refreshTree();
		void updateButtons();

		// The tags of one category in display order, which is the order the gradient walks.
		std::vector<Tag> tagsInCategory(int categoryId) const;
		// Swaps the selected tag with its neighbour `offset` places away, and writes the whole
		// group's sort_order back. Nothing happens at either end.
		void moveSelectedTag(int offset);

		// The selected tag, or a default-constructed one (id == NoTagId) when the selection is a
		// category or nothing.
		Tag selectedTag() const;
		// The selected category, or one with id == NoTagCategoryId when the selection is a tag,
		// the uncategorised pseudo-node, or nothing.
		TagCategory selectedCategory() const;
		// Where a new tag should land: the selected category, or the parent of the selected tag.
		int targetCategoryId() const;

		Ui::ManageTagsDialog* m_ui;
		PartEditorController m_controller;
		// The rows behind the tree items — the selected* accessors read from here, not off the
		// widget, so sort_order and an unparsable colour survive an edit instead of being reset
		// from what is painted.
		std::vector<Tag> m_tags;
		std::vector<TagCategory> m_categories;
	};

}
