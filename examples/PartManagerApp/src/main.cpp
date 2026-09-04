#include "PartManager_AppStartup.h"
#include "settings/PartManager_Settings.h"
#include "ui/PartManager_DatabaseSelectorDialog.h"
#include "ui/PartManager_MainWindow.h"

#include <QApplication>
#include <QIcon>
#include <QMessageBox>

int main(int argc, char* argv[])
{
	// Shared with the GUI test binary, so the tests render exactly what the user sees.
	PartManager::applyHighDpiAttributes();


	// The icons live in core's icons.qrc, and the app links the library statically, so the
	// linker drops the generated resource-init object unless something references it.
	Q_INIT_RESOURCE(icons);


	QApplication app(argc, argv);
	// Before any widget exists, so the database selector is readable too — every screen inherits
	// the application font.
	PartManager::repairDefaultUiFont(app);
	// Before the selector too — it has combo boxes of its own.
	PartManager::installScrollGuard(app);
	PartManager::installToolTipWrapper(app);

	// AppSettings (and therefore the known-databases registry, §1b) keys off these.
	QCoreApplication::setOrganizationName("KROIA");
	QCoreApplication::setApplicationName("PartManager");
	// The taskbar, the Alt-Tab card and every window's title bar. AppIcon.ico is the same drawing
	// and covers the .exe in Explorer, but Windows takes that one from the binary's resources and
	// never from a running process — so both are needed, and both are generated from app.svg.
	app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

	// §8/§9. After the organization/application names, because Settings reads its file from a
	// path those key off — asking earlier would read a different (empty) settings file.
	const PartManager::AppPreferences preferences = PartManager::Settings::getPreferences();
	PartManager::applyLanguage(app, preferences.language);
	PartManager::applyTheme(app, preferences.theme);

	// §1a: a .pmdb passed on the command line (what a file association hands us when the
	// user double-clicks one) opens straight through. That is an explicit pick, not the
	// auto-resume-last-used that §1b rules out, so the selector is still the plain-launch path.
	std::unique_ptr<PartManager::DatabaseHandle> handle;
	QStringList arguments = QCoreApplication::arguments();
	if (arguments.size() > 1)
	{
		PartManager::DatabaseSelectorController controller;
		QString error;
		handle = controller.openDatabase(arguments.at(1), error);
		if (!handle)
		{
			QMessageBox::warning(nullptr, QObject::tr("Could not open database"), error);
			return 1;
		}
	}
	else
	{
		// §1b: startup always shows the selector — no auto-resume-last-used.
		PartManager::DatabaseSelectorDialog selector;
		if (selector.exec() != QDialog::Accepted)
		{
			return 0;
		}
		handle = selector.takeHandle();
	}

	// §1b: Switch Database closes the window and hands back the database it opened, and the next
	// turn of this loop builds a fresh window on it. An in-process restart rather than swapping
	// the handle underneath the running window: every dialog, the mesh cache builder and the
	// partlist panel hold a raw DatabaseHandle* taken from it, and none of them would notice.
	while (handle)
	{
		PartManager::MainWindow window(std::move(handle));
		window.show();
		const int code = app.exec();
		handle = window.takeSwitchTarget();
		if (!handle)
		{
			return code;
		}
	}
	return 0;
}
