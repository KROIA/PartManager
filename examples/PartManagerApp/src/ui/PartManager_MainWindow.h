// @file PartManager_MainWindow.h
// @brief The app's main window — ribbon host plus a still-empty body (§7).
//
// Layout lives in PartManager_MainWindow.ui; the ribbon itself is built in code
// because RibbonWidget's tabs/groups/buttons are not Designer-expressible. The
// body is a placeholder on purpose: the category tree, part table and preview
// from main-window.svg are a later slice, as are the ribbon buttons' actions.
// @see docs/design/ARCHITECTURE.md §7
// @see PartManager_MainWindowController.h
#pragma once

#include "controllers/PartManager_MainWindowController.h"
#include <QMainWindow>
#include <memory>

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

	private:
		// Builds the Home/Parts tabs of §7 into the .ui file's ribbonToolBar.
		void buildRibbon();

		Ui::MainWindow* m_ui;
		MainWindowController m_controller;
		RibbonWidget::Ribbon* m_ribbon = nullptr;
	};

}
