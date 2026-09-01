#include "ui/PartManager_ManageDatabasesDialog.h"
#include "ui_PartManager_ManageDatabasesDialog.h"
#include "database/PartManager_SchemaMigrator.h"

#include <QBrush>
#include <QColor>
#include <QInputDialog>
#include <QMessageBox>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		// Badge colors, matching the mockup's green / amber / red / grey.
		QColor badgeColor(SchemaBadge badge)
		{
			switch (badge)
			{
			case SchemaBadge::current:		return QColor(0x18, 0x6a, 0x3a);
			case SchemaBadge::needsUpdate:	return QColor(0x8a, 0x4b, 0x00);
			case SchemaBadge::tooNew:		return QColor(0xb0, 0x22, 0x22);
			default:						return QColor(0x8a, 0x92, 0x9c);
			}
		}
	}

	QString ManageDatabasesDialog::badgeText(const DatabaseListEntry& entry)
	{
		switch (entry.badge)
		{
		case SchemaBadge::current:		return tr("v%1 current").arg(entry.schemaVersion);
		case SchemaBadge::needsUpdate:	return tr("v%1 needs update").arg(entry.schemaVersion);
		case SchemaBadge::tooNew:		return tr("v%1 too new").arg(entry.schemaVersion);
		case SchemaBadge::stale:		return tr("missing");
		default:						return tr("unreadable");
		}
	}

	QString ManageDatabasesDialog::badgeExplanation(const DatabaseListEntry& entry)
	{
		switch (entry.badge)
		{
		case SchemaBadge::current:
			return tr("Schema is up to date with this copy of PartManager.");
		case SchemaBadge::needsUpdate:
			return tr("Opening will back up the database, then migrate schema v%1 to v%2 automatically.")
				.arg(entry.schemaVersion).arg(CurrentSchemaVersion);
		case SchemaBadge::tooNew:
			return tr("Saved by a newer PartManager (schema v%1) — this copy only supports up to v%2. "
				"Update PartManager to open it.").arg(entry.schemaVersion).arg(CurrentSchemaVersion);
		case SchemaBadge::stale:
			return tr("The entry file is gone from disk — the folder was moved, renamed or deleted. "
				"Use \"Remove from list\" to forget it.");
		default:
			return tr("The entry file could not be read, so its schema version is unknown.");
		}
	}

	void ManageDatabasesDialog::fillRow(QTreeWidgetItem& item, const DatabaseListEntry& entry)
	{
		item.setText(NameColumn, entry.name);				// user data (folder name) — not translated
		item.setText(SchemaColumn, badgeText(entry));
		item.setForeground(SchemaColumn, QBrush(badgeColor(entry.badge)));
		item.setText(DescriptionColumn, entry.description);	// user data — not translated
		item.setText(LastOpenedColumn, entry.lastOpenedAt.isEmpty() ? tr("never") : entry.lastOpenedAt);
		item.setText(PathColumn, entry.pmdbPath);

		QString explanation = badgeExplanation(entry);
		for (int column = NameColumn; column <= PathColumn; ++column)
		{
			item.setToolTip(column, explanation);
		}
	}

	ManageDatabasesDialog::ManageDatabasesDialog(QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::ManageDatabasesDialog)
	{
		m_ui->setupUi(this);

		connect(m_ui->editDescriptionButton, &QPushButton::clicked,
			this, &ManageDatabasesDialog::onEditDescription);
		connect(m_ui->removeButton, &QPushButton::clicked,
			this, &ManageDatabasesDialog::onRemoveFromList);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &ManageDatabasesDialog::accept);
		connect(m_ui->databaseList, &QTreeWidget::itemDoubleClicked,
			this, &ManageDatabasesDialog::onEditDescription);
		connect(m_ui->databaseList, &QTreeWidget::itemSelectionChanged,
			this, [this]() {
				bool hasSelection = !selectedPmdbPath().isEmpty();
				m_ui->editDescriptionButton->setEnabled(hasSelection);
				m_ui->removeButton->setEnabled(hasSelection);
			});

		refreshList();
	}

	ManageDatabasesDialog::~ManageDatabasesDialog()
	{
		delete m_ui;
	}

	void ManageDatabasesDialog::refreshList()
	{
		m_ui->databaseList->clear();
		for (const DatabaseListEntry& entry : m_controller.knownDatabases())
		{
			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->databaseList);
			fillRow(*item, entry);
		}
		for (int column = NameColumn; column < PathColumn; ++column)
		{
			m_ui->databaseList->resizeColumnToContents(column);
		}

		bool isEmpty = m_ui->databaseList->topLevelItemCount() == 0;
		m_ui->emptyHintLabel->setVisible(isEmpty);
		m_ui->editDescriptionButton->setEnabled(false);
		m_ui->removeButton->setEnabled(false);
	}

	QString ManageDatabasesDialog::selectedPmdbPath() const
	{
		QTreeWidgetItem* item = m_ui->databaseList->currentItem();
		if (!item || !item->isSelected())
		{
			return QString();
		}
		return item->text(PathColumn);
	}

	void ManageDatabasesDialog::onEditDescription()
	{
		QString pmdbPath = selectedPmdbPath();
		if (pmdbPath.isEmpty())
		{
			return;
		}

		bool confirmed = false;
		QString description = QInputDialog::getMultiLineText(this, tr("Edit description"),
			tr("Description (stored as README.md in the database's folder):"),
			DatabaseSelectorController::descriptionOf(pmdbPath), &confirmed);
		if (!confirmed)
		{
			return;
		}
		if (!DatabaseSelectorController::setDescription(pmdbPath, description))
		{
			QMessageBox::warning(this, tr("Could not save description"),
				tr("README.md could not be written — the database folder may have been moved or "
					"is read-only."));
		}
		refreshList();
	}

	void ManageDatabasesDialog::onRemoveFromList()
	{
		QString pmdbPath = selectedPmdbPath();
		if (pmdbPath.isEmpty())
		{
			return;
		}
		// Deliberately only un-registers — deleting the folder is a separate action (§1b).
		if (QMessageBox::question(this, tr("Remove from list"),
			tr("Forget this database?\n\nPartManager stops listing it. Its folder, its parts and all "
				"its files stay exactly where they are on disk — nothing is deleted."))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.removeFromList(pmdbPath);
		refreshList();
	}

}
