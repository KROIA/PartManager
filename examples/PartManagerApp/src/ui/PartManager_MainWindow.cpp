#include "ui/PartManager_MainWindow.h"
#include "ui_PartManager_MainWindow.h"

#if RIBBON_WIDGET_LIBRARY_AVAILABLE == 1
	#include "RibbonWidget.h"
#endif

namespace PartManager
{

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

		auto addButton = [this](RibbonWidget::RibbonButtonGroup* group, const QString& text)
		{
			// No icon set yet — the ribbon icon assets are a later slice (§12b resources/).
			RibbonWidget::RibbonButton* button =
				new RibbonWidget::RibbonButton(text, text, QString(), true, group);
			connect(button, &QToolButton::clicked, this, &MainWindow::onNotImplemented);
		};

		addButton(newGroup, tr("New Part"));
		addButton(newGroup, tr("New Partlist"));
		addButton(stockGroup, tr("Restock"));
		addButton(stockGroup, tr("Take Out"));
		addButton(viewGroup, tr("List / Grid"));
		addButton(viewGroup, tr("3D Viewer"));
		addButton(manageGroup, tr("Edit Type Templates"));
		addButton(manageGroup, tr("Import from Mouser"));
		addButton(filesGroup, tr("Attach File"));
		addButton(filesGroup, tr("Open Datasheet"));
#endif
	}

}
