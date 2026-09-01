#include "ui/PartManager_MainWindow.h"
#include "ui_PartManager_MainWindow.h"

#include "ui/PartManager_ManageTagsDialog.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"
#include "widgets/PartManager_TagChipDelegate.h"

#include <QHeaderView>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>

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
	}

	MainWindow::MainWindow(std::unique_ptr<DatabaseHandle> handle, QWidget* parent)
		: QMainWindow(parent)
		, m_ui(new Ui::MainWindow)
		, m_controller(std::move(handle))
	{
		m_ui->setupUi(this);
		buildRibbon();

		// The database name is user data (a folder name), so only the frame around it is translated.
		setWindowTitle(tr("PartManager — %1").arg(m_controller.databaseName()));
		m_ui->statusBar->showMessage(m_controller.pmdbPath());

		// TODO(core/search): both filter boxes need the §2a query engine (core/search), which
		// does not exist yet — they stay disabled rather than pretending to filter.
		m_ui->partTable->setItemDelegateForColumn(0, new TagChipDelegate(this));
		m_ui->bodySplitter->setStretchFactor(0, 0);
		m_ui->bodySplitter->setStretchFactor(1, 1);
		m_ui->bodySplitter->setSizes({ 240, 760 });

		connect(m_ui->categoryTree, &QTreeWidget::itemSelectionChanged,
			this, &MainWindow::onCategorySelectionChanged);
		// Double-clicking a row is the only way into the part editor (§12b's "part link").
		connect(m_ui->partTable, &QTableWidget::cellDoubleClicked,
			this, [this](int row, int) { onPartActivated(row); });

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

	void MainWindow::reloadCategories()
	{
		m_ui->categoryTree->clear();
		for (const CategoryNode& root : m_controller.categoryTree())
		{
			addCategoryItem(root, nullptr);
		}
		m_ui->categoryTree->expandAll();

		// Open on the first category rather than an empty table — the mockup shows a
		// selected category, and there is nothing else the Home tab could usefully show.
		if (QTreeWidgetItem* first = m_ui->categoryTree->topLevelItem(0))
		{
			m_ui->categoryTree->setCurrentItem(first);
		}
	}

	void MainWindow::addCategoryItem(const CategoryNode& node, QTreeWidgetItem* parent)
	{
		QTreeWidgetItem* item = parent
			? new QTreeWidgetItem(parent)
			: new QTreeWidgetItem(m_ui->categoryTree);

		// §7a: `Category (inStock)`. The category name is user data, only the frame is translated.
		item->setText(0, tr("%1 (%2)").arg(node.name).arg(node.inStockCount));
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
		// The editor autosaved as it went (§10), so the table is stale by the time it closes.
		refreshCurrentCategory();
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

	void MainWindow::showParts(int typeId, const QString& typeName)
	{
		// §7b: the header is per-category and comes from data, so it is built here rather
		// than in the .ui — the one part of this screen Designer genuinely cannot express.
		std::vector<PartColumn> columns = m_controller.columnsFor(typeId);
		std::vector<PartRow> rows = m_controller.partsFor(typeId, columns);

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
				m_ui->partTable->setItem(rowIndex, columnIndex, cell);
			}
		}

		m_ui->partTable->resizeColumnsToContents();
		m_ui->partTable->horizontalHeader()->setStretchLastSection(true);
		m_ui->partsHeaderLabel->setText(tr("%1 — %n part(s)", "", static_cast<int>(rows.size())).arg(typeName));
	}

	void MainWindow::buildRibbon()
	{
#if RIBBON_WIDGET_LIBRARY_AVAILABLE == 1
		m_ribbon = new RibbonWidget::Ribbon(m_ui->ribbonToolBar);

		// Tabs, groups and buttons all register themselves with the parent passed to their
		// constructor — calling addTab()/addGroup()/addButton() on top of that adds them twice.
		RibbonWidget::RibbonTab* homeTab = new RibbonWidget::RibbonTab(tr("Home"), QString(), m_ribbon);
		RibbonWidget::RibbonTab* partsTab = new RibbonWidget::RibbonTab(tr("Parts"), QString(), m_ribbon);

		RibbonWidget::RibbonButtonGroup* newGroup = new RibbonWidget::RibbonButtonGroup(tr("New"), homeTab);
		RibbonWidget::RibbonButtonGroup* stockGroup = new RibbonWidget::RibbonButtonGroup(tr("Stock"), homeTab);
		RibbonWidget::RibbonButtonGroup* viewGroup = new RibbonWidget::RibbonButtonGroup(tr("View"), homeTab);
		RibbonWidget::RibbonButtonGroup* manageGroup = new RibbonWidget::RibbonButtonGroup(tr("Manage"), partsTab);
		RibbonWidget::RibbonButtonGroup* filesGroup = new RibbonWidget::RibbonButtonGroup(tr("Files"), partsTab);

		auto addButton = [this](RibbonWidget::RibbonButtonGroup* group, const QString& text,
			void (MainWindow::*slot)())
		{
			// No icon set yet — the ribbon icon assets are a later slice (§12b resources/).
			RibbonWidget::RibbonButton* button =
				new RibbonWidget::RibbonButton(text, text, QString(), true, group);
			connect(button, &QToolButton::clicked, this, slot);
		};

		addButton(newGroup, tr("New Part"), &MainWindow::onNewPart);
		addButton(newGroup, tr("New Partlist"), &MainWindow::onNotImplemented);
		addButton(stockGroup, tr("Restock"), &MainWindow::onNotImplemented);
		addButton(stockGroup, tr("Take Out"), &MainWindow::onNotImplemented);
		// The only button this slice can actually satisfy — everything it needs already exists.
		addButton(viewGroup, tr("Refresh"), &MainWindow::reloadCategories);
		addButton(viewGroup, tr("List / Grid"), &MainWindow::onNotImplemented);
		addButton(viewGroup, tr("3D Viewer"), &MainWindow::onNotImplemented);
		addButton(manageGroup, tr("Edit Type Templates"), &MainWindow::onNotImplemented);
		addButton(manageGroup, tr("Manage Tags"), &MainWindow::onManageTags);
		addButton(manageGroup, tr("Import from Mouser"), &MainWindow::onNotImplemented);
		addButton(filesGroup, tr("Attach File"), &MainWindow::onNotImplemented);
		addButton(filesGroup, tr("Open Datasheet"), &MainWindow::onNotImplemented);
#endif
	}

}
