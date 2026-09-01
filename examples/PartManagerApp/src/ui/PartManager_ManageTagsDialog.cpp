#include "ui/PartManager_ManageTagsDialog.h"
#include "ui_PartManager_ManageTagsDialog.h"

#include <QColorDialog>
#include <QInputDialog>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>

namespace PartManager
{
	namespace
	{
		// Item data role holding the tag's id.
		constexpr int TagIdRole = Qt::UserRole;
		// What a new tag starts out as until the user picks a colour.
		const char* const DefaultTagColor = "#90A4AE";

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}
	}

	ManageTagsDialog::ManageTagsDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::ManageTagsDialog)
		, m_controller(handle)
	{
		m_ui->setupUi(this);

		connect(m_ui->newButton, &QPushButton::clicked, this, &ManageTagsDialog::onNewTag);
		connect(m_ui->renameButton, &QPushButton::clicked, this, &ManageTagsDialog::onRename);
		connect(m_ui->colorButton, &QPushButton::clicked, this, &ManageTagsDialog::onRecolour);
		connect(m_ui->deleteButton, &QPushButton::clicked, this, &ManageTagsDialog::onDelete);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &ManageTagsDialog::accept);
		connect(m_ui->tagList, &QListWidget::itemSelectionChanged, this, [this]()
		{
			const bool hasSelection = selectedTag().id != NoTagId;
			m_ui->renameButton->setEnabled(hasSelection);
			m_ui->colorButton->setEnabled(hasSelection);
			m_ui->deleteButton->setEnabled(hasSelection);
		});

		refreshList();
	}

	ManageTagsDialog::~ManageTagsDialog()
	{
		delete m_ui;
	}

	void ManageTagsDialog::refreshList()
	{
		m_ui->tagList->clear();
		m_tags = m_controller.allTags();
		for (const Tag& tag : m_tags)
		{
			QListWidgetItem* item = new QListWidgetItem(toQt(tag.name), m_ui->tagList); // user data
			item->setData(TagIdRole, tag.id);

			// The list doubles as the colour preview — the same fill the chips use.
			const QColor color(toQt(tag.color));
			if (color.isValid())
			{
				item->setBackground(color);
				item->setForeground(color.lightness() < 128 ? Qt::white : Qt::black);
			}
		}

		const bool isEmpty = m_ui->tagList->count() == 0;
		m_ui->emptyHintLabel->setVisible(isEmpty);
		m_ui->renameButton->setEnabled(false);
		m_ui->colorButton->setEnabled(false);
		m_ui->deleteButton->setEnabled(false);
	}

	Tag ManageTagsDialog::selectedTag() const
	{
		QListWidgetItem* item = m_ui->tagList->currentItem();
		if (!item || !item->isSelected())
		{
			return Tag();
		}
		// Read back from the cached rows rather than off the item, so sort_order and an
		// unparsable colour survive an edit instead of being reset from what is painted.
		const int tagId = item->data(TagIdRole).toInt();
		for (const Tag& tag : m_tags)
		{
			if (tag.id == tagId)
			{
				return tag;
			}
		}
		return Tag();
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
		if (m_controller.createTag(tag) == NoTagId)
		{
			// `tag.name` is UNIQUE, so this is almost always a duplicate.
			QMessageBox::warning(this, tr("Could not create tag"),
				tr("A tag with that name already exists."));
			return;
		}
		refreshList();
	}

	void ManageTagsDialog::onRename()
	{
		Tag tag = selectedTag();
		if (tag.id == NoTagId)
		{
			return;
		}

		bool confirmed = false;
		const QString name = QInputDialog::getText(this, tr("Rename Tag"), tr("Tag name:"),
			QLineEdit::Normal, toQt(tag.name), &confirmed);
		if (!confirmed || name.trimmed().isEmpty())
		{
			return;
		}

		tag.name = name.trimmed().toStdString();
		if (!m_controller.updateTag(tag))
		{
			QMessageBox::warning(this, tr("Could not rename tag"),
				tr("A tag with that name already exists."));
			return;
		}
		refreshList();
	}

	void ManageTagsDialog::onRecolour()
	{
		Tag tag = selectedTag();
		if (tag.id == NoTagId)
		{
			return;
		}

		const QColor picked = QColorDialog::getColor(QColor(toQt(tag.color)), this, tr("Tag colour"));
		if (!picked.isValid())
		{
			return;
		}
		tag.color = picked.name().toStdString();
		m_controller.updateTag(tag);
		refreshList();
	}

	void ManageTagsDialog::onDelete()
	{
		const Tag tag = selectedTag();
		if (tag.id == NoTagId)
		{
			return;
		}
		// Deleting takes the tag off every part carrying it (§2d), which is not undoable.
		if (QMessageBox::question(this, tr("Delete tag"),
			tr("Delete \"%1\"? It is removed from every part that carries it.").arg(toQt(tag.name)))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.deleteTag(tag.id);
		refreshList();
	}

}
