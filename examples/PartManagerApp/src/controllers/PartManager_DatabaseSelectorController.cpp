#include "controllers/PartManager_DatabaseSelectorController.h"
#include "database/PartManager_DatabaseRegistry.h"

#include <QDateTime>
#include <QFileInfo>
#include <QDir>

namespace PartManager
{

	QString DatabaseSelectorController::displayNameOf(const QString& pmdbPath)
	{
		return QFileInfo(pmdbPath).absoluteDir().dirName();
	}

	std::vector<DatabaseListEntry> DatabaseSelectorController::knownDatabases() const
	{
		std::vector<DatabaseListEntry> entries;
		for (const RegisteredDatabase& registered : DatabaseRegistry::list())
		{
			QString path = QString::fromStdString(registered.pmdbPath);
			entries.push_back({ displayNameOf(path), path,
				QString::fromStdString(registered.lastOpenedAt) });
		}
		return entries;
	}

	std::unique_ptr<DatabaseHandle> DatabaseSelectorController::createDatabase(const QString& parentFolder,
		const QString& name, QString& outErrorMessage)
	{
		std::string error;
		std::unique_ptr<DatabaseHandle> handle =
			DatabaseHandle::createNew(parentFolder.toStdString(), name.toStdString(), error);
		if (!handle)
		{
			outErrorMessage = QString::fromStdString(error);
			return nullptr;
		}
		DatabaseRegistry::add(handle->pmdbPath());
		DatabaseRegistry::updateLastOpenedAt(handle->pmdbPath(),
			QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString());
		return handle;
	}

	void DatabaseSelectorController::registerDatabase(const QString& pmdbPath)
	{
		DatabaseRegistry::add(pmdbPath.toStdString());
	}

	void DatabaseSelectorController::removeFromList(const QString& pmdbPath)
	{
		DatabaseRegistry::remove(pmdbPath.toStdString());
	}

	std::unique_ptr<DatabaseHandle> DatabaseSelectorController::openDatabase(const QString& pmdbPath,
		QString& outErrorMessage)
	{
		auto handle = std::make_unique<DatabaseHandle>(pmdbPath.toStdString());
		if (!handle->open())
		{
			outErrorMessage = QString::fromStdString(handle->errorMessage());
			return nullptr;
		}
		DatabaseRegistry::add(handle->pmdbPath()); // no-op if already known
		DatabaseRegistry::updateLastOpenedAt(handle->pmdbPath(),
			QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString());
		return handle;
	}

}
