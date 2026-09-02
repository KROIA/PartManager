#include "ui/PartManager_MainWindow.h"
#include "ui_PartManager_MainWindow.h"

#include "controllers/PartManager_PartEditorController.h"
#include "ui/PartManager_ColumnsDialog.h"
#include "ui/PartManager_ManageTagsDialog.h"
#include "ui/PartManager_MouserSearchDialog.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "ui/PartManager_PartlistEditorDialog.h"
#include "ui/PartManager_PartlistImportDialog.h"
#include "ui/PartManager_PartlistManagerDialog.h"
#include "ui/PartManager_OrderManagerDialog.h"
#include "ui/PartManager_KicadLibraryDialog.h"
#include "ui/PartManager_Model3DDialog.h"
#include "ui/PartManager_SettingsDialog.h"
#include "ui/PartManager_StockDialog.h"
#include "widgets/PartManager_TagChipDelegate.h"

#include "backup/PartManager_BackupManager.h"
#include "settings/PartManager_Settings.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidgetItem>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>

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
		// Only the table grows with the window; both side panels keep their width and can be
		// collapsed to nothing, so the preview never eats the rows it is describing.
		m_ui->bodySplitter->setStretchFactor(0, 0);
		m_ui->bodySplitter->setStretchFactor(1, 1);
		m_ui->bodySplitter->setStretchFactor(2, 0);
		m_ui->bodySplitter->setSizes({ 220, 540, 240 });

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
		Model3DDialog dialog(m_controller.handle(), partId, name, this);
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
		QTreeWidgetItem* item = parent
			? new QTreeWidgetItem(parent)
			: new QTreeWidgetItem(m_ui->categoryTree);

		// §7a: `Category (inStock)`, or `Category (inStock : matches)` while the tree filter
		// is active. The category name is user data, only the frame is translated.
		item->setText(0, m_ui->treeFilterEdit->text().trimmed().isEmpty()
			? tr("%1 (%2)").arg(node.name).arg(node.inStockCount)
			: tr("%1 (%2 : %3)").arg(node.name).arg(node.inStockCount).arg(node.matchCount));
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

		// §6 datasheet auto-download. Silent on failure on purpose: Mouser publishes an empty
		// or dead DataSheetUrl often enough that a modal per miss would be noise, and the part
		// itself is unaffected either way. The URL is handed to the editor regardless, so the
		// Download button opens already holding it and a retry costs one click.
		if (!datasheetUrl.isEmpty())
		{
			PartEditorController controller(m_controller.handle());
			Part part;
			if (controller.loadPart(partId, part))
			{
				QApplication::setOverrideCursor(Qt::WaitCursor);
				const bool downloaded = controller.downloadDatasheet(part, datasheetUrl.toStdString()) != 0;
				QApplication::restoreOverrideCursor();
				if (downloaded)
				{
					controller.savePart(part);
				}
			}
		}

		// §2d seeded the new part's tags inside insertPart(); opening the editor is what
		// shows the user that happened, and is where everything else about it gets filled in.
		PartEditorDialog editor(m_controller.handle(), partId, this);
		editor.setDatasheetSourceUrl(datasheetUrl);
		editor.exec();

		// A new part changes the tree's in-stock counts as well as the table.
		reloadCategories();
	}

	void MainWindow::onNewPartlist()
	{
		// Creating and opening in one step, the same shortcut New Part takes — an empty list
		// named "New partlist" is nothing anyone wants to look at in the overview first.
		PartlistController controller(m_controller.handle());
		Partlist partlist;
		partlist.name = tr("New partlist").toStdString();
		partlist.source = PartlistSource::Manual;
		const int id = controller.create(partlist);
		if (id == NoPartlistId)
		{
			QMessageBox::warning(this, tr("Could not create the partlist"),
				tr("The database rejected the new partlist."));
			return;
		}
		PartlistEditorDialog editor(controller, id, this);
		editor.exec();
	}

	void MainWindow::onImportPartlist()
	{
		PartlistController controller(m_controller.handle());
		PartlistImportDialog import(controller, this);
		if (import.exec() != QDialog::Accepted)
		{
			return;
		}
		// Straight into the editor, like the other two partlist entry points: an import that
		// left rows unresolved is exactly what the user has to look at next.
		PartlistEditorDialog editor(controller, import.createdPartlistId(), this);
		editor.exec();
	}

	void MainWindow::onManagePartlists()
	{
		PartlistManagerDialog dialog(m_controller.handle(), this);
		dialog.exec();
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
		// A renamed/recoloured/deleted tag changes the chips painted in the table.
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

		m_currentColumns = m_controller.columnsFor(typeId);
		const std::vector<PartColumn>& columns = m_currentColumns;
		std::vector<PartRow> rows = m_controller.partsFor(typeId, columns, filter);

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
				QTableWidgetItem* cell = new QTableWidgetItem(row.cells.at(columnIndex)); // user data
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
				}
				if (columns[static_cast<size_t>(columnIndex)].key == "stock_qty" && row.stockQty < 0)
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
		m_ui->previewTakeOutButton->setEnabled(hasPart);
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
