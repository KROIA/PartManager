#include "controllers/PartManager_DatabaseSelectorController.h"
#include "database/PartManager_DatabaseRegistry.h"
#include "database/PartManager_DatabaseMetadata.h"
#include "database/PartManager_SchemaMigrator.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>

namespace PartManager
{

	QString DatabaseSelectorController::displayNameOf(const QString& pmdbPath)
	{
		return QFileInfo(pmdbPath).absoluteDir().dirName();
	}

	QString DatabaseSelectorController::readmePathOf(const QString& pmdbPath)
	{
		return QFileInfo(pmdbPath).absoluteDir().filePath(QStringLiteral("README.md"));
	}

	SchemaBadge DatabaseSelectorController::badgeOf(const QString& pmdbPath, int& outSchemaVersion)
	{
		outSchemaVersion = 0;
		if (!QFileInfo::exists(pmdbPath))
		{
			return SchemaBadge::stale;
		}
		// §1a: the .pmdb is a cache of db_meta precisely so this list can badge every
		// entry without opening a single SQLite file.
		DatabaseMetadataValues values;
		if (!DatabaseMetadata::readPmdbFile(pmdbPath.toStdString(), values))
		{
			return SchemaBadge::unreadable;
		}
		outSchemaVersion = values.schemaVersion;
		if (values.schemaVersion == CurrentSchemaVersion)
		{
			return SchemaBadge::current;
		}
		return values.schemaVersion < CurrentSchemaVersion ? SchemaBadge::needsUpdate : SchemaBadge::tooNew;
	}

	QString DatabaseSelectorController::descriptionOf(const QString& pmdbPath)
	{
		QFile readme(readmePathOf(pmdbPath));
		if (!readme.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			return QString();
		}
		QTextStream stream(&readme);
		stream.setCodec("UTF-8");
		QString text = stream.readAll();

		// The file starts with a "# <name>" title written at creation time; the description is
		// everything after it, so a README hand-edited outside the app still reads back sensibly.
		int firstBreak = text.indexOf(QLatin1Char('\n'));
		if (text.startsWith(QLatin1Char('#')) && firstBreak >= 0)
		{
			text = text.mid(firstBreak + 1);
		}
		return text.trimmed();
	}

	bool DatabaseSelectorController::setDescription(const QString& pmdbPath, const QString& description)
	{
		QFile readme(readmePathOf(pmdbPath));
		if (!readme.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		{
			return false;
		}
		QTextStream stream(&readme);
		stream.setCodec("UTF-8");
		stream << "# " << displayNameOf(pmdbPath) << "\n\n" << description.trimmed() << "\n";
		return true;
	}

	std::vector<DatabaseListEntry> DatabaseSelectorController::knownDatabases() const
	{
		std::vector<DatabaseListEntry> entries;
		for (const RegisteredDatabase& registered : DatabaseRegistry::list())
		{
			QString path = QString::fromStdString(registered.pmdbPath);
			DatabaseListEntry entry;
			entry.name = displayNameOf(path);
			entry.pmdbPath = path;
			entry.lastOpenedAt = QString::fromStdString(registered.lastOpenedAt);
			entry.description = descriptionOf(path);
			entry.badge = badgeOf(path, entry.schemaVersion);
			entries.push_back(entry);
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
