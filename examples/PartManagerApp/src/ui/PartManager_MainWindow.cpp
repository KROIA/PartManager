#include "ui/PartManager_MainWindow.h"
#include "ui_PartManager_MainWindow.h"

#include "ui/PartManager_ManageTagsDialog.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "ui/PartManager_StockDialog.h"
#include "widgets/PartManager_TagChipDelegate.h"

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

		// §2d seeded the new part's tags inside insertPart(); opening the editor is what
		// shows the user that happened, and is where everything else about it gets filled in.
		PartEditorDialog editor(m_controller.handle(), dialog.createdPartId(), this);
		editor.exec();

		// A new part changes the tree's in-stock counts as well as the table.
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

	void MainWindow::showParts(int typeId, const QString& typeName)
	{
		// §7b: the header is per-category and comes from data, so it is built here rather
		// than in the .ui — the one part of this screen Designer genuinely cannot express.
		const QString filter = m_ui->tableFilterEdit->text();
		markFilterError(m_ui->tableFilterEdit, searchError(filter));

		std::vector<PartColumn> columns = m_controller.columnsFor(typeId);
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

		m_ui->partTable->resizeColumnsToContents();
		m_ui->partTable->horizontalHeader()->setStretchLastSection(true);
		m_ui->partsHeaderLabel->setText(tr("%1 — %n part(s)", "", static_cast<int>(rows.size())).arg(typeName));
		// Refilling the table drops the selection without always emitting the signal, and the
		// values behind a kept selection may have just changed anyway.
		updatePreview();
	}

	void MainWindow::updatePreview()
	{
		const PartPreview preview = m_controller.previewFor(selectedPartId());
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

		auto addButton = [this](RibbonWidget::RibbonButtonGroup* group, const QString& text,
			const QString& iconPath, void (MainWindow::*slot)())
		{
			RibbonWidget::RibbonButton* button =
				new RibbonWidget::RibbonButton(text, text, iconPath, true, group);
			connect(button, &QToolButton::clicked, this, slot);
		};

		// Import from Mouser has no icon yet — that one is still on the asset list.
		addButton(newGroup, tr("New Part"), QStringLiteral(":/icons/new-part.png"), &MainWindow::onNewPart);
		addButton(newGroup, tr("New Partlist"), QStringLiteral(":/icons/new-partlist.png"), &MainWindow::onNotImplemented);
		addButton(stockGroup, tr("Restock"), QStringLiteral(":/icons/restock.png"), &MainWindow::onRestock);
		addButton(stockGroup, tr("Take Out"), QStringLiteral(":/icons/take-out.png"), &MainWindow::onTakeOut);
		addButton(viewGroup, tr("Refresh"), QStringLiteral(":/icons/refresh.png"), &MainWindow::reloadCategories);
		addButton(viewGroup, tr("List / Grid"), QStringLiteral(":/icons/view-list.png"), &MainWindow::onNotImplemented);
		addButton(viewGroup, tr("3D Viewer"), QStringLiteral(":/icons/viewer-3d.png"), &MainWindow::onNotImplemented);
		addButton(manageGroup, tr("Edit Type Templates"), QStringLiteral(":/icons/edit-type-template.png"), &MainWindow::onNotImplemented);
		addButton(manageGroup, tr("Manage Tags"), QStringLiteral(":/icons/manage-tags.png"), &MainWindow::onManageTags);
		addButton(manageGroup, tr("Import from Mouser"), QString(), &MainWindow::onNotImplemented);
		addButton(filesGroup, tr("Attach File"), QStringLiteral(":/icons/attach-file.png"), &MainWindow::onNotImplemented);
		addButton(filesGroup, tr("Open Datasheet"), QStringLiteral(":/icons/open-datasheet.png"), &MainWindow::onNotImplemented);
#endif
	}

}
