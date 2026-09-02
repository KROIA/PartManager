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
class QTimer;
class QTreeWidgetItem;

namespace Ui { class MainWindow; }
namespace RibbonWidget { class Ribbon; }

namespace PartManager
{

	class KicadPreviewWidget;
	class PartlistPanel;

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
		// Parts tab's Import from Mouser button — search, prefill, create, fetch the datasheet (§6).
		void onNewPartFromMouser();
		// Home tab's New Partlist button — creates an empty BOM in the panel below the table (§4).
		void onNewPartlist();
		// Home tab's Partlists button — reveals the §4 panel, which is also the list overview.
		void onManagePartlists();
		// Home tab's Import CSV/BOM button — column mapping, then the result in the panel (§4, §5).
		void onImportPartlist();
		// Home tab's Orders button — the §4 order view: stage, submit, confirm arrivals, close.
		void onManageOrders();
		// Parts tab's Manage Tags button (§2d).
		void onManageTags();
		// Home tab's Stock group (§7): both write one stock_transaction for the selected part (§3).
		void onRestock();
		void onTakeOut();
		// Home tab's View group — the §7b "Customize Columns..." dialog for the selected category.
		void onCustomizeColumns();
		// Persists a column width the user just dragged (§7b).
		void onColumnResized(int logicalIndex, int oldSize, int newSize);
		// Parts tab's KiCad group (§5a) — regenerate the symbol/footprint libraries.
		void onGenerateKicadLibraries();
		// §6's "Open on Mouser" for the selected part, from the preview panel.
		void onOpenOnMouser();
		// Home tab's 3D Viewer button (§13) — the selected part's 3D model: attach, view, remove.
		void onView3DModel();
		// Parts tab's Settings button (§9). Ends the session when a backup was restored, because
		// the file the handle was opened against is no longer the one on disk.
		void onSettings();
		// §9a: takes a snapshot when one is due. Fires on a timer while the app runs; the check
		// is cheap (a directory listing) so a short tick is fine and the interval stays honest
		// even if the machine slept through a due time.
		void onBackupTick();

	protected:
		// §9a: the "always on clean shutdown" snapshot. Also the only one a user who never leaves
		// the app running for six hours would ever get.
		void closeEvent(QCloseEvent* event) override;

	private:
		// Repaints the §5a symbol and footprint previews for the selected part. Shows what the
		// generated library *would* contain when the part has nothing attached yet, rather than
		// an empty box — that is the state most parts are in, and it is not an error.
		void updateKicadPreviews(const PartPreview& preview);

		// Reveals the §4 partlist panel under the part table, giving it a usable share of the
		// window the first time. Everything partlist-related goes through here rather than
		// through a dialog, so a part can be dragged out of the table straight into a BOM.
		void showPartlistPanel();
		// Shared tail of both New Part flows: open the editor on the fresh part, then reload.
		// `datasheetUrl` is the §6 Mouser DataSheetUrl, empty for the manual flow.
		void openNewPart(int partId, const QString& datasheetUrl);
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
		// The §4 screen, hidden until the ribbon asks for it. Owned by the splitter (Qt parent).
		PartlistPanel* m_partlistPanel = nullptr;
		// §5a: the part's schematic symbol and PCB footprint, under its photo. Built in code
		// rather than in the .ui, which would need them promoted there first.
		KicadPreviewWidget* m_symbolPreview = nullptr;
		KicadPreviewWidget* m_footprintPreview = nullptr;
		// Which category the table currently shows, so an edit can re-render it in place.
		// The part the user last picked, kept across the table refills a stock write causes.
		int m_selectedPartId = 0;
		int m_currentTypeId = NoParentType;
		// §9a. Owned by the window (Qt parent), so it stops when the window goes.
		QTimer* m_backupTimer = nullptr;
		QString m_currentTypeName;
		// The columns behind the table's current header — a dragged divider only reports a
		// section index, so this is what turns that back into a column key (§7b).
		std::vector<PartColumn> m_currentColumns;
	};

}
