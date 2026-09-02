#include "ui/PartManager_ManageTagsDialog.h"
#include "ui_PartManager_ManageTagsDialog.h"
#include "persistence/PartManager_TagRepository.h"

#include <QColorDialog>
#include <QFont>
#include <QInputDialog>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		// Item data roles. A category item carries TagIdRole == NoTagId and its own id in
		// CategoryIdRole; a tag item carries both, so its parent is known without walking up.
		constexpr int TagIdRole = Qt::UserRole;
		constexpr int CategoryIdRole = Qt::UserRole + 1;

		// What a new tag or category starts out as when it has no family colour to derive from.
		const char* const DefaultTagColor = "#90A4AE";
		const char* const DefaultCategoryColor = "#546E7A";

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// The same fill the chips use, with readable text over it.
		void paintSwatch(QTreeWidgetItem* item, const std::string& color)
		{
			const QColor fill(toQt(color));
			if (!fill.isValid())
			{
				return;
			}
			item->setBackground(0, fill);
			item->setForeground(0, fill.lightness() < 128 ? Qt::white : Qt::black);
		}
	}

	ManageTagsDialog::ManageTagsDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::ManageTagsDialog)
		, m_controller(handle)
	{
		m_ui->setupUi(this);

		connect(m_ui->newButton, &QPushButton::clicked, this, &ManageTagsDialog::onNewTag);
		connect(m_ui->newCategoryButton, &QPushButton::clicked, this, &ManageTagsDialog::onNewCategory);
		connect(m_ui->renameButton, &QPushButton::clicked, this, &ManageTagsDialog::onRename);
		connect(m_ui->colorButton, &QPushButton::clicked, this, &ManageTagsDialog::onRecolour);
		connect(m_ui->moveButton, &QPushButton::clicked, this, &ManageTagsDialog::onMoveToCategory);
		connect(m_ui->deleteButton, &QPushButton::clicked, this, &ManageTagsDialog::onDelete);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &ManageTagsDialog::accept);
		connect(m_ui->tagTree, &QTreeWidget::itemSelectionChanged, this, &ManageTagsDialog::updateButtons);

		refreshTree();
	}

	ManageTagsDialog::~ManageTagsDialog()
	{
		delete m_ui;
	}

	void ManageTagsDialog::refreshTree()
	{
		// Which node was selected, so the same one can be selected again afterwards — every edit
		// rebuilds the whole tree, and losing the selection after a rename is jarring.
		const int selectedTagId = selectedTag().id;
		const int selectedCategoryId = selectedCategory().id;

		m_ui->tagTree->clear();
		m_tags = m_controller.allTags();
		m_categories = m_controller.tagCategories();

		QTreeWidgetItem* toSelect = nullptr;
		for (const TagCategory& category : m_categories)
		{
			QTreeWidgetItem* parent = new QTreeWidgetItem(m_ui->tagTree,
				QStringList{ toQt(category.name) });   // user data — never tr()'d
			parent->setData(0, TagIdRole, NoTagId);
			parent->setData(0, CategoryIdRole, category.id);
			QFont bold = parent->font(0);
			bold.setBold(true);
			parent->setFont(0, bold);
			paintSwatch(parent, category.color);
			parent->setExpanded(true);
			if (category.id == selectedCategoryId)
			{
				toSelect = parent;
			}

			for (const Tag& tag : m_tags)
			{
				if (tag.categoryId != category.id)
				{
					continue;
				}
				QTreeWidgetItem* item = new QTreeWidgetItem(parent, QStringList{ toQt(tag.name) });
				item->setData(0, TagIdRole, tag.id);
				item->setData(0, CategoryIdRole, category.id);
				paintSwatch(item, tag.color);
				if (tag.id == selectedTagId)
				{
					toSelect = item;
				}
			}
		}

		// The loose tags, gathered under a heading that is not a category row. Hidden entirely
		// when there are none, so a tidy database does not carry an empty node forever.
		QTreeWidgetItem* loose = nullptr;
		for (const Tag& tag : m_tags)
		{
			if (tag.categoryId != NoTagCategoryId)
			{
				continue;
			}
			if (loose == nullptr)
			{
				loose = new QTreeWidgetItem(m_ui->tagTree, QStringList{ tr("Uncategorised") });
				loose->setData(0, TagIdRole, NoTagId);
				loose->setData(0, CategoryIdRole, NoTagCategoryId);
				loose->setFlags(loose->flags() & ~Qt::ItemIsSelectable);   // a heading, not a row
				loose->setExpanded(true);
			}
			QTreeWidgetItem* item = new QTreeWidgetItem(loose, QStringList{ toQt(tag.name) });
			item->setData(0, TagIdRole, tag.id);
			item->setData(0, CategoryIdRole, NoTagCategoryId);
			paintSwatch(item, tag.color);
			if (tag.id == selectedTagId)
			{
				toSelect = item;
			}
		}

		if (toSelect != nullptr)
		{
			m_ui->tagTree->setCurrentItem(toSelect);
		}
		m_ui->emptyHintLabel->setVisible(m_tags.empty() && m_categories.empty());
		updateButtons();
	}

	void ManageTagsDialog::updateButtons()
	{
		const bool isTag = selectedTag().id != NoTagId;
		const bool isCategory = selectedCategory().id != NoTagCategoryId;
		m_ui->renameButton->setEnabled(isTag || isCategory);
		m_ui->colorButton->setEnabled(isTag || isCategory);
		m_ui->deleteButton->setEnabled(isTag || isCategory);
		m_ui->moveButton->setEnabled(isTag);
	}

	Tag ManageTagsDialog::selectedTag() const
	{
		QTreeWidgetItem* item = m_ui->tagTree->currentItem();
		if (item == nullptr || !item->isSelected())
		{
			return Tag();
		}
		const int tagId = item->data(0, TagIdRole).toInt();
		for (const Tag& tag : m_tags)
		{
			if (tag.id == tagId)
			{
				return tag;
			}
		}
		return Tag();
	}

	TagCategory ManageTagsDialog::selectedCategory() const
	{
		QTreeWidgetItem* item = m_ui->tagTree->currentItem();
		if (item == nullptr || !item->isSelected() || item->data(0, TagIdRole).toInt() != NoTagId)
		{
			return TagCategory();   // nothing selected, or a tag is — not a category
		}
		const int categoryId = item->data(0, CategoryIdRole).toInt();
		for (const TagCategory& category : m_categories)
		{
			if (category.id == categoryId)
			{
				return category;
			}
		}
		return TagCategory();
	}

	int ManageTagsDialog::targetCategoryId() const
	{
		const TagCategory category = selectedCategory();
		if (category.id != NoTagCategoryId)
		{
			return category.id;
		}
		// A tag is selected: a new tag goes beside it, which is nearly always what was meant.
		QTreeWidgetItem* item = m_ui->tagTree->currentItem();
		return (item != nullptr && item->isSelected()) ? item->data(0, CategoryIdRole).toInt()
			: NoTagCategoryId;
	}

	void ManageTagsDialog::onNewTag()
	{
		bool confirmed = false;
		const QString name = QInputDialog::getText(this, tr("New Tag"), tr("Tag name:"),
			QLineEdit::Normal, QString(), &confirmed);
		if (!confirmed || name.trimmed().isEmpty())
		{
			return;
		}

		Tag tag;
		tag.name = name.trimmed().toStdString();
		tag.color = DefaultTagColor;
		tag.categoryId = targetCategoryId();
		// Born into a family: it takes the next step of that family's ramp rather than the grey
		// placeholder, so the colour is right without anyone opening the colour picker.
		for (const TagCategory& category : m_categories)
		{
			if (category.id != tag.categoryId)
			{
				continue;
			}
			int members = 0;
			for (const Tag& existing : m_tags)
			{
				members += (existing.categoryId == category.id) ? 1 : 0;
			}
			tag.color = TagRepository::shadeOf(category.color, members, members + 1);
			tag.sortOrder = members;
		}
		if (m_controller.createTag(tag) == NoTagId)
		{
			// `tag.name` is UNIQUE, so this is almost always a duplicate.
			QMessageBox::warning(this, tr("Could not create tag"),
				tr("A tag with that name already exists."));
			return;
		}
		refreshTree();
	}

	void ManageTagsDialog::onNewCategory()
	{
		bool confirmed = false;
		const QString name = QInputDialog::getText(this, tr("New Category"), tr("Category name:"),
			QLineEdit::Normal, QString(), &confirmed);
		if (!confirmed || name.trimmed().isEmpty())
		{
			return;
		}

		TagCategory category;
		category.name = name.trimmed().toStdString();
		category.color = DefaultCategoryColor;
		category.sortOrder = static_cast<int>(m_categories.size());
		if (m_controller.createTagCategory(category) == NoTagCategoryId)
		{
			QMessageBox::warning(this, tr("Could not create category"),
				tr("A category with that name already exists."));
			return;
		}
		refreshTree();
	}

	void ManageTagsDialog::onRename()
	{
		Tag tag = selectedTag();
		TagCategory category = selectedCategory();
		const bool isTag = tag.id != NoTagId;
		if (!isTag && category.id == NoTagCategoryId)
		{
			return;
		}

		bool confirmed = false;
		const QString name = QInputDialog::getText(this,
			isTag ? tr("Rename Tag") : tr("Rename Category"),
			isTag ? tr("Tag name:") : tr("Category name:"),
			QLineEdit::Normal, toQt(isTag ? tag.name : category.name), &confirmed);
		if (!confirmed || name.trimmed().isEmpty())
		{
			return;
		}

		bool ok = false;
		if (isTag)
		{
			tag.name = name.trimmed().toStdString();
			ok = m_controller.updateTag(tag);
		}
		else
		{
			category.name = name.trimmed().toStdString();
			ok = m_controller.updateTagCategory(category);
		}
		if (!ok)
		{
			QMessageBox::warning(this, tr("Could not rename"),
				tr("Something with that name already exists."));
			return;
		}
		refreshTree();
	}

	void ManageTagsDialog::onRecolour()
	{
		const Tag tag = selectedTag();
		TagCategory category = selectedCategory();
		const bool isTag = tag.id != NoTagId;
		if (!isTag && category.id == NoTagCategoryId)
		{
			return;
		}

		const QColor current(toQt(isTag ? tag.color : category.color));
		const QColor picked = QColorDialog::getColor(current, this,
			isTag ? tr("Tag colour") : tr("Category colour"));
		if (!picked.isValid())
		{
			return;
		}

		if (isTag)
		{
			Tag recoloured = tag;
			recoloured.color = picked.name().toStdString();
			m_controller.updateTag(recoloured);
			refreshTree();
			return;
		}

		// A category's colour is the family's, so its members are re-derived from it. Anything
		// else leaves a "one colour, different shades" family in two unrelated colours.
		category.color = picked.name().toStdString();
		m_controller.updateTagCategory(category);
		std::vector<Tag> members;
		for (const Tag& member : m_tags)
		{
			if (member.categoryId == category.id)
			{
				members.push_back(member);
			}
		}
		const int count = static_cast<int>(members.size());
		for (int i = 0; i < count; ++i)
		{
			Tag member = members[static_cast<size_t>(i)];
			member.color = TagRepository::shadeOf(category.color, i, count);
			m_controller.updateTag(member);
		}
		refreshTree();
	}

	void ManageTagsDialog::onMoveToCategory()
	{
		const Tag tag = selectedTag();
		if (tag.id == NoTagId)
		{
			return;
		}

		QStringList choices;
		std::vector<int> ids;
		choices << tr("(no category)");
		ids.push_back(NoTagCategoryId);
		int currentIndex = 0;
		for (const TagCategory& category : m_categories)
		{
			if (category.id == tag.categoryId)
			{
				currentIndex = static_cast<int>(ids.size());
			}
			choices << toQt(category.name);   // user data
			ids.push_back(category.id);
		}

		bool confirmed = false;
		const QString picked = QInputDialog::getItem(this, tr("Move Tag"),
			tr("Category for \"%1\":").arg(toQt(tag.name)), choices, currentIndex, false, &confirmed);
		if (!confirmed)
		{
			return;
		}
		const int index = choices.indexOf(picked);
		if (index < 0)
		{
			return;
		}
		m_controller.setTagCategory(tag.id, ids[static_cast<size_t>(index)]);
		refreshTree();
	}

	void ManageTagsDialog::onDelete()
	{
		const Tag tag = selectedTag();
		const TagCategory category = selectedCategory();
		if (tag.id != NoTagId)
		{
			// Deleting takes the tag off every part carrying it (§2d), which is not undoable.
			if (QMessageBox::question(this, tr("Delete tag"),
				tr("Delete \"%1\"? It is removed from every part that carries it.").arg(toQt(tag.name)))
				!= QMessageBox::Yes)
			{
				return;
			}
			m_controller.deleteTag(tag.id);
			refreshTree();
			return;
		}
		if (category.id == NoTagCategoryId)
		{
			return;
		}
		// The tags survive — say so, because "delete" over a heading full of tags reads as though
		// it would take them too.
		if (QMessageBox::question(this, tr("Delete category"),
			tr("Delete \"%1\"? Its tags are kept and become uncategorised.").arg(toQt(category.name)))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.deleteTagCategory(category.id);
		refreshTree();
	}

}
