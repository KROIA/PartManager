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
// Right-clicking anywhere the child widgets do not claim opens §1b's Help / About / Switch
// Database menu — the one place the promise that a database can be swapped without restarting
// is kept.
// @see docs/design/ARCHITECTURE.md §1b, §7, §7a, §7b
// @see PartManager_MainWindowController.h, PartManager_TagChipDelegate.h
#pragma once

#include "controllers/PartManager_MainWindowController.h"
#include "controllers/PartManager_StockController.h"
#include <QMainWindow>
#include <memory>

class QDockWidget;
class QLineEdit;
class QTimer;
class QTreeWidgetItem;

namespace Ui { class MainWindow; }
namespace RibbonWidget { class Ribbon; }

namespace PartManager
{

	class KicadPreviewWidget;
	class MeshCacheBuilder;
	class Model3DViewer;
	class TagFilterButton;
	class PartlistPanel;

	class MainWindow : public QMainWindow
	{
		Q_OBJECT
	public:
		explicit MainWindow(std::unique_ptr<DatabaseHandle> handle, QWidget* parent = nullptr);
		~MainWindow() override;

		// §1b: the database the context menu's Switch Database picked, or null on a normal quit.
		// The window closes itself after a switch and main() builds a new one on what comes back
		// out of here — an in-process restart rather than a live handle swap, because every open
		// dialog, the mesh builder and the partlist panel hold a raw DatabaseHandle* taken from
		// this one, and none of them would notice it being exchanged underneath them.
		std::unique_ptr<DatabaseHandle> takeSwitchTarget();

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
		// Parts tab's Edit Type Templates button (§2, §11) — what a part type declares.
		void onEditTypeTemplates();
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
		// Preview panel: hands the selected part's stored datasheet to the system PDF viewer.
		void onOpenDatasheet();
		// Home tab's 3D Viewer button (§13) — the selected part's 3D model: attach, view, remove.
		void onView3DModel();
		// Parts tab's Settings button (§9). Ends the session when a backup was restored, because
		// the file the handle was opened against is no longer the one on disk.
		void onSettings();
		// Context menu's Switch Database (§1b) — the selector again, then close and let main()
		// rebuild the window on whatever it opened.
		void onSwitchDatabase();
		// Context menu's Help. There is no user manual to open, so this says so and offers the
		// project page rather than pretending to a help system.
		void onHelp();
		// Context menu's About — version, Qt, the open database, the licence.
		void onAbout();
		// §9a: takes a snapshot when one is due. Fires on a timer while the app runs; the check
		// is cheap (a directory listing) so a short tick is fine and the interval stays honest
		// even if the machine slept through a due time.
		void onBackupTick();

	protected:
		// §9a: the "always on clean shutdown" snapshot. Also the only one a user who never leaves
		// the app running for six hours would ever get.
		void closeEvent(QCloseEvent* event) override;
		// §1b's "reachable anytime" menu. Reached only when no child widget claimed the click —
		// the partlist rows take Qt::CustomContextMenu, which accepts the event and stops it here.
		void contextMenuEvent(QContextMenuEvent* event) override;
		// Turns the browser dock's ✕ into "put it back": the dock has a close button so a floating
		// window can be dismissed the way every floating window can, but closing the browser
		// outright would leave the main window empty with no way to bring it back.
		bool eventFilter(QObject* watched, QEvent* event) override;

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

		// The §10 partlist flush and the §9a shutdown snapshot. Shared by closeEvent() and the
		// database switch, which is a shutdown of this database in every way that matters.
		void flushPendingWork();

		// Builds the Home/Parts tabs of §7 into the .ui file's ribbonToolBar.
		void buildRibbon();
		// Wires both §7a filter boxes to their debounce timers, and builds the tag filter
		// drop-down that writes `tag:` terms into the table box.
		void setupFilters();
		// Re-reads the tag vocabulary into the filter drop-down, after Manage Tags or a
		// database switch changed it.
		void reloadTagFilter();
		// Red border + tooltip when a filter box holds a malformed query; clears both when it doesn't.
		void markFilterError(QLineEdit* edit, const QString& error);
		// Adds one CategoryNode and its children under `parent` (nullptr = a tree root). While the
		// §7a tree filter is on, a node with no matches under it is skipped entirely.
		void addCategoryItem(const CategoryNode& node, QTreeWidgetItem* parent);
		// Tints the header of the column the table is sorted by. Qt's sort arrow is easy to miss
		// among a dozen headers, so the colour carries the same information more loudly.
		void highlightSortedColumn();
		// Rebuilds the table's dynamic header and rows for one category.
		void showParts(int typeId, const QString& typeName);

		Ui::MainWindow* m_ui;
		MainWindowController m_controller;
		// Non-owning view of the same connection m_controller holds open.
		StockController m_stock;
		RibbonWidget::Ribbon* m_ribbon = nullptr;
		// The §4 screen, hidden until the ribbon asks for it. Owned by m_partlistDock.
		PartlistPanel* m_partlistPanel = nullptr;
		// §7's tree/table/preview and §4's BOM panel, each in its own dock so the user decides
		// where they sit. Both owned by the window (Qt parent).
		QDockWidget* m_browserDock = nullptr;
		QDockWidget* m_partlistDock = nullptr;
		// §5a: the part's schematic symbol and PCB footprint, under its photo. Built in code
		// rather than in the .ui, which would need them promoted there first.
		KicadPreviewWidget* m_symbolPreview = nullptr;
		KicadPreviewWidget* m_footprintPreview = nullptr;
		// §13: the same part in 3D, under the other two. Shares the app's one STEP converter,
		// which also sweeps the database in the background so most models are ready before they
		// are ever selected.
		Model3DViewer* m_modelPreview = nullptr;
		MeshCacheBuilder* m_meshBuilder = nullptr;
		// §7a/§2d: ticks tags into the table filter box. Owned by the header layout (Qt parent).
		TagFilterButton* m_tagFilter = nullptr;
		// Which category the table currently shows, so an edit can re-render it in place.
		// The part the user last picked, kept across the table refills a stock write causes.
		int m_selectedPartId = 0;
		int m_currentTypeId = NoParentType;
		// §9a. Owned by the window (Qt parent), so it stops when the window goes.
		QTimer* m_backupTimer = nullptr;
		QString m_currentTypeName;
		// §1b, see takeSwitchTarget(). Non-null only between the user picking another database
		// and main() taking it back out.
		std::unique_ptr<DatabaseHandle> m_switchTarget;
		// The columns behind the table's current header — a dragged divider only reports a
		// section index, so this is what turns that back into a column key (§7b).
		std::vector<PartColumn> m_currentColumns;
	};

}
