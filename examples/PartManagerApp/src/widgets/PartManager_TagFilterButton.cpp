#include "widgets/PartManager_TagFilterButton.h"

#include <QMenu>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWidgetAction>

namespace PartManager
{
	namespace
	{
		// A tag item's name, as typed into the search box. Categories carry an empty one.
		constexpr int TagNameRole = Qt::UserRole;

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}
	}

	TagFilterButton::TagFilterButton(QWidget* parent)
		: QToolButton(parent)
		, m_tree(new QTreeWidget(this))
	{
		setPopupMode(QToolButton::InstantPopup);
		setToolButtonStyle(Qt::ToolButtonTextOnly);
		setToolTip(tr("Filter by tag. Tags in one category match any; across categories, all."));

		m_tree->setHeaderHidden(true);
		m_tree->setMinimumSize(220, 300);
		m_tree->setSelectionMode(QAbstractItemView::NoSelection);

		QMenu* menu = new QMenu(this);
		QWidgetAction* action = new QWidgetAction(menu);
		action->setDefaultWidget(m_tree);
		menu->addAction(action);
		setMenu(menu);

		connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int)
		{
			if (m_updating)
			{
				return;
			}
			syncFromItem(item);
			updateLabel();
			emit selectionChanged();
		});

		updateLabel();
	}

	void TagFilterButton::setVocabulary(const std::vector<TagCategory>& categories,
		const std::vector<Tag>& tags)
	{
		const std::vector<std::vector<std::string>> previous = selectedGroups();

		m_updating = true;
		m_tree->clear();
		for (const TagCategory& category : categories)
		{
			QTreeWidgetItem* parent = new QTreeWidgetItem(m_tree,
				QStringList{ toQt(category.name) });   // user data — never tr()'d
			parent->setData(0, TagNameRole, QString());
			parent->setFlags(parent->flags() | Qt::ItemIsUserCheckable);
			parent->setCheckState(0, Qt::Unchecked);
			parent->setExpanded(true);
			for (const Tag& tag : tags)
			{
				if (tag.categoryId != category.id)
				{
					continue;
				}
				QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList{ toQt(tag.name) });
				item->setData(0, TagNameRole, toQt(tag.name));
				item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
				item->setCheckState(0, Qt::Unchecked);
			}
		}
		// Loose tags sit at the top level rather than under a heading of their own: they are one
		// group in the query, and a heading here would suggest they OR with something.
		for (const Tag& tag : tags)
		{
			if (tag.categoryId != NoTagCategoryId)
			{
				continue;
			}
			QTreeWidgetItem* item = new QTreeWidgetItem(m_tree, QStringList{ toQt(tag.name) });
			item->setData(0, TagNameRole, toQt(tag.name));
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
			item->setCheckState(0, Qt::Unchecked);
		}
		m_updating = false;

		setSelectedGroups(previous);   // ticks on tags that are still here survive the refresh
		setEnabled(m_tree->topLevelItemCount() > 0);
	}

	std::vector<std::vector<std::string>> TagFilterButton::selectedGroups() const
	{
		std::vector<std::vector<std::string>> groups;
		std::vector<std::string> loose;
		for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
		{
			QTreeWidgetItem* top = m_tree->topLevelItem(i);
			const QString ownName = top->data(0, TagNameRole).toString();
			if (!ownName.isEmpty())
			{
				if (top->checkState(0) == Qt::Checked)
				{
					loose.push_back(ownName.toStdString());
				}
				continue;
			}
			std::vector<std::string> group;
			for (int j = 0; j < top->childCount(); ++j)
			{
				QTreeWidgetItem* child = top->child(j);
				if (child->checkState(0) == Qt::Checked)
				{
					group.push_back(child->data(0, TagNameRole).toString().toStdString());
				}
			}
			if (!group.empty())
			{
				groups.push_back(group);
			}
		}
		if (!loose.empty())
		{
			groups.push_back(loose);
		}
		return groups;
	}

	void TagFilterButton::setSelectedGroups(const std::vector<std::vector<std::string>>& groups)
	{
		QStringList wanted;
		for (const std::vector<std::string>& group : groups)
		{
			for (const std::string& name : group)
			{
				wanted << toQt(name).toLower();
			}
		}

		m_updating = true;
		for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
		{
			QTreeWidgetItem* top = m_tree->topLevelItem(i);
			const QString ownName = top->data(0, TagNameRole).toString();
			if (!ownName.isEmpty())
			{
				top->setCheckState(0, wanted.contains(ownName.toLower()) ? Qt::Checked : Qt::Unchecked);
				continue;
			}
			for (int j = 0; j < top->childCount(); ++j)
			{
				QTreeWidgetItem* child = top->child(j);
				const QString name = child->data(0, TagNameRole).toString().toLower();
				child->setCheckState(0, wanted.contains(name) ? Qt::Checked : Qt::Unchecked);
			}
			syncFromItem(top->childCount() > 0 ? top->child(0) : top);
		}
		m_updating = false;
		updateLabel();
	}

	void TagFilterButton::syncFromItem(QTreeWidgetItem* item)
	{
		if (item == nullptr)
		{
			return;
		}
		const bool guarded = m_updating;
		m_updating = true;

		QTreeWidgetItem* category = (item->parent() != nullptr) ? item->parent() : item;
		if (item->parent() == nullptr && !item->data(0, TagNameRole).toString().isEmpty())
		{
			m_updating = guarded;
			return;   // a loose tag at the top level: nothing above or below it to keep in step
		}
		if (item->parent() == nullptr)
		{
			// The category itself was ticked — every member follows it.
			const Qt::CheckState state = item->checkState(0) == Qt::Unchecked
				? Qt::Unchecked : Qt::Checked;
			for (int i = 0; i < item->childCount(); ++i)
			{
				item->child(i)->setCheckState(0, state);
			}
			item->setCheckState(0, state);
			m_updating = guarded;
			return;
		}

		// A member was ticked — the category shows all / some / none of them.
		int checked = 0;
		for (int i = 0; i < category->childCount(); ++i)
		{
			checked += (category->child(i)->checkState(0) == Qt::Checked) ? 1 : 0;
		}
		category->setCheckState(0, checked == 0 ? Qt::Unchecked
			: (checked == category->childCount() ? Qt::Checked : Qt::PartiallyChecked));
		m_updating = guarded;
	}

	void TagFilterButton::updateLabel()
	{
		int count = 0;
		for (const std::vector<std::string>& group : selectedGroups())
		{
			count += static_cast<int>(group.size());
		}
		setText(count == 0 ? tr("Tags") : tr("Tags (%1)").arg(count));
	}

}
