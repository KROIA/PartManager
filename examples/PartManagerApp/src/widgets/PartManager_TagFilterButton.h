// @file PartManager_TagFilterButton.h
// @brief §7a's tag filter: a two-level checkable tag tree in a drop-down, beside the filter box.
//
// The picker owns no filtering of its own. It turns a set of ticks into `tag:`
// terms and writes them into the §7a search box (SearchQuery::withTagGroups), so
// there is exactly one filter pipeline and the user can see — and hand-edit —
// what the ticks did.
//
// **Within a family the tags OR; across families they AND.** Ticking I2C and SPI
// asks for either, which is the whole point of grouping them; ticking SMD as well
// asks for "either bus, and surface mount". Anything else makes the second tick
// in a family narrow the result to nothing, which is not what ticking two members
// of one list means anywhere else.
// @see docs/design/ARCHITECTURE.md §2d, §7a
// @see PartManager_SearchQuery.h, PartManager_ManageTagsDialog.h
#pragma once

#include "domain/PartManager_Tag.h"
#include "domain/PartManager_TagCategory.h"
#include <QToolButton>
#include <string>
#include <vector>

class QTreeWidget;
class QTreeWidgetItem;

namespace PartManager
{

	class TagFilterButton : public QToolButton
	{
		Q_OBJECT
	public:
		explicit TagFilterButton(QWidget* parent = nullptr);

		// Re-reads the vocabulary shown in the drop-down. Ticks on tags that still exist survive,
		// so refreshing after a rename in Manage Tags does not silently widen the filter.
		void setVocabulary(const std::vector<TagCategory>& categories, const std::vector<Tag>& tags);

		// The ticked tags as SearchQuery groups: one group per family with anything ticked, plus
		// one for the ticked uncategorised tags. Empty when nothing is ticked.
		std::vector<std::vector<std::string>> selectedGroups() const;

		// Ticks exactly the tags named here (matched case-insensitively, as the grammar does).
		// Names that are not in the vocabulary are ignored rather than invented.
		void setSelectedGroups(const std::vector<std::vector<std::string>>& groups);

	signals:
		// A tick changed. Not emitted by setVocabulary() or setSelectedGroups(), so the widget
		// cannot drive a round trip through whatever set it.
		void selectionChanged();

	private:
		void updateLabel();
		// Applies a category's tick down to its children, and re-derives a category's own
		// check state from them.
		void syncFromItem(QTreeWidgetItem* item);

		QTreeWidget* m_tree;
		// Set while the widget is writing its own check states, so itemChanged does not read
		// half-applied state or emit for something the caller asked for.
		bool m_updating = false;
	};

}
