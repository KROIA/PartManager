#include "controllers/PartManager_MainWindowController.h"

#include <QDir>
#include <QFileInfo>

namespace PartManager
{

	MainWindowController::MainWindowController(std::unique_ptr<DatabaseHandle> handle)
		: m_handle(std::move(handle))
	{
	}

	QString MainWindowController::databaseName() const
	{
		return QFileInfo(pmdbPath()).absoluteDir().dirName();
	}

	QString MainWindowController::pmdbPath() const
	{
		return m_handle ? QString::fromStdString(m_handle->pmdbPath()) : QString();
	}

}
