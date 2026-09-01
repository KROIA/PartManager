// @file PartManager_ManageDatabasesDialog.h
// @brief "Manage Databases..." (§1b) — the known-databases list plus description editing.
//
// The same registry rows the selector shows, with the schema-version badge (§1c)
// and the per-database description spelled out, plus the two list-level actions:
// edit description (writes the database folder's README.md) and "Remove from
// list" (un-registers only — deleting a database folder is deliberately not here).
// Layout lives in PartManager_ManageDatabasesDialog.ui; all behavior sits in
// DatabaseSelectorController (§12b).
// @see docs/design/ARCHITECTURE.md §1b, §1c
// @see PartManager_DatabaseSelectorController.h
#pragma once

#include "controllers/PartManager_DatabaseSelectorController.h"
#include <QDialog>

class QTreeWidgetItem;
namespace Ui { class ManageDatabasesDialog; }

namespace PartManager
{

	class ManageDatabasesDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit ManageDatabasesDialog(QWidget* parent = nullptr);
		~ManageDatabasesDialog() override;

		// Column indices, shared with the selector's list so fillRow() can serve both.
		enum Column { NameColumn = 0, SchemaColumn, DescriptionColumn, LastOpenedColumn, PathColumn };

		// Short badge text for a row, e.g. "v4 ✓ current" or "v6 ✗ too new".
		static QString badgeText(const DatabaseListEntry& entry);
		// The one-line explanation behind the badge — what opening this database would do.
		static QString badgeExplanation(const DatabaseListEntry& entry);
		// Writes one registry row into a list item using the Column indices above.
		static void fillRow(QTreeWidgetItem& item, const DatabaseListEntry& entry);

	private slots:
		void onEditDescription();
		void onRemoveFromList();

	private:
		// Re-reads the registry into the list widget and re-enables/disables the buttons.
		void refreshList();
		// .pmdb path of the selected row, empty if nothing is selected.
		QString selectedPmdbPath() const;

		Ui::ManageDatabasesDialog* m_ui;
		DatabaseSelectorController m_controller;
	};

}
