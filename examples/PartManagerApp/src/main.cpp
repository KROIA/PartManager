#include "ui/PartManager_DatabaseSelectorDialog.h"
#include "ui/PartManager_MainWindow.h"

#include <QApplication>
#include <QFont>
#include <QMessageBox>

namespace
{
	// Qt 5.15's Windows font database takes the default UI font from the DEFAULT_GUI_FONT stock
	// object, whose LOGFONT height is in *unscaled* 96-DPI pixels (-11), and then converts that
	// height to points using the *scaled* system DPI. On a 250% display that is
	// 11 * 72 / 240 = 3.3pt where it should be 8.25pt — so every layout scales correctly and
	// every label comes out unreadably small. Reproduces under both scale-factor rounding
	// policies, so it is the font path and not the scaling path.
	//
	// Qt multiplies logical point sizes by the device pixel ratio again when it paints, and under
	// PassThrough rounding that ratio *is* the system DPI over 96 — the very number the bad
	// division used. Multiplying back is therefore the exact inverse, not a fudge factor.
	//
	// Guarded so a Qt build or platform that gets this right is left alone: a plausible UI font
	// is never touched.
	void repairDefaultUiFont(QApplication& app)
	{
		// No desktop UI font is anywhere near this small; Windows' own is 8.25pt.
		constexpr qreal SuspiciouslySmallPointSize = 6.0;

		QFont font = app.font();
		const qreal ratio = app.devicePixelRatio();
		if (ratio <= 1.0 || font.pointSizeF() <= 0.0
			|| font.pointSizeF() >= SuspiciouslySmallPointSize)
		{
			return;
		}
		font.setPointSizeF(font.pointSizeF() * ratio);
		app.setFont(font);
	}
}

int main(int argc, char* argv[])
{
	// The icons live in core's icons.qrc, and the app links the library statically, so the
	// linker drops the generated resource-init object unless something references it.
	Q_INIT_RESOURCE(icons);

	QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	QApplication app(argc, argv);
	// Before any widget exists, so the database selector is readable too — every screen inherits
	// the application font.
	repairDefaultUiFont(app);

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
