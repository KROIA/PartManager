// @file PartManager_MainWindow.h
// @brief The app's main window — ribbon host plus the Home tab's category tree and part table (§7).
//
// Layout lives in PartManager_MainWindow.ui; the ribbon itself is built in code
// because RibbonWidget's tabs/groups/buttons are not Designer-expressible, and
// the part table's header is filled in code because it is per-category dynamic
// (§7b). Everything else — splitter, tree, table, filter boxes — is in the .ui.
// The window holds no business logic: it asks MainWindowController for nodes and
// rows and renders them.
// @see docs/design/ARCHITECTURE.md §7, §7a, §7b
// @see PartManager_MainWindowController.h, PartManager_TagChipDelegate.h
#pragma once

#include "controllers/PartManager_MainWindowController.h"
#include <QMainWindow>
#include <memory>

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

	private:
		// Builds the Home/Parts tabs of §7 into the .ui file's ribbonToolBar.
		void buildRibbon();
		// Adds one CategoryNode and its children under `parent` (nullptr = a tree root).
		void addCategoryItem(const CategoryNode& node, QTreeWidgetItem* parent);
		// Rebuilds the table's dynamic header and rows for one category.
		void showParts(int typeId, const QString& typeName);

		Ui::MainWindow* m_ui;
		MainWindowController m_controller;
		RibbonWidget::Ribbon* m_ribbon = nullptr;
	};

}
