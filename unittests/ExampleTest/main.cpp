#include <iostream>
#include <QtGlobal>
#include "UnitTest_Gui.h"
#include "PartManager.h"
#include "tests.h"


int main(int argc, char* argv[])
{
	// Qt writes its warnings to stderr, which interleaves unreadably with the buffered test
	// output — a warning ends up nowhere near the test that caused it. Same stream, same order.
	qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message)
		{
			std::cout << "[Qt] " << message.toStdString() << std::endl;
		});

	// Opening a database starts SQLiteWrapper's file-change watcher, a QThread whose run() calls
	// exec() — and a QEventLoop warns on every construction while no QCoreApplication exists.
	// The GUI tests create one anyway, so create it once up front and the whole run is quiet.
	UnitTest::Gui::ensureApplication();

	PartManager::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	return results.getSuccess();
}