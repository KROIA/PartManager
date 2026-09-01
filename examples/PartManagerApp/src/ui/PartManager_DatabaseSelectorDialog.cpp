#include "ui/PartManager_DatabaseSelectorDialog.h"
#include "ui/PartManager_ManageDatabasesDialog.h"
#include "ui_PartManager_DatabaseSelectorDialog.h"

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QTreeWidgetItem>

namespace PartManager
{
	namespace
	{
		// Column index the .pmdb path is stored in — also the row's identity.
		constexpr int PathColumn = ManageDatabasesDialog::PathColumn;

		// A row that can't be opened at all: the entry file is gone, or its schema is
		// newer than this build and §1c refuses to open it either way.
		bool isOpenable(SchemaBadge badge)
		{
			return badge != SchemaBadge::tooNew && badge != SchemaBadge::stale;
		}
	}

	DatabaseSelectorDialog::DatabaseSelectorDialog(QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::DatabaseSelectorDialog)
	{
		m_ui->setupUi(this);

		connect(m_ui->newButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::onNewDatabase);
		connect(m_ui->browseButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::onBrowseForExisting);
		connect(m_ui->removeButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::onRemoveFromList);
		connect(m_ui->manageButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::onManageDatabases);
		connect(m_ui->openButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::onOpenSelected);
		connect(m_ui->quitButton, &QPushButton::clicked, this, &DatabaseSelectorDialog::reject);
		connect(m_ui->databaseList, &QTreeWidget::itemDoubleClicked,
			this, &DatabaseSelectorDialog::onOpenSelected);
		connect(m_ui->databaseList, &QTreeWidget::itemSelectionChanged,
			this, &DatabaseSelectorDialog::onSelectionChanged);

		refreshList();
	}

	DatabaseSelectorDialog::~DatabaseSelectorDialog()
	{
		delete m_ui;
	}

	std::unique_ptr<DatabaseHandle> DatabaseSelectorDialog::takeHandle()
	{
		return std::move(m_handle);
	}

	void DatabaseSelectorDialog::refreshList()
	{
		m_ui->databaseList->clear();
		for (const DatabaseListEntry& entry : m_controller.knownDatabases())
		{
			QTreeWidgetItem* item = new QTreeWidgetItem(m_ui->databaseList);
			ManageDatabasesDialog::fillRow(*item, entry);
		}
		for (int column = ManageDatabasesDialog::NameColumn; column < PathColumn; ++column)
		{
			m_ui->databaseList->resizeColumnToContents(column);
		}

		bool isEmpty = m_ui->databaseList->topLevelItemCount() == 0;
		m_ui->emptyHintLabel->setVisible(isEmpty);
		onSelectionChanged();
	}

	void DatabaseSelectorDialog::onSelectionChanged()
	{
		QString pmdbPath = selectedPmdbPath();
		if (pmdbPath.isEmpty())
		{
			m_ui->openButton->setEnabled(false);
			m_ui->removeButton->setEnabled(false);
			m_ui->badgeHintLabel->clear();
			return;
		}

		DatabaseListEntry entry;
		entry.pmdbPath = pmdbPath;
		entry.badge = DatabaseSelectorController::badgeOf(pmdbPath, entry.schemaVersion);
		m_ui->openButton->setEnabled(isOpenable(entry.badge));
		m_ui->removeButton->setEnabled(true);	// forgetting a dead entry is exactly how you clean one up
		m_ui->badgeHintLabel->setText(ManageDatabasesDialog::badgeExplanation(entry));
	}

	QString DatabaseSelectorDialog::selectedPmdbPath() const
	{
		QTreeWidgetItem* item = m_ui->databaseList->currentItem();
		if (!item || !item->isSelected())
		{
			return QString();
		}
		return item->text(PathColumn);
	}

	void DatabaseSelectorDialog::onNewDatabase()
	{
		QString parentFolder = QFileDialog::getExistingDirectory(this,
			tr("Choose where to create the new database"));
		if (parentFolder.isEmpty())
		{
			return;
		}

		bool confirmed = false;
		QString name = QInputDialog::getText(this, tr("New Database"),
			tr("Database name (this becomes the folder name):"), QLineEdit::Normal, QString(), &confirmed);
		if (!confirmed || name.isEmpty())
		{
			return;
		}

		QString error;
		m_handle = m_controller.createDatabase(parentFolder, name, error);
		if (!m_handle)
		{
			QMessageBox::warning(this, tr("Could not create database"), error);
			refreshList();
			return;
		}
		accept();
	}

	void DatabaseSelectorDialog::onBrowseForExisting()
	{
		// §1b: a database is opened by its entry file, not by its folder.
		QString pmdbPath = QFileDialog::getOpenFileName(this, tr("Open a PartManager database"),
			QString(), tr("PartManager databases (*.pmdb)"));
		if (pmdbPath.isEmpty())
		{
			return;
		}
		m_controller.registerDatabase(pmdbPath);
		refreshList();
	}

	void DatabaseSelectorDialog::onRemoveFromList()
	{
		QString pmdbPath = selectedPmdbPath();
		if (pmdbPath.isEmpty())
		{
			return;
		}
		// Deliberately only un-registers — deleting the folder is a separate action (§1b).
		if (QMessageBox::question(this, tr("Remove from list"),
			tr("Forget this database? Its folder and all its files stay untouched on disk."))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.removeFromList(pmdbPath);
		refreshList();
	}

	void DatabaseSelectorDialog::onManageDatabases()
	{
		ManageDatabasesDialog dialog(this);
		dialog.exec();
		refreshList();	// descriptions may have changed, entries may be gone
	}

	void DatabaseSelectorDialog::onOpenSelected()
	{
		QString pmdbPath = selectedPmdbPath();
		if (pmdbPath.isEmpty())
		{
			return;
		}
		int schemaVersion = 0;
		if (!isOpenable(DatabaseSelectorController::badgeOf(pmdbPath, schemaVersion)))
		{
			return;		// double-click can reach here past the disabled Open button
		}

		QString error;
		m_handle = m_controller.openDatabase(pmdbPath, error);
		if (!m_handle)
		{
			QMessageBox::warning(this, tr("Could not open database"), error);
			return;
		}
		accept();
	}

}
