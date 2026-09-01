// @file PartManager_MainWindow.h
// @brief The app's main window — ribbon host plus the Home tab's category tree and part table (§7).
//
// Layout lives in PartManager_MainWindow.ui; the ribbon itself is built in code
// because RibbonWidget's tabs/groups/buttons are not Designer-expressible, and
// the part table's header is filled in code because it is per-category dynamic
// (§7b) and the preview panel's field list is per-part dynamic (§7c). Everything
// else — splitter, tree, table, filter boxes, preview chrome — is in the .ui.
// The window holds no business logic: it asks MainWindowController for nodes and
// rows and renders them.
// @see docs/design/ARCHITECTURE.md §7, §7a, §7b
// @see PartManager_MainWindowController.h, PartManager_TagChipDelegate.h
#pragma once

#include "controllers/PartManager_MainWindowController.h"
#include "controllers/PartManager_StockController.h"
#include <QMainWindow>
#include <memory>

class QLineEdit;
class QTreeWidgetItem;

namespace Ui { class MainWindow; }
namespace RibbonWidget { class Ribbon; }

namespace PartManager
{

	class MainWindow : public QMainWindow
	{
		Q_OBJECT
	public:
		explicit MainWindow(std::unique_ptr<DatabaseHandle> handle, QWidget* parent = nullptr);
		~MainWindow() override;

	private slots:
		// Placeholder for every ribbon button until the screens behind them exist.
		void onNotImplemented();
		// Loads the tree from the database again — the ribbon's Refresh button and startup both use it.
		void reloadCategories();
		// Fills the table with the newly selected category's parts (§7b).
		void onCategorySelectionChanged();
		// Opens the part editor for the double-clicked row (§10 autosave, no Save button).
		void onPartActivated(int row);
		// Re-renders the §7c preview panel for whatever row is selected now, empty state included.
		void updatePreview();
		// Home tab's New Part button — the manual/blank flow (§11).
		void onNewPart();
		// Parts tab's Manage Tags button (§2d).
		void onManageTags();
		// Home tab's Stock group (§7): both write one stock_transaction for the selected part (§3).
		void onRestock();
		void onTakeOut();

	private:
		// Shared body of the two Stock buttons — prompt, then one transaction through the controller.
		void changeStock(bool restocking);
		// part id of the selected table row, 0 when nothing is selected; outName gets its name.
		int selectedPartId(QString* outName = nullptr) const;

		// Re-renders the currently selected category, after an edit changed what it shows.
		void refreshCurrentCategory();

		// Builds the Home/Parts tabs of §7 into the .ui file's ribbonToolBar.
		void buildRibbon();
		// Wires both §7a filter boxes to their debounce timers.
		void setupFilters();
		// Red border + tooltip when a filter box holds a malformed query; clears both when it doesn't.
		void markFilterError(QLineEdit* edit, const QString& error);
		// Adds one CategoryNode and its children under `parent` (nullptr = a tree root).
		void addCategoryItem(const CategoryNode& node, QTreeWidgetItem* parent);
		// Rebuilds the table's dynamic header and rows for one category.
		void showParts(int typeId, const QString& typeName);

		Ui::MainWindow* m_ui;
		MainWindowController m_controller;
		// Non-owning view of the same connection m_controller holds open.
		StockController m_stock;
		RibbonWidget::Ribbon* m_ribbon = nullptr;
		// Which category the table currently shows, so an edit can re-render it in place.
		int m_currentTypeId = NoParentType;
		QString m_currentTypeName;
	};

}
