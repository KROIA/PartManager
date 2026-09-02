#include "ui/PartManager_SettingsDialog.h"
#include "ui_PartManager_SettingsDialog.h"

#include "PartManager_AppStartup.h"
#include "mouser/PartManager_MouserCartClient.h"
#include "mouser/PartManager_MouserClient.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QUrl>

namespace PartManager
{
	namespace
	{
		// The snapshot path a row stands for; rows are rebuilt on every refresh, so the path
		// travels with the item rather than being looked up by index.
		constexpr int SnapshotPathRole = Qt::UserRole;

		enum SnapshotColumn
		{
			SnapshotColumnTaken = 0,
			SnapshotColumnSize,
			SnapshotColumnCount
		};

		QString humanSize(long long bytes)
		{
			if (bytes < 1024)
			{
				return QObject::tr("%1 B").arg(bytes);
			}
			if (bytes < 1024 * 1024)
			{
				return QObject::tr("%1 KB").arg(bytes / 1024);
			}
			return QObject::tr("%1 MB").arg(bytes / (1024 * 1024));
		}

		// "2026-09-02_0630" -> "2026-09-02 06:30". The stored form is a filename, not something
		// to show a human.
		QString humanTimestamp(const std::string& stamp)
		{
			if (stamp.size() != 15)
			{
				return QString::fromStdString(stamp);
			}
			return QString::fromStdString(stamp.substr(0, 10)) + QStringLiteral(" ")
				+ QString::fromStdString(stamp.substr(11, 2)) + QStringLiteral(":")
				+ QString::fromStdString(stamp.substr(13, 2));
		}
	}

	SettingsDialog::SettingsDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::SettingsDialog)
		, m_handle(handle)
	{
		m_ui->setupUi(this);

		// The data carried alongside each entry is the stored value, so a reordered or translated
		// combo can never change what gets written.
		m_ui->languageCombo->addItem(tr("English"), QStringLiteral("en"));
		m_ui->languageCombo->addItem(tr("Deutsch"), QStringLiteral("de"));
		m_ui->themeCombo->addItem(tr("Follow system"), QString::fromLatin1(ThemeName::System));
		m_ui->themeCombo->addItem(tr("Light"), QString::fromLatin1(ThemeName::Light));
		m_ui->themeCombo->addItem(tr("Dark"), QString::fromLatin1(ThemeName::Dark));

		m_ui->snapshotTable->setColumnCount(SnapshotColumnCount);
		m_ui->snapshotTable->setHorizontalHeaderLabels(QStringList() << tr("Taken") << tr("Size"));
		m_ui->snapshotTable->horizontalHeader()->setSectionResizeMode(SnapshotColumnTaken,
			QHeaderView::Stretch);

		m_ui->intervalSpin->setRange(Settings::MinBackupIntervalHours, Settings::MaxBackupIntervalHours);
		m_ui->retentionSpin->setRange(Settings::MinBackupRetentionCount, Settings::MaxBackupRetentionCount);

		m_preferences = Settings::getPreferences();
		showPreferences();

		connect(m_ui->languageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &SettingsDialog::onLanguageChanged);
		connect(m_ui->themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &SettingsDialog::onThemeChanged);
		connect(m_ui->currencyEdit, &QLineEdit::textChanged, this, &SettingsDialog::save);
		connect(m_ui->kicadPathEdit, &QLineEdit::textChanged, this, &SettingsDialog::save);
		connect(m_ui->browseKicadButton, &QPushButton::clicked, this, &SettingsDialog::browseKicadPath);
		connect(m_ui->backupsEnabledCheck, &QCheckBox::toggled, this, &SettingsDialog::save);
		connect(m_ui->intervalSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &SettingsDialog::save);
		connect(m_ui->retentionSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &SettingsDialog::save);
		connect(m_ui->folderEdit, &QLineEdit::textChanged, this, &SettingsDialog::save);
		connect(m_ui->browseFolderButton, &QPushButton::clicked, this, &SettingsDialog::browseBackupFolder);
		connect(m_ui->backupNowButton, &QPushButton::clicked, this, &SettingsDialog::backupNow);
		connect(m_ui->restoreButton, &QPushButton::clicked, this, &SettingsDialog::restoreSelected);
		connect(m_ui->openFolderButton, &QPushButton::clicked, this, &SettingsDialog::openBackupFolder);
		connect(m_ui->snapshotTable, &QTableWidget::itemSelectionChanged,
			this, &SettingsDialog::updateButtons);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &SettingsDialog::accept);

		refreshSnapshots();
	}

	SettingsDialog::~SettingsDialog()
	{
		delete m_ui;
	}

	bool SettingsDialog::restoredFromBackup() const
	{
		return m_restored;
	}

	std::string SettingsDialog::databaseFilePath() const
	{
		return m_handle != nullptr ? m_handle->databaseFilePath() : std::string();
	}

	void SettingsDialog::showPreferences()
	{
		m_loading = true;

		const int language = m_ui->languageCombo->findData(
			QString::fromStdString(m_preferences.language));
		m_ui->languageCombo->setCurrentIndex(language < 0 ? 0 : language);
		const int theme = m_ui->themeCombo->findData(QString::fromStdString(m_preferences.theme));
		m_ui->themeCombo->setCurrentIndex(theme < 0 ? 0 : theme);
		m_ui->currencyEdit->setText(QString::fromStdString(m_preferences.currency));
		m_ui->kicadPathEdit->setText(QString::fromStdString(m_preferences.kicadLibraryPath));
		m_ui->backupsEnabledCheck->setChecked(m_preferences.backupsEnabled);
		m_ui->intervalSpin->setValue(m_preferences.backupIntervalHours);
		m_ui->retentionSpin->setValue(m_preferences.backupRetentionCount);
		m_ui->folderEdit->setText(QString::fromStdString(m_preferences.backupFolder));

		m_ui->databasePathLabel->setText(m_handle != nullptr
			? QString::fromStdString(m_handle->pmdbPath())
			: tr("No database is open."));

		// Reported, never edited: a key written here would sit in plain text in the settings
		// file. Naming the variable is the actionable part.
		QStringList keys;
		keys.append(MouserClient::hasApiKey()
			? tr("Search: set (MOUSER_SEARCH_API)")
			: tr("Search: not set — set MOUSER_SEARCH_API and restart"));
		keys.append(MouserCartClient::hasApiKey()
			? tr("Cart: set (MOUSER_API)")
			: tr("Cart: not set — set MOUSER_API and restart"));
		keys.append(tr("Keys are read from the environment only and are never stored by PartManager."));
		m_ui->apiKeyLabel->setText(keys.join(QStringLiteral("\n")));

		m_loading = false;
	}

	void SettingsDialog::save()
	{
		if (m_loading)
		{
			return;
		}
		m_preferences.language = m_ui->languageCombo->currentData().toString().toStdString();
		m_preferences.theme = m_ui->themeCombo->currentData().toString().toStdString();
		m_preferences.currency = m_ui->currencyEdit->text().trimmed().toStdString();
		m_preferences.kicadLibraryPath = m_ui->kicadPathEdit->text().trimmed().toStdString();
		m_preferences.backupsEnabled = m_ui->backupsEnabledCheck->isChecked();
		m_preferences.backupIntervalHours = m_ui->intervalSpin->value();
		m_preferences.backupRetentionCount = m_ui->retentionSpin->value();
		m_preferences.backupFolder = m_ui->folderEdit->text().trimmed().toStdString();
		Settings::setPreferences(m_preferences);
	}

	void SettingsDialog::onThemeChanged()
	{
		save();
		if (m_loading)
		{
			return;
		}
		// Live, not on restart: a theme you have to restart to see is one nobody trusts.
		if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
		{
			applyTheme(*app, m_preferences.theme);
		}
	}

	void SettingsDialog::onLanguageChanged()
	{
		save();
		if (m_loading)
		{
			return;
		}
		QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
		if (app == nullptr)
		{
			return;
		}
		if (!applyLanguage(*app, m_preferences.language))
		{
			// There is no .qm yet, and pretending the switch worked would leave the user waiting
			// for a German UI that is never coming.
			m_ui->statusLabel->setText(
				tr("No translation is installed for that language yet — the interface stays in English."));
			return;
		}
		// Already-built widgets keep the strings they were constructed with; Qt only re-reads
		// tr() on a retranslateUi() pass, which none of these dialogs implement.
		m_ui->statusLabel->setText(tr("The language applies fully after restarting PartManager."));
	}

	void SettingsDialog::browseKicadPath()
	{
		const QString folder = QFileDialog::getExistingDirectory(this,
			tr("KiCad library output folder"), m_ui->kicadPathEdit->text());
		if (!folder.isEmpty())
		{
			m_ui->kicadPathEdit->setText(folder);
		}
	}

	void SettingsDialog::browseBackupFolder()
	{
		const QString folder = QFileDialog::getExistingDirectory(this,
			tr("Backup folder"), m_ui->folderEdit->text());
		if (!folder.isEmpty())
		{
			m_ui->folderEdit->setText(folder);
		}
		refreshSnapshots();
	}

	void SettingsDialog::refreshSnapshots()
	{
		m_snapshots = BackupManager::listSnapshots(databaseFilePath(), m_preferences.backupFolder);

		m_ui->snapshotTable->clearContents();
		m_ui->snapshotTable->setRowCount(0);
		int row = 0;
		for (const BackupEntry& snapshot : m_snapshots)
		{
			m_ui->snapshotTable->insertRow(row);
			const QString cells[SnapshotColumnCount] = {
				humanTimestamp(snapshot.timestamp),
				humanSize(snapshot.sizeBytes),
			};
			for (int column = 0; column < SnapshotColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setData(SnapshotPathRole, QString::fromStdString(snapshot.path));
				item->setToolTip(QString::fromStdString(snapshot.path));
				m_ui->snapshotTable->setItem(row, column, item);
			}
			++row;
		}
		m_ui->snapshotTable->horizontalHeader()->setSectionResizeMode(SnapshotColumnTaken,
			QHeaderView::Stretch);
		updateButtons();
	}

	void SettingsDialog::updateButtons()
	{
		const bool hasDatabase = m_handle != nullptr;
		m_ui->backupNowButton->setEnabled(hasDatabase);
		m_ui->openFolderButton->setEnabled(hasDatabase);
		m_ui->restoreButton->setEnabled(hasDatabase && m_ui->snapshotTable->currentRow() >= 0);
		if (!hasDatabase)
		{
			m_ui->statusLabel->setText(
				tr("Open a database to take or restore snapshots. The other settings apply anyway."));
		}
	}

	void SettingsDialog::backupNow()
	{
		std::string error;
		const std::string path = BackupManager::createSnapshot(databaseFilePath(),
			m_preferences.backupFolder, m_preferences.backupRetentionCount, &error);
		if (path.empty())
		{
			QMessageBox::warning(this, tr("Could not take a snapshot"),
				QString::fromStdString(error));
			return;
		}
		m_ui->statusLabel->setText(tr("Snapshot written to %1").arg(QString::fromStdString(path)));
		refreshSnapshots();
	}

	void SettingsDialog::restoreSelected()
	{
		QTableWidgetItem* item = m_ui->snapshotTable->item(m_ui->snapshotTable->currentRow(),
			SnapshotColumnTaken);
		if (item == nullptr || m_handle == nullptr)
		{
			return;
		}
		const QString snapshotPath = item->data(SnapshotPathRole).toString();

		if (QMessageBox::question(this, tr("Restore this snapshot?"),
			tr("The database will be replaced with the snapshot from %1.\n\n"
			   "Your current database is not deleted — it is renamed and left beside it, so this "
			   "can be undone. PartManager will close afterwards.")
				.arg(item->text()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		// The connection has to go before the file can be replaced. On Windows the copy would
		// simply fail otherwise, which is the safe direction but a confusing message.
		const std::string databasePath = databaseFilePath();
		m_handle->close();

		std::string movedAside;
		std::string error;
		if (!BackupManager::restore(databasePath, snapshotPath.toStdString(), &movedAside, &error))
		{
			QMessageBox::warning(this, tr("Could not restore"), QString::fromStdString(error));
			// Put the session back the way it was: a failed restore must not leave the app
			// holding a closed database.
			m_handle->open();
			return;
		}

		m_restored = true;
		QMessageBox::information(this, tr("Restored"),
			tr("The snapshot is in place. Your previous database was kept as:\n\n%1\n\n"
			   "PartManager will close now — reopen it to work with the restored data.")
				.arg(QString::fromStdString(movedAside)));
		accept();
	}

	void SettingsDialog::openBackupFolder()
	{
		const std::string folder = BackupManager::backupFolderFor(databaseFilePath(),
			m_preferences.backupFolder);
		if (folder.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(folder)));
	}

}
