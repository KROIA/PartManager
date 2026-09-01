#include "ui/PartManager_DatabaseSelectorDialog.h"
#include "ui/PartManager_MainWindow.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char* argv[])
{
	// The icons live in core's icons.qrc, and the app links the library statically, so the
	// linker drops the generated resource-init object unless something references it.
	Q_INIT_RESOURCE(icons);

	QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	QApplication app(argc, argv);
	// AppSettings (and therefore the known-databases registry, §1b) keys off these.
	QCoreApplication::setOrganizationName("KROIA");
	QCoreApplication::setApplicationName("PartManager");

	// TODO(§8): install the QTranslator for the saved language here.
	// TODO(§9): apply the saved theme/palette here.

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

	PartManager::MainWindow window(std::move(handle));
	window.show();
	return app.exec();
}
