// @file PartManager_DatabaseSelectorDialog.h
// @brief Startup database selector (§1b) — pick, create, import or forget a database.
//
// Always shown at startup; there is deliberately no auto-resume-last-used
// (§1b). Layout lives in PartManager_DatabaseSelectorDialog.ui; this class only
// fills the list and forwards clicks to DatabaseSelectorController (§12b).
// On accept(), takeHandle() hands the opened database to the caller.
// @see docs/design/ARCHITECTURE.md §1b
// @see PartManager_DatabaseSelectorController.h
#pragma once

#include "controllers/PartManager_DatabaseSelectorController.h"
#include <QDialog>
#include <memory>

namespace Ui { class DatabaseSelectorDialog; }

namespace PartManager
{

	class DatabaseSelectorDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit DatabaseSelectorDialog(QWidget* parent = nullptr);
		~DatabaseSelectorDialog() override;

		// The database opened by this dialog. Only non-null after the dialog was accepted.
		std::unique_ptr<DatabaseHandle> takeHandle();

	private slots:
		void onNewDatabase();
		void onBrowseForExisting();
		void onRemoveFromList();
		void onOpenSelected();

	private:
		// Re-reads the registry into the list widget and re-enables/disables the buttons.
		void refreshList();
		// .pmdb path of the selected row, empty if nothing is selected.
		QString selectedPmdbPath() const;

		Ui::DatabaseSelectorDialog* m_ui;
		DatabaseSelectorController m_controller;
		std::unique_ptr<DatabaseHandle> m_handle;
	};

}
