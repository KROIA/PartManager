#include <iostream>
#include <QApplication>
#include <QtGlobal>
#include "UnitTest_Gui.h"
#include "PartManager.h"
#include "PartManager_AppStartup.h"
#include "tests.h"


int main(int argc, char* argv[])
{
	// Qt writes its warnings to stderr, which interleaves unreadably with the buffered test
	// output — a warning ends up nowhere near the test that caused it. Same stream, same order.
	qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message)
		{
			std::cout << "[Qt] " << message.toStdString() << std::endl;
		});


	// The same display setup the app performs. Without it the GUI tests run unscaled with Qt
	// 5.15's broken 3.3pt default font, which is a screen the user never sees — so a layout that
	// only breaks at the real scale could never fail a test. Attributes first: Qt reads them when
	// the QApplication is constructed and ignores them afterwards.
	PartManager::applyHighDpiAttributes();

	// Opening a database starts SQLiteWrapper's file-change watcher, a QThread whose run() calls
	// exec() — and a QEventLoop warns on every construction while no QCoreApplication exists.
	// The GUI tests create one anyway, so create it once up front and the whole run is quiet.
	UnitTest::Gui::ensureApplication();
	if (QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance()))
	{
		PartManager::repairDefaultUiFont(*app);
	}

	PartManager::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	// getSuccess() is true when everything passed, which as an exit code means failure —
	// inverted since the template, and silently wrong for any CI gate or script that checks it.
	return results.getSuccess() ? 0 : 1;
}