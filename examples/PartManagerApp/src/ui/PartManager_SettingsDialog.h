// @file PartManager_SettingsDialog.h
// @brief The Settings screen (`settings-dialog.svg`, §9) — general, appearance, storage, backups.
//
// §10 autosave, like every other editor: each control writes the whole
// preferences struct on change, and there is no Save button. The theme applies
// live, because a theme you have to restart to see is not a setting anyone
// trusts.
//
// **No API-key field, deliberately.** §9's Storage tab lists one, but this
// dialog only *reports* whether `MOUSER_SEARCH_API` / `MOUSER_CART_API` are set
// and how to set them. Writing a key into the settings file would put it in
// plain text in the user's data folder, which is exactly what keeping it in the
// environment avoids.
//
// The Backups tab doubles as the restore UI (§9a): "Restore Selected" swaps a
// snapshot in, having first moved the current database aside. It closes the
// database to do so, which is why it ends the session rather than carrying on
// with a handle pointing at a file that is no longer there.
// @see docs/design/ARCHITECTURE.md §8, §9, §9a
// @see PartManager_Settings.h, PartManager_BackupManager.h
#pragma once

#include "backup/PartManager_BackupManager.h"
#include "database/PartManager_DatabaseHandle.h"
#include "filestore/PartManager_FileStore.h"
#include "settings/PartManager_Settings.h"
#include <QDialog>
#include <vector>

namespace Ui { class SettingsDialog; }

namespace PartManager
{

	class SettingsDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `handle` may be null: Settings is reachable before a database is open, and everything
		// but the Backups tab still makes sense then.
		explicit SettingsDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~SettingsDialog() override;

		// True when a restore was performed, so the caller knows the database on disk is not the
		// one its handle was opened against and the session has to end.
		bool restoredFromBackup() const;

	private slots:
		// §10: writes every preference back. One slot for all of them — they are one struct and
		// a partial write is not a state anything wants.
		void save();
		void onThemeChanged();
		void onLanguageChanged();
		void browseKicadPath();
		void browseBackupFolder();
		void refreshSnapshots();
		void backupNow();
		void restoreSelected();
		void openBackupFolder();
		// §12a housekeeping: finds stored files no `part_file` row points at any more, and the
		// mesh-cache entries the `.pmmesh` rename left behind. Reports only; deleting is a
		// second, separate press.
		void scanUnusedFiles();
		void deleteUnusedFiles();
		void updateButtons();

	private:
		// Fills every control from `m_preferences` without triggering save().
		void showPreferences();
		std::string databaseFilePath() const;

		Ui::SettingsDialog* m_ui;
		DatabaseHandle* m_handle;
		AppPreferences m_preferences;
		std::vector<BackupEntry> m_snapshots;
		// What the last scan found, so "Delete them" removes exactly what was reported rather
		// than re-scanning and possibly deleting something the user never saw listed.
		FileStoreOrphans m_orphans;
		int m_staleMeshCache = 0;
		// Guards showPreferences()' own setter calls, which would otherwise look like user edits
		// and write the defaults back over the stored values.
		bool m_loading = false;
		bool m_restored = false;
	};

}
