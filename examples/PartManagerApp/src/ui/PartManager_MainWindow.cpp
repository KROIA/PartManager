#include "ui/PartManager_MainWindow.h"
#include "ui_PartManager_MainWindow.h"

#include "controllers/PartManager_PartEditorController.h"
#include "ui/PartManager_ColumnsDialog.h"
#include "ui/PartManager_ManageTagsDialog.h"
#include "ui/PartManager_MouserSearchDialog.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "ui/PartManager_OrderManagerDialog.h"
#include "services/PartManager_MeshCacheBuilder.h"
#include "widgets/PartManager_AttachmentIconPainter.h"
#include "widgets/PartManager_KicadPreviewWidget.h"
#include "widgets/PartManager_Model3DViewer.h"
#include "widgets/PartManager_PartlistPanel.h"
#include "ui/PartManager_KicadLibraryDialog.h"
#include "ui/PartManager_Model3DDialog.h"
#include "ui/PartManager_SettingsDialog.h"
#include "ui/PartManager_StockDialog.h"
#include "widgets/PartManager_TagChipDelegate.h"
#include "widgets/PartManager_TagFilterButton.h"
#include "widgets/PartManager_TypeIconPainter.h"

#include "backup/PartManager_BackupManager.h"
#include "search/PartManager_SearchQuery.h"
#include "settings/PartManager_Settings.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDockWidget>
#include <QBrush>
#include <QColor>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPixmapCache>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QUrl>

#include <fstream>
#include <iterator>

#if RIBBON_WIDGET_LIBRARY_AVAILABLE == 1
	#include "RibbonWidget.h"
#endif

namespace PartManager
{
	namespace
	{
		// Tree item data roles: the node's part_type id, and its raw name without the "(count)" suffix.
		constexpr int TypeIdRole = Qt::UserRole;
		constexpr int TypeNameRole = Qt::UserRole + 1;

		// A negative count means the book-keeping is off (§3) — it is shown, never hidden.
		const QColor NegativeStockColor(0xC0, 0x39, 0x2B);

		// The header tint on the column the table is sorted by. Dark enough to need light text,
		// so both are named together — a background set without its foreground is the classic way
		// to make a header unreadable under a dark theme.
		const QColor SortedColumnColor(0x2E, 0x6D, 0xA4);
		const QColor SortedColumnTextColor(0xFF, 0xFF, 0xFF);

		// Big enough to tell an SOIC from an electrolytic at a glance, small enough that the
		// table still reads as a table.
		constexpr int ThumbnailSize = 28;

		// The Files column's glyphs. Smaller than the thumbnail — four of them share one cell.
		// Big enough that a folded page corner and a pad row are actually distinguishable at a
		// glance; the row is 34 px tall, so this is what fits without stretching the grid.
		constexpr int AttachmentGlyphSize = 18;

		// A cell's numeric value for sorting, when it has one. Without it "10" sorts before "9",
		// which on a Stock column is not a quirk but a wrong answer.
		constexpr int SortKeyRole = Qt::UserRole + 20;

		// Sorts on SortKeyRole when both cells carry one, and case-insensitively otherwise —
		// QTableWidgetItem's own operator< is a case-*sensitive* string compare, which files every
		// lowercase part name after every uppercase one.
		class SortableItem : public QTableWidgetItem
		{
		public:
			explicit SortableItem(const QString& text) : QTableWidgetItem(text) {}

			bool operator<(const QTableWidgetItem& other) const override
			{
				const QVariant mine = data(SortKeyRole);
				const QVariant theirs = other.data(SortKeyRole);
				if (mine.isValid() && theirs.isValid())
				{
					return mine.toDouble() < theirs.toDouble();
				}
				return QString::compare(text(), other.text(), Qt::CaseInsensitive) < 0;
			}
		};

		// The number a cell sorts by, or an invalid QVariant when it is not numeric. Parses the
		// leading number so "4.7 kΩ" and "100 nF" still order sensibly within one unit — which is
		// the case that matters, since a column holds one attribute and therefore one unit.
		QVariant numericSortKey(const QString& text)
		{
			QString number;
			for (QChar c : text.trimmed())
			{
				if (c.isDigit() || c == '.' || c == '-' || c == '+'
					|| ((c == 'e' || c == 'E') && !number.isEmpty()))
				{
					number += c;
					continue;
				}
				break;
			}
			bool ok = false;
			const double value = number.toDouble(&ok);
			return ok ? QVariant(value) : QVariant();
		}

		// KiCad files are a few kB and read only when the selection changes, so slurping is fine.
		std::string readWholeFile(const QString& path)
		{
			std::ifstream stream(path.toStdString(), std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		// One scaled thumbnail per stored image, shared by every row and every refill. Without
		// the cache a category of a few hundred parts decodes a few hundred JPEGs on every
		// keystroke of the table filter. QPixmapCache is size-bounded and evicts itself.
		QPixmap thumbnailFor(const QString& path)
		{
			const QString key = QStringLiteral("pm-thumb-%1-%2").arg(ThumbnailSize).arg(path);
			QPixmap cached;
			if (QPixmapCache::find(key, &cached))
			{
				return cached;
			}
			QPixmap source(path);
			if (source.isNull())
			{
				// A null pixmap is cached too — a broken or non-image file must not be re-decoded
				// once per refill for the rest of the session.
				QPixmapCache::insert(key, source);
				return source;
			}
			const QPixmap scaled = source.scaled(ThumbnailSize, ThumbnailSize,
				Qt::KeepAspectRatio, Qt::SmoothTransformation);
			QPixmapCache::insert(key, scaled);
			return scaled;
		}
	}

	MainWindow::MainWindow(std::unique_ptr<DatabaseHandle> handle, QWidget* parent)
		: QMainWindow(parent)
		, m_ui(new Ui::MainWindow)
		, m_controller(std::move(handle))
		, m_stock(m_controller.handle())
	{
		m_ui->setupUi(this);
		buildRibbon();

		// The database name is user data (a folder name), so only the frame around it is translated.
		setWindowTitle(tr("PartManager — %1").arg(m_controller.databaseName()));
		m_ui->statusBar->showMessage(m_controller.pmdbPath());

		setupFilters();
		m_ui->partTable->setItemDelegateForColumn(0, new TagChipDelegate(this));
		// Rows are sized for the thumbnail whether or not a given part has one, so the grid does
		// not jump about as images are attached.
		m_ui->partTable->setIconSize(QSize(ThumbnailSize, ThumbnailSize));
		m_ui->partTable->verticalHeader()->setDefaultSectionSize(ThumbnailSize + 6);
		// §7b: click a header to sort, click again to reverse. Qt draws the arrow that says which
		// column and which way; highlightSortedColumn() adds the colour on top of it, because an
		// arrow in one header among a dozen is easy to lose.
		m_ui->partTable->setSortingEnabled(true);
		m_ui->partTable->horizontalHeader()->setSortIndicatorShown(true);
		m_ui->partTable->horizontalHeader()->setSectionsClickable(true);
		connect(m_ui->partTable->horizontalHeader(), &QHeaderView::sortIndicatorChanged,
			this, [this](int, Qt::SortOrder) { highlightSortedColumn(); });
		// Only the table grows with the window; both side panels keep their width and can be
		// collapsed to nothing, so the preview never eats the rows it is describing.
		m_ui->bodySplitter->setStretchFactor(0, 0);
		m_ui->bodySplitter->setStretchFactor(1, 1);
		m_ui->bodySplitter->setStretchFactor(2, 0);
		m_ui->bodySplitter->setSizes({ 220, 540, 240 });

		// §7's browser is a dock rather than the central widget, so it can be moved, floated or
		// stacked against the partlist panel — which is the whole reason that panel became a dock
		// too. Not closable: with both docks gone the window would be empty and unrecoverable.
		m_browserDock = new QDockWidget(tr("Component Browser"), this);
		m_browserDock->setObjectName(QStringLiteral("browserDock"));
		m_browserDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
		m_browserDock->setWidget(m_ui->bodySplitter);
		addDockWidget(Qt::LeftDockWidgetArea, m_browserDock);
		// Nothing is left in the middle. A visible-but-empty central widget keeps a stripe of grey
		// between the docks and refuses to shrink past its minimum size.
		m_ui->centralWidget->hide();

		// §5a: the symbol and footprint sit directly under the photo, so one glance at the
		// preview answers "is this the right package" without opening the editor or KiCad.
		m_symbolPreview = new KicadPreviewWidget(m_ui->previewPanel);
		m_footprintPreview = new KicadPreviewWidget(m_ui->previewPanel);
		for (KicadPreviewWidget* view : { m_symbolPreview, m_footprintPreview })
		{
			view->setMinimumHeight(70);
			view->setMaximumHeight(110);
		}
		m_symbolPreview->setToolTip(tr("The schematic symbol this part places in KiCad."));
		m_footprintPreview->setToolTip(tr("The PCB footprint this part places in KiCad."));
		{
			QHBoxLayout* kicadRow = new QHBoxLayout();
			kicadRow->addWidget(m_symbolPreview);
			kicadRow->addWidget(m_footprintPreview);
			// Straight after the photo, before the scrolling detail list.
			m_ui->previewLayout->insertLayout(
				m_ui->previewLayout->indexOf(m_ui->previewGraphicLabel) + 1, kicadRow);
		}

		// §13: the fourth drawing of the same part, under the other three. Orbitable in place —
		// the dialog is for attaching and replacing, not for the one look that answers "is this
		// the right package".
		m_meshBuilder = new MeshCacheBuilder(m_controller.handle(), this);
		m_modelPreview = new Model3DViewer(m_ui->previewPanel);
		m_modelPreview->setCacheBuilder(m_meshBuilder);
		m_modelPreview->setMinimumHeight(110);
		m_modelPreview->setMaximumHeight(170);
		m_modelPreview->setToolTip(tr("The 3D model this part places on the board. Drag to orbit."));
		// +2: the photo, then the symbol/footprint row that was just inserted after it.
		m_ui->previewLayout->insertWidget(
			m_ui->previewLayout->indexOf(m_ui->previewGraphicLabel) + 2, m_modelPreview);

		// §13's background sweep. Whatever it converts is reported on the status bar and nowhere
		// else — it is work the user did not ask for and must not be interrupted by.
		connect(m_meshBuilder, &MeshCacheBuilder::progressed, this,
			[this](int done, int total)
			{
				if (total <= 0)
				{
					m_ui->statusBar->showMessage(m_controller.pmdbPath());
					return;
				}
				m_ui->statusBar->showMessage(
					tr("Preparing 3D models — %1 of %2…").arg(done).arg(total));
			});
		// (A conversion finishing for the model currently on screen is picked up by the viewer
		// itself, which is already listening to the same builder for exactly that.)
		// After the window is up, not during startup: the first sweep reads every 3D model in the
		// database and there is no reason for that to sit between the user and their part list.
		QTimer::singleShot(3000, m_meshBuilder, &MeshCacheBuilder::rescan);

		// §4 lives here rather than in a pair of dialogs: the part table is the component browser
		// the old editor's part picker was missing, so a line is added by dragging a row down into
		// the panel. Hidden until the ribbon asks for it, so the Home tab is unchanged by default.
		m_partlistPanel = new PartlistPanel(m_controller.handle(), this);
		m_partlistDock = new QDockWidget(tr("Partlists"), this);
		m_partlistDock->setObjectName(QStringLiteral("partlistDock"));
		m_partlistDock->setWidget(m_partlistPanel);
		// Right by default, which is where the BOM wants to be while parts are dragged into it
		// from the table on the left. Hidden until the ribbon asks for it, as before.
		addDockWidget(Qt::RightDockWidgetArea, m_partlistDock);
		m_partlistDock->hide();
		connect(m_partlistPanel, &PartlistPanel::hideRequested, m_partlistDock, &QWidget::hide);
		connect(m_partlistPanel, &PartlistPanel::stockChanged, this, &MainWindow::reloadCategories);

		// The drag half of the same feature. DragOnly: the table itself accepts nothing, so a row
		// dropped back onto it does nothing rather than reordering the category.
		m_ui->partTable->setDragEnabled(true);
		m_ui->partTable->setDragDropMode(QAbstractItemView::DragOnly);

		connect(m_ui->categoryTree, &QTreeWidget::itemSelectionChanged,
			this, &MainWindow::onCategorySelectionChanged);
		// Double-clicking a row is the only way into the part editor (§12b's "part link").
		connect(m_ui->partTable, &QTableWidget::cellDoubleClicked,
			this, [this](int row, int) { onPartActivated(row); });
		connect(m_ui->partTable, &QTableWidget::itemSelectionChanged,
			this, &MainWindow::updatePreview);
		// The panel's two buttons are the ribbon actions again, aimed at the selected row.
		connect(m_ui->previewOpenButton, &QPushButton::clicked,
			this, [this]() { onPartActivated(m_ui->partTable->currentRow()); });
		connect(m_ui->previewTakeOutButton, &QPushButton::clicked, this, &MainWindow::onTakeOut);
		connect(m_ui->previewMouserButton, &QPushButton::clicked, this, &MainWindow::onOpenOnMouser);
		connect(m_ui->previewDatasheetButton, &QPushButton::clicked,
			this, &MainWindow::onOpenDatasheet);
		// §7b: dragging a header divider saves that column's width for the shown category.
		connect(m_ui->partTable->horizontalHeader(), &QHeaderView::sectionResized,
			this, &MainWindow::onColumnResized);

		// §9a. A 15-minute tick rather than a timer set to the configured interval: the check is
		// a directory listing, and a machine that slept through a due time still gets its
		// snapshot at the next tick instead of waiting a full interval from wake-up.
		m_backupTimer = new QTimer(this);
		m_backupTimer->setInterval(15 * 60 * 1000);
		connect(m_backupTimer, &QTimer::timeout, this, &MainWindow::onBackupTick);
		m_backupTimer->start();
		// One on the way in as well, so a database that has not been opened in weeks is snapshot
		// before the user starts editing it, not after.
		onBackupTick();

		reloadCategories();
	}

	MainWindow::~MainWindow()
	{
#if RIBBON_WIDGET_LIBRARY_AVAILABLE == 1
		delete m_ribbon;
#endif
		delete m_ui;
	}

	void MainWindow::onNotImplemented()
	{
		// Ribbon actions land in later slices; the buttons exist so the shell matches §7.
	}

	void MainWindow::onGenerateKicadLibraries()
	{
		KicadLibraryDialog dialog(m_controller.handle(), this);
		dialog.exec();
	}

	void MainWindow::onOpenOnMouser()
	{
		const int partId = selectedPartId();
		if (partId == 0)
		{
			return;
		}
		PartEditorController editor(m_controller.handle());
		const std::string url = PartEditorController::mouserPageUrl(
			editor.mouserPartNumber(partId), editor.mouserUrl(partId));
		if (url.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
	}

	void MainWindow::onOpenDatasheet()
	{
		// Read again rather than caching the path from updatePreview(): the file can be detached
		// in the editor while the same row stays selected, and an opened-from-stale-path viewer
		// would then show a datasheet the part no longer has.
		const QString path = m_controller.previewFor(selectedPartId()).datasheetPath;
		if (path.isEmpty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	}

	void MainWindow::onView3DModel()
	{
		QString name;
		const int partId = selectedPartId(&name);
		if (partId == 0)
		{
			QMessageBox::information(this, tr("No part selected"),
				tr("Select a part first — the 3D viewer shows that part's model."));
			return;
		}
		// Opens whether or not the part has a model: attaching one is the same screen, because
		// the first thing anyone does after attaching is check it is the right file.
		Model3DDialog dialog(m_controller.handle(), m_meshBuilder, partId, name, this);
		// A model attached in there is a new STEP file to tessellate and a new glyph in the Files
		// column, neither of which the dialog can do on its own.
		connect(&dialog, &Model3DDialog::modelChanged, this, [this]()
			{
				if (m_meshBuilder != nullptr) { m_meshBuilder->rescan(); }
				refreshCurrentCategory();
				updatePreview();
			});
		dialog.exec();
	}

	void MainWindow::onSettings()
	{
		SettingsDialog dialog(m_controller.handle(), this);
		dialog.exec();
		if (dialog.restoredFromBackup())
		{
			// The handle is closed and the file behind it is a different one now. Carrying on
			// would show the restored database's name over the old database's data.
			close();
			return;
		}
		// A changed theme repaints itself, but the tree/table brushes were built against the old
		// palette — re-render so the rows match the rest of the window.
		reloadCategories();
	}

	void MainWindow::onBackupTick()
	{
		const AppPreferences preferences = Settings::getPreferences();
		if (!preferences.backupsEnabled)
		{
			return;
		}
		const std::string databasePath = m_controller.handle() != nullptr
			? m_controller.handle()->databaseFilePath() : std::string();
		if (databasePath.empty()
			|| !BackupManager::isSnapshotDue(databasePath, preferences.backupFolder,
				preferences.backupIntervalHours))
		{
			return;
		}
		// Silent on success and on failure alike: a backup is not something to interrupt the user
		// about, and a modal every 15 minutes because a folder is read-only would be worse than
		// the missing snapshot. The Settings dialog's list is where the truth is visible.
		BackupManager::createSnapshot(databasePath, preferences.backupFolder,
			preferences.backupRetentionCount);
	}

	void MainWindow::closeEvent(QCloseEvent* event)
	{
		// The partlist panel is a widget, not a dialog, so nothing else flushes its §10 debounce.
		if (m_partlistPanel != nullptr)
		{
			m_partlistPanel->flushPendingEdits();
		}

		// §9a's "always on clean shutdown" snapshot. Unconditional rather than due-based: this is
		// the last chance to capture the session's edits, and it costs one file copy.
		const AppPreferences preferences = Settings::getPreferences();
		if (preferences.backupsEnabled && m_controller.handle() != nullptr
			&& m_controller.handle()->isOpen())
		{
			BackupManager::createSnapshot(m_controller.handle()->databaseFilePath(),
				preferences.backupFolder, preferences.backupRetentionCount);
		}
		QMainWindow::closeEvent(event);
	}

	void MainWindow::setupFilters()
	{
		// §7a's two boxes are independent: the tree one re-counts every category, the table
		// one filters the selected category's rows. Both debounce so a burst of keystrokes
		// costs one query round, not one per character.
		auto wire = [this](QLineEdit* edit, void (MainWindow::*slot)())
		{
			QTimer* timer = new QTimer(this);
			timer->setSingleShot(true);
			timer->setInterval(200);
			connect(timer, &QTimer::timeout, this, slot);
			connect(edit, &QLineEdit::textChanged, timer, qOverload<>(&QTimer::start));
		};
		wire(m_ui->treeFilterEdit, &MainWindow::reloadCategories);
		wire(m_ui->tableFilterEdit, &MainWindow::refreshCurrentCategory);

		// §2d's tag picker sits beside the table box and writes into it, rather than filtering on
		// its own: one pipeline, and the user can see and hand-edit what the ticks produced.
		m_tagFilter = new TagFilterButton(m_ui->partsPanel);
		m_ui->partsHeaderLayout->insertWidget(
			m_ui->partsHeaderLayout->indexOf(m_ui->tableFilterEdit), m_tagFilter);
		connect(m_tagFilter, &TagFilterButton::selectionChanged, this, [this]()
		{
			m_ui->tableFilterEdit->setText(QString::fromStdString(SearchQuery::withTagGroups(
				m_ui->tableFilterEdit->text().toStdString(), m_tagFilter->selectedGroups())));
		});
		reloadTagFilter();
	}

	void MainWindow::reloadTagFilter()
	{
		if (m_tagFilter == nullptr)
		{
			return;
		}
		const PartEditorController tags(m_controller.handle());
		m_tagFilter->setVocabulary(tags.tagCategories(), tags.allTags());
	}

	void MainWindow::markFilterError(QLineEdit* edit, const QString& error)
	{
		// The Designer tooltip is the syntax help; stash it once so the error can borrow the slot.
		if (!edit->property("syntaxHelp").isValid())
		{
			edit->setProperty("syntaxHelp", edit->toolTip());
		}
		const QString help = edit->property("syntaxHelp").toString();

		edit->setStyleSheet(error.isEmpty() ? QString() : QStringLiteral("border: 1px solid #c0392b;"));
		edit->setToolTip(error.isEmpty() ? help : tr("Invalid query: %1").arg(error) + "\n\n" + help);
	}

	void MainWindow::reloadCategories()
	{
		const QString filter = m_ui->treeFilterEdit->text();
		markFilterError(m_ui->treeFilterEdit, searchError(filter));

		// Rebuilding the tree resets the selection, which would yank the table out from under
		// the user on every debounced keystroke — so the current category is re-selected.
		const int previousTypeId = m_currentTypeId;

		m_ui->categoryTree->clear();
		for (const CategoryNode& root : m_controller.categoryTree(filter))
		{
			addCategoryItem(root, nullptr);
		}
		m_ui->categoryTree->expandAll();

		QTreeWidgetItem* target = nullptr;
		for (QTreeWidgetItemIterator it(m_ui->categoryTree); *it && !target; ++it)
		{
			if ((*it)->data(0, TypeIdRole).toInt() == previousTypeId)
			{
				target = *it;
			}
		}
		// Open on the first category rather than an empty table — the mockup shows a
		// selected category, and there is nothing else the Home tab could usefully show.
		if (!target)
		{
			target = m_ui->categoryTree->topLevelItem(0);
		}
		if (target)
		{
			m_ui->categoryTree->setCurrentItem(target);
		}
	}

	void MainWindow::addCategoryItem(const CategoryNode& node, QTreeWidgetItem* parent)
	{
		const bool filtering = !m_ui->treeFilterEdit->text().trimmed().isEmpty();

		// While the filter is on, a branch with no hits anywhere under it is dropped rather than
		// shown at zero — the tree becomes the shape of the result, not the whole catalogue with
		// most of it reading "(0)". matchCount already includes every descendant, so a category
		// that only matches through a child survives here and its child is kept below.
		if (filtering && node.matchCount == 0)
		{
			return;
		}

		QTreeWidgetItem* item = parent
			? new QTreeWidgetItem(parent)
			: new QTreeWidgetItem(m_ui->categoryTree);

		// §7a: `Category (parts)`, or `Category (parts : matches)` while the tree filter is
		// active. The count is every part in the category, not only the ones in stock — with
		// in-stock, creating a part with no opening quantity left the number unchanged, so the
		// tree appeared not to have noticed. In stock moved to the tooltip, where it is still
		// one hover away.
		item->setText(0, filtering
			? tr("%1 (%2 : %3)").arg(node.name).arg(node.partCount).arg(node.matchCount)
			: tr("%1 (%2)").arg(node.name).arg(node.partCount));
		item->setToolTip(0, tr("%n part(s) in this category and below", "", node.partCount)
			+ QLatin1Char('\n')
			+ tr("%n of them in stock", "", node.inStockCount));
		item->setData(0, TypeIdRole, node.typeId);
		item->setData(0, TypeNameRole, node.name);

		for (const CategoryNode& child : node.children)
		{
			addCategoryItem(child, item);
		}
	}

	void MainWindow::onCategorySelectionChanged()
	{
		QTreeWidgetItem* item = m_ui->categoryTree->currentItem();
		if (!item || !item->isSelected())
		{
			m_ui->partTable->clearContents();
			m_ui->partTable->setRowCount(0);
			m_ui->partsHeaderLabel->setText(tr("Select a category"));
			updatePreview();
			return;
		}

		// The label carries the raw type name, not the "(count)" text the tree item shows.
		m_currentTypeId = item->data(0, TypeIdRole).toInt();
		m_currentTypeName = item->data(0, TypeNameRole).toString();
		showParts(m_currentTypeId, m_currentTypeName);
	}

	void MainWindow::refreshCurrentCategory()
	{
		if (m_currentTypeId != NoParentType)
		{
			showParts(m_currentTypeId, m_currentTypeName);
		}
	}

	void MainWindow::onPartActivated(int row)
	{
		// The part id rides on the first cell, the same one the chips are attached to.
		QTableWidgetItem* item = m_ui->partTable->item(row, 0);
		if (!item)
		{
			return;
		}
		const int partId = item->data(Qt::UserRole).toInt();
		if (partId == 0)
		{
			return;
		}

		PartEditorDialog editor(m_controller.handle(), partId, this);
		editor.exec();
		// The editor autosaved as it went (§10), so the table is stale by the time it closes — and
		// since the quantity field is editable there, the tree's in-stock counts can be too.
		reloadCategories();
	}

	void MainWindow::onNewPart()
	{
		NewPartDialog dialog(m_controller.handle(), this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}
		openNewPart(dialog.createdPartId(), QString());
	}

	void MainWindow::onNewPartFromMouser()
	{
		MouserSearchDialog search(this);
		if (search.exec() != QDialog::Accepted)
		{
			return;
		}

		// Same form as the manual flow, only pre-populated — §6 is "auto-fill what's possible,
		// correct the rest", so the user still confirms every field and presses Create.
		const MouserPartPrefill& prefill = search.selectedPrefill();
		NewPartDialog dialog(m_controller.handle(), this);
		dialog.setPrefill(prefill);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}
		openNewPart(dialog.createdPartId(), QString::fromStdString(prefill.datasheetUrl));
	}

	void MainWindow::openNewPart(int partId, const QString& datasheetUrl)
	{
		if (partId == 0)
		{
			return;
		}

		// The §6 datasheet download already happened inside New Part, where every other pending
		// attachment is applied — doing it again here would fetch the same file twice. The URL is
		// still handed to the editor, so a retry after a dead link costs one click.

		// §2d seeded the new part's tags inside insertPart(); opening the editor is what
		// shows the user that happened, and is where everything else about it gets filled in.
		PartEditorDialog editor(m_controller.handle(), partId, this);
		editor.setDatasheetSourceUrl(datasheetUrl);
		editor.exec();

		// A new part changes the tree's in-stock counts as well as the table.
		reloadCategories();
	}

	void MainWindow::updateKicadPreviews(const PartPreview& preview)
	{
		if (!m_symbolPreview || !m_footprintPreview) { return; }

		if (preview.partId == 0)
		{
			m_symbolPreview->showMessage(QString());
			m_footprintPreview->showMessage(QString());
			m_symbolPreview->setCaption(QString());
			m_footprintPreview->setCaption(QString());
			if (m_modelPreview != nullptr)
			{
				m_modelPreview->setFootprint(KicadDrawing());
				// An empty path clears it — the viewer says "no 3D model" rather than leaving
				// the previous part's shape on screen.
				m_modelPreview->showModel(QString());
			}
			return;
		}

		if (!preview.kicadSymbolPath.isEmpty())
		{
			const KicadDrawing drawing = KicadGeometry::symbol(
				readWholeFile(preview.kicadSymbolPath), preview.name.toStdString());
			m_symbolPreview->showDrawing(drawing, tr("Symbol file cannot be drawn."));
			m_symbolPreview->setCaption(QString::fromStdString(drawing.name));
		}
		else
		{
			// Not an error: most parts have no symbol of their own, and this is exactly what
			// "Generate Libraries" would put in the library for them.
			m_symbolPreview->showDrawing(
				KicadGeometry::genericSymbolForType(preview.typeName.toStdString()),
				tr("No symbol."));
			m_symbolPreview->setCaption(tr("generated"));
		}

		if (!preview.kicadFootprintPath.isEmpty())
		{
			const KicadDrawing drawing =
				KicadGeometry::footprint(readWholeFile(preview.kicadFootprintPath));
			m_footprintPreview->showDrawing(drawing, tr("Footprint file cannot be drawn."));
			m_footprintPreview->setCaption(QString::fromStdString(drawing.name));
			// The same pads, under the 3D model — which is the one view that shows whether the
			// model and the footprint actually agree about where the part sits.
			if (m_modelPreview != nullptr) { m_modelPreview->setFootprint(drawing); }
		}
		else
		{
			m_footprintPreview->showMessage(tr("No footprint."));
			m_footprintPreview->setCaption(QString());
			if (m_modelPreview != nullptr) { m_modelPreview->setFootprint(KicadDrawing()); }
		}

		// Last, so the board is built once from the final footprint and the camera is framed
		// against both it and the model rather than against whichever arrived first.
		if (m_modelPreview != nullptr)
		{
			m_modelPreview->showModel(preview.model3DPath);
		}
	}

	void MainWindow::showPartlistPanel()
	{
		if (m_partlistDock == nullptr)
		{
			return;
		}
		const bool wasHidden = m_partlistDock->isHidden();
		m_partlistDock->show();
		// Tabbed behind the browser after a user drag, showing it is not enough to see it.
		m_partlistDock->raise();
		if (wasHidden)
		{
			// A freshly shown dock gets whatever its size hint asks for, which for a grid is
			// nearly nothing. Two fifths of the window is enough rows to work in while leaving
			// the browser usable; after that the user's own drag wins.
			resizeDocks({ m_partlistDock }, { width() * 2 / 5 }, Qt::Horizontal);
		}
	}

	void MainWindow::onNewPartlist()
	{
		// Creating and opening in one step, the same shortcut New Part takes — an empty list
		// named "New partlist" is nothing anyone wants to look at in an overview first.
		showPartlistPanel();
		m_partlistPanel->createPartlist();
	}

	void MainWindow::onImportPartlist()
	{
		showPartlistPanel();
		m_partlistPanel->importPartlist();
	}

	void MainWindow::onManagePartlists()
	{
		// The panel's own selector is the overview the manager dialog used to be — one screen
		// instead of a popup that opened a second popup.
		showPartlistPanel();
		m_partlistPanel->reloadPartlists();
	}

	void MainWindow::onManageOrders()
	{
		OrderManagerDialog dialog(m_controller.handle(), this);
		dialog.exec();
		// Confirming an arrival restocks, so the counts in the tree and the table have moved.
		reloadCategories();
	}

	void MainWindow::onManageTags()
	{
		ManageTagsDialog dialog(m_controller.handle(), this);
		dialog.exec();
		// A renamed/recoloured/deleted tag changes the chips painted in the table, and the
		// vocabulary the filter drop-down offers.
		reloadTagFilter();
		refreshCurrentCategory();
	}

	int MainWindow::selectedPartId(QString* outName) const
	{
		// Same cell the part id and the chips ride on — the table selects whole rows.
		QTableWidgetItem* item = m_ui->partTable->item(m_ui->partTable->currentRow(), 0);
		if (!item || !item->isSelected())
		{
			return 0;
		}
		if (outName)
		{
			*outName = item->text();
		}
		return item->data(Qt::UserRole).toInt();
	}

	void MainWindow::onRestock()
	{
		changeStock(true);
	}

	void MainWindow::onTakeOut()
	{
		changeStock(false);
	}

	void MainWindow::changeStock(bool restocking)
	{
		QString partName;
		const int partId = selectedPartId(&partName);
		if (partId == 0)
		{
			// The buttons stay enabled: "nothing is selected" is worth saying once, and there is
			// no other state in which they would be greyed out.
			QMessageBox::information(this, restocking ? tr("Restock") : tr("Take Out"),
				tr("Select a part in the table first."));
			return;
		}

		StockDialog dialog(partName, m_stock.quantity(partId),
			restocking ? StockDialog::Mode::Restock : StockDialog::Mode::TakeOut, this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}

		const bool written = restocking
			? m_stock.restock(partId, dialog.quantity(), dialog.note())
			: m_stock.takeOut(partId, dialog.quantity(), dialog.note(), dialog.reason());
		if (!written)
		{
			QMessageBox::warning(this, tr("Stock unchanged"),
				tr("The database rejected the change — nothing was recorded."));
			return;
		}

		// The quantity moved, so both the table's Stock column and the tree's in-stock counts
		// are stale; reloadCategories() re-selects the same category and re-renders the rows.
		reloadCategories();
	}

	void MainWindow::onCustomizeColumns()
	{
		if (m_currentTypeId == NoParentType)
		{
			QMessageBox::information(this, tr("Customize Columns"), tr("Select a category first."));
			return;
		}

		ColumnsDialog dialog(m_currentTypeName, m_controller.allColumnsFor(m_currentTypeId), this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}

		// "Restore Defaults" drops the saved rows rather than saving a layout that looks default —
		// the category then derives its columns from its attributes again (§7b).
		if (dialog.resetRequested())
		{
			m_controller.resetColumns(m_currentTypeId);
		}
		else
		{
			m_controller.saveColumns(m_currentTypeId, dialog.columns());
		}
		refreshCurrentCategory();
	}

	void MainWindow::onColumnResized(int logicalIndex, int oldSize, int newSize)
	{
		Q_UNUSED(oldSize);
		if (m_currentTypeId == NoParentType
			|| logicalIndex < 0 || logicalIndex >= static_cast<int>(m_currentColumns.size()))
		{
			return;
		}
		// The last section stretches to fill the table, so its width is the window's, not the
		// user's — saving it would rewrite the layout on every window resize.
		if (logicalIndex == static_cast<int>(m_currentColumns.size()) - 1)
		{
			return;
		}

		m_currentColumns[static_cast<size_t>(logicalIndex)].widthPx = newSize;
		m_controller.saveColumnWidth(m_currentTypeId, m_currentColumns[static_cast<size_t>(logicalIndex)].key,
			newSize);
	}

	void MainWindow::showParts(int typeId, const QString& typeName)
	{
		// §7b: the header is per-category and comes from data, so it is built here rather
		// than in the .ui — the one part of this screen Designer genuinely cannot express.
		const QString filter = m_ui->tableFilterEdit->text();
		markFilterError(m_ui->tableFilterEdit, searchError(filter));
		// The box is the source of truth, so a hand-typed or cleared `tag:` term ticks the
		// picker rather than the two disagreeing. setSelectedGroups() emits nothing, so this
		// cannot bounce back into the box.
		if (m_tagFilter != nullptr)
		{
			m_tagFilter->setSelectedGroups(SearchQuery::parse(filter.toStdString()).tagGroups);
		}

		m_currentColumns = m_controller.columnsFor(typeId);
		const std::vector<PartColumn>& columns = m_currentColumns;
		std::vector<PartRow> rows = m_controller.partsFor(typeId, columns, filter);

		// Sorting off while the table is filled: with it on, every setItem() re-sorts what is
		// already there and the rows land in an order that has nothing to do with the loop below.
		// The indicator is remembered and re-applied, so a refresh keeps the user's chosen order.
		QHeaderView* const sortHeader = m_ui->partTable->horizontalHeader();
		const int previousSortColumn = sortHeader->sortIndicatorSection();
		const Qt::SortOrder previousSortOrder = sortHeader->sortIndicatorOrder();
		m_ui->partTable->setSortingEnabled(false);

		m_ui->partTable->clearContents();
		m_ui->partTable->setColumnCount(static_cast<int>(columns.size()));
		QStringList headers;
		for (const PartColumn& column : columns)
		{
			headers.append(column.label);
		}
		m_ui->partTable->setHorizontalHeaderLabels(headers);
		m_ui->partTable->setRowCount(static_cast<int>(rows.size()));

		for (int rowIndex = 0; rowIndex < static_cast<int>(rows.size()); ++rowIndex)
		{
			const PartRow& row = rows[static_cast<size_t>(rowIndex)];
			for (int columnIndex = 0; columnIndex < row.cells.size(); ++columnIndex)
			{
				QTableWidgetItem* cell = new SortableItem(row.cells.at(columnIndex)); // user data
				const QString& columnKey = columns[static_cast<size_t>(columnIndex)].key;
				if (columnKey == "files")
				{
					// Glyphs only — the cell's text is empty by design (see formatCell). It still
					// sorts: the flag set is the key, so one click groups the parts that have
					// nothing attached, which is the list worth working through.
					cell->setData(Qt::DecorationRole, AttachmentIconPainter::strip(
						row.attachments, AttachmentGlyphSize, devicePixelRatioF()));
					cell->setData(SortKeyRole, row.attachments);
					cell->setToolTip(AttachmentIconPainter::describe(row.attachments));
					cell->setTextAlignment(Qt::AlignCenter);
				}
				else
				{
					const QVariant sortKey = numericSortKey(row.cells.at(columnIndex));
					if (sortKey.isValid())
					{
						cell->setData(SortKeyRole, sortKey);
					}
				}
				if (columnIndex == 0)
				{
					// §2d chips ride along on the name cell; TagChipDelegate paints them.
					QVariantList tags;
					for (const Tag& tag : row.tags)
					{
						tags.append(QStringList{ QString::fromStdString(tag.name),
							QString::fromStdString(tag.color) });
					}
					cell->setData(TagChipDelegate::TagsRole, tags);
					cell->setData(Qt::UserRole, row.partId);
					// The product photo, so a part is identifiable without reading the row. It is
					// just another attachment (role='image'), set in New Part or the editor.
					QPixmap thumbnail;
					if (!row.imagePath.isEmpty())
					{
						thumbnail = thumbnailFor(row.imagePath);
					}
					if (thumbnail.isNull())
					{
						// No photo is the normal state — a CSV import brings none at all — so the
						// column falls back to a coloured glyph for the part's type rather than
						// leaving a blank that makes every such row look alike.
						thumbnail = TypeIconPainter::icon(row.typeName, ThumbnailSize,
							devicePixelRatioF());
					}
					if (!thumbnail.isNull())
					{
						cell->setData(Qt::DecorationRole, thumbnail);
					}
				}
				if (columnKey == "stock_qty" && row.stockQty < 0)
				{
					// §3 lets stock go negative; a plain black number would hide that.
					cell->setForeground(QBrush(NegativeStockColor));
				}
				m_ui->partTable->setItem(rowIndex, columnIndex, cell);
			}
		}

		// Laying out the header emits sectionResized for every column — unblocked, the app would
		// immediately save its own auto-fit widths back over the ones the user chose.
		QHeaderView* header = m_ui->partTable->horizontalHeader();
		{
			const QSignalBlocker blocker(header);
			m_ui->partTable->resizeColumnsToContents();
			for (int index = 0; index < static_cast<int>(columns.size()); ++index)
			{
				if (columns[static_cast<size_t>(index)].widthPx > 0)
				{
					m_ui->partTable->setColumnWidth(index, columns[static_cast<size_t>(index)].widthPx);
				}
			}
			header->setStretchLastSection(true);
		}

		// Back on, and put the user's order back. sortItems() rather than only restoring the
		// indicator: re-enabling sorting sets the arrow without actually reordering the new rows.
		m_ui->partTable->setSortingEnabled(true);
		if (previousSortColumn >= 0 && previousSortColumn < static_cast<int>(columns.size()))
		{
			m_ui->partTable->sortItems(previousSortColumn, previousSortOrder);
		}
		highlightSortedColumn();

		m_ui->partsHeaderLabel->setText(tr("%1 — %n part(s)", "", static_cast<int>(rows.size())).arg(typeName));

		// Reselect the same part if it is still in the list — it may have been filtered out, or
		// deleted, in which case the preview correctly falls back to its empty state. The id comes
		// from m_selectedPartId rather than from the table: a restock rebuilds the category tree,
		// and clearing the tree empties the table first, so by now the table itself has forgotten.
		if (m_selectedPartId != 0)
		{
			for (int rowIndex = 0; rowIndex < m_ui->partTable->rowCount(); ++rowIndex)
			{
				const QTableWidgetItem* cell = m_ui->partTable->item(rowIndex, 0);
				if (cell && cell->data(Qt::UserRole).toInt() == m_selectedPartId)
				{
					m_ui->partTable->selectRow(rowIndex);
					break;
				}
			}
		}
		// Refilling the table drops the selection without always emitting the signal, and the
		// values behind a kept selection may have just changed anyway.
		updatePreview();
	}

	void MainWindow::highlightSortedColumn()
	{
		QHeaderView* header = m_ui->partTable->horizontalHeader();
		const int sorted = header->isSortIndicatorShown() ? header->sortIndicatorSection() : -1;

		for (int column = 0; column < m_ui->partTable->columnCount(); ++column)
		{
			QTableWidgetItem* item = m_ui->partTable->horizontalHeaderItem(column);
			if (item == nullptr)
			{
				continue;
			}
			// A tint rather than a bold font: changing the weight re-measures the header and the
			// column jumps a few pixels wider every time the sort moves to it.
			if (column == sorted)
			{
				item->setBackground(QBrush(SortedColumnColor));
				item->setForeground(QBrush(SortedColumnTextColor));
			}
			else
			{
				// A default-constructed brush, not a palette colour — the header then paints
				// itself from the current style, so this still follows a theme change.
				item->setBackground(QBrush());
				item->setForeground(QBrush());
			}
		}
	}

	void MainWindow::updatePreview()
	{
		const int partId = selectedPartId();
		if (partId != 0)
		{
			// Only ever remember a real pick. A momentarily empty table (the tree is being rebuilt)
			// must not erase which part the user is looking at.
			m_selectedPartId = partId;
		}
		const PartPreview preview = m_controller.previewFor(partId);
		const bool hasPart = preview.partId != 0;

		m_ui->previewEmptyLabel->setVisible(!hasPart);
		m_ui->previewScroll->setVisible(hasPart);
		m_ui->previewOpenButton->setEnabled(hasPart);
		// §6's "Open on Mouser". Enabled only when the part actually has an article number, so a
		// disabled button is itself the hint that the Mouser P/N is missing.
		{
			PartEditorController editor(m_controller.handle());
			const std::string mouserNumber = hasPart ? editor.mouserPartNumber(preview.partId)
				: std::string();
			m_ui->previewMouserButton->setEnabled(!mouserNumber.empty());
			m_ui->previewMouserButton->setToolTip(mouserNumber.empty()
				? tr("This part has no Mouser part number. Open it in the part editor and fill in "
					 "the “Mouser P/N” field — it is what ordering needs, and it is not the MPN.")
				: tr("Opens %1 on mouser.com.").arg(QString::fromStdString(mouserNumber)));
		}
		m_ui->previewTakeOutButton->setEnabled(hasPart);
		// Greyed out means "there is no file", not "the button is broken" — so the tooltip says
		// which of the two it is rather than describing a button that cannot be pressed.
		m_ui->previewDatasheetButton->setEnabled(!preview.datasheetPath.isEmpty());
		m_ui->previewDatasheetButton->setToolTip(preview.datasheetPath.isEmpty()
			? tr("No datasheet is attached to this part. Open it in the part editor to attach or "
				 "download one.")
			: tr("Opens this part's datasheet in your PDF viewer."));

		// The part's photo at panel size — the same attachment the table shows a thumbnail of.
		// (KiCad previews follow below, once the empty state has been dealt with.)
		// Loaded straight rather than through thumbnailFor(), which caches at table scale. Set
		// before the empty-state return, so deselecting clears the previous part's picture
		// instead of leaving it under a blank panel.
		QPixmap graphic;
		if (!preview.imagePath.isEmpty())
		{
			graphic.load(preview.imagePath);
		}
		if (graphic.isNull())
		{
			// Same placeholder the table draws, at panel size — so the preview of a part with no
			// photo still says what kind of thing it is instead of "[ no image ]".
			m_ui->previewGraphicLabel->setText(QString());
			m_ui->previewGraphicLabel->setPixmap(hasPart
				? TypeIconPainter::icon(preview.typeName,
					m_ui->previewGraphicLabel->maximumHeight(), devicePixelRatioF())
				: QPixmap());
			m_ui->previewGraphicLabel->setToolTip(hasPart
				? tr("No photo yet — this is a placeholder for “%1”. Attach one in the part "
					 "editor, or import the part from Mouser, which downloads its product photo.")
					.arg(preview.typeName)
				: QString());
		}
		else
		{
			m_ui->previewGraphicLabel->setPixmap(graphic.scaled(
				m_ui->previewGraphicLabel->width(), m_ui->previewGraphicLabel->maximumHeight(),
				Qt::KeepAspectRatio, Qt::SmoothTransformation));
			m_ui->previewGraphicLabel->setToolTip(QString());
		}

		updateKicadPreviews(preview);

		if (!hasPart)
		{
			return;
		}

		m_ui->previewNameLabel->setText(preview.name); // user data
		m_ui->previewDescriptionLabel->setText(preview.description);
		m_ui->previewDescriptionLabel->setVisible(!preview.description.isEmpty());

		// §2d chips as rich text: the table needs a delegate because it paints inside a cell,
		// the panel just needs coloured runs of text — same colour rule, far less machinery.
		QStringList chips;
		for (const Tag& tag : preview.tags)
		{
			const QColor background(QString::fromStdString(tag.color));
			chips.append(QStringLiteral("<span style=\"background-color:%1; color:%2;\">&nbsp;%3&nbsp;</span>")
				.arg(background.isValid() ? background.name() : QStringLiteral("#CCCCCC"),
					background.isValid() && background.lightness() < 128
						? QStringLiteral("#FFFFFF") : QStringLiteral("#000000"),
					QString::fromStdString(tag.name).toHtmlEscaped())); // user data
		}
		m_ui->previewTagsLabel->setText(chips.join(QStringLiteral(" ")));
		m_ui->previewTagsLabel->setVisible(!chips.isEmpty());

		// §7c: the field list is per-part, so it is built here rather than in the .ui — same
		// reason the table's header is.
		while (m_ui->previewFormLayout->rowCount() > 0)
		{
			m_ui->previewFormLayout->removeRow(0);
		}
		for (const PreviewField& field : preview.fields)
		{
			QLabel* value = new QLabel(field.value, m_ui->previewContent); // already display-ready
			value->setWordWrap(true);
			value->setTextInteractionFlags(Qt::TextSelectableByMouse);
			m_ui->previewFormLayout->addRow(tr("%1:").arg(field.label), value);
		}
	}

	void MainWindow::buildRibbon()
	{
#if RIBBON_WIDGET_LIBRARY_AVAILABLE == 1
		m_ribbon = new RibbonWidget::Ribbon(m_ui->ribbonToolBar);

		// Tabs, groups and buttons all register themselves with the parent passed to their
		// constructor — calling addTab()/addGroup()/addButton() on top of that adds them twice.
		RibbonWidget::RibbonTab* homeTab =
			new RibbonWidget::RibbonTab(tr("Home"), QStringLiteral(":/icons/tab-home.png"), m_ribbon);
		RibbonWidget::RibbonTab* partsTab =
			new RibbonWidget::RibbonTab(tr("Parts"), QStringLiteral(":/icons/tab-parts.png"), m_ribbon);

		RibbonWidget::RibbonButtonGroup* newGroup = new RibbonWidget::RibbonButtonGroup(tr("New"), homeTab);
		RibbonWidget::RibbonButtonGroup* stockGroup = new RibbonWidget::RibbonButtonGroup(tr("Stock"), homeTab);
		RibbonWidget::RibbonButtonGroup* viewGroup = new RibbonWidget::RibbonButtonGroup(tr("View"), homeTab);
		RibbonWidget::RibbonButtonGroup* manageGroup = new RibbonWidget::RibbonButtonGroup(tr("Manage"), partsTab);
		RibbonWidget::RibbonButtonGroup* filesGroup = new RibbonWidget::RibbonButtonGroup(tr("Files"), partsTab);
		RibbonWidget::RibbonButtonGroup* kicadGroup = new RibbonWidget::RibbonButtonGroup(tr("KiCad"), partsTab);

		auto addButton = [this](RibbonWidget::RibbonButtonGroup* group, const QString& text,
			const QString& iconPath, void (MainWindow::*slot)())
		{
			RibbonWidget::RibbonButton* button =
				new RibbonWidget::RibbonButton(text, text, iconPath, true, group);
			connect(button, &QToolButton::clicked, this, slot);
		};

		// Import from Mouser has no icon yet — that one is still on the asset list.
		addButton(newGroup, tr("New Part"), QStringLiteral(":/icons/new-part.png"), &MainWindow::onNewPart);
		addButton(newGroup, tr("New Partlist"), QStringLiteral(":/icons/new-partlist.png"), &MainWindow::onNewPartlist);
		addButton(newGroup, tr("Import CSV / BOM"), QStringLiteral(":/icons/import-csv.png"), &MainWindow::onImportPartlist);
		addButton(newGroup, tr("Partlists"), QStringLiteral(":/icons/view-list.png"), &MainWindow::onManagePartlists);
		addButton(newGroup, tr("Orders"), QStringLiteral(":/icons/orders.png"), &MainWindow::onManageOrders);
		addButton(stockGroup, tr("Restock"), QStringLiteral(":/icons/restock.png"), &MainWindow::onRestock);
		addButton(stockGroup, tr("Take Out"), QStringLiteral(":/icons/take-out.png"), &MainWindow::onTakeOut);
		addButton(viewGroup, tr("Refresh"), QStringLiteral(":/icons/refresh.png"), &MainWindow::reloadCategories);
		addButton(viewGroup, tr("Customize Columns"), QStringLiteral(":/icons/tabelle.png"), &MainWindow::onCustomizeColumns);
		addButton(viewGroup, tr("List / Grid"), QStringLiteral(":/icons/view-list.png"), &MainWindow::onNotImplemented);
		addButton(viewGroup, tr("3D Viewer"), QStringLiteral(":/icons/viewer-3d.png"), &MainWindow::onView3DModel);
		addButton(manageGroup, tr("Edit Type Templates"), QStringLiteral(":/icons/edit-type-template.png"), &MainWindow::onNotImplemented);
		addButton(manageGroup, tr("Manage Tags"), QStringLiteral(":/icons/manage-tags.png"), &MainWindow::onManageTags);
		addButton(manageGroup, tr("Settings"), QStringLiteral(":/icons/settings.png"), &MainWindow::onSettings);
		addButton(manageGroup, tr("Import from Mouser"), QStringLiteral(":/icons/mouser-search.png"), &MainWindow::onNewPartFromMouser);
		addButton(kicadGroup, tr("Generate Libraries"), QStringLiteral(":/icons/viewer-3d.png"), &MainWindow::onGenerateKicadLibraries);
		addButton(filesGroup, tr("Attach File"), QStringLiteral(":/icons/attach-file.png"), &MainWindow::onNotImplemented);
		addButton(filesGroup, tr("Open Datasheet"), QStringLiteral(":/icons/open-datasheet.png"), &MainWindow::onNotImplemented);
#endif
	}

}
