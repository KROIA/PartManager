#include "settings/PartManager_Settings.h"
#include "PartManager_debug.h"

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
	#include "ApplicationSettings.h"
	#include "SettingsGroup.h"
	#include "ListSetting.h"
	#include <QVariant>
	#include <QVariantMap>
	#include <QString>
	#include <QDir>
	#include <QStandardPaths>
#endif

namespace PartManager
{

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
	namespace
	{
		// Keys used inside each known-database list entry's QVariantMap.
		const QString PATH_KEY = QStringLiteral("path");
		const QString LAST_OPENED_KEY = QStringLiteral("lastOpenedAt");

		// SettingsGroup subclass owning the one ListSetting this facade needs — AppSettings only
		// grants protected addSetting() access to a SettingsGroup's own subclasses.
		class DatabaseSettingsGroup : public AppSettings::SettingsGroup
		{
		public:
			DatabaseSettingsGroup()
				: AppSettings::SettingsGroup("Database")
				, knownDatabases("knownDatabases")
			{
				addSetting(knownDatabases);
			}

			AppSettings::ListSetting knownDatabases;
		};

		// Per-user application-data folder holding the settings file. Without this the settings
		// file would land in the process' working directory, so the known-databases list would
		// silently be per-launch-directory. GenericDataLocation is the same root Qt's
		// AppDataLocation is built on, but doesn't vary with QCoreApplication's
		// organization/application name — which the app sets and a unit test doesn't, and both
		// have to reach the same file.
		QString settingsDirectory()
		{
			QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
				+ QStringLiteral("/PartManager");
			QDir().mkpath(directory);
			return directory;
		}

		// ApplicationSettings subclass owning the group above — protected addGroup() access
		// works the same way, through ordinary inheritance.
		class PartManagerAppSettings : public AppSettings::ApplicationSettings
		{
		public:
			PartManagerAppSettings()
				: AppSettings::ApplicationSettings(settingsDirectory(), "PartManager")
			{
				addGroup(m_databaseGroup);
			}

			DatabaseSettingsGroup m_databaseGroup;
		};

		// Single process-wide instance backing this facade.
		PartManagerAppSettings& instance()
		{
			static PartManagerAppSettings settings;
			return settings;
		}
	}
#endif

	std::string Settings::getSettingsFilePath()
	{
#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		return instance().getFilePath().toStdString();
#else
		return std::string();
#endif
	}

	std::vector<KnownDatabaseEntry> Settings::getKnownDatabases()
	{
		std::vector<KnownDatabaseEntry> result;

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		instance().load();
		const std::vector<QVariant>& list = instance().m_databaseGroup.knownDatabases.getData();
		result.reserve(list.size());
		for (const QVariant& entry : list)
		{
			QVariantMap map = entry.toMap();
			KnownDatabaseEntry known;
			known.path = map.value(PATH_KEY).toString().toStdString();
			known.lastOpenedAt = map.value(LAST_OPENED_KEY).toString().toStdString();
			result.push_back(known);
		}
#else
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, known-database list is not persisted\n");
#endif
		return result;
	}

	void Settings::setKnownDatabases(const std::vector<KnownDatabaseEntry>& entries)
	{

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		std::vector<QVariant> list;
		list.reserve(entries.size());
		for (const KnownDatabaseEntry& known : entries)
		{
			QVariantMap map;
			map[PATH_KEY] = QString::fromStdString(known.path);
			map[LAST_OPENED_KEY] = QString::fromStdString(known.lastOpenedAt);
			list.push_back(map);
		}
		instance().m_databaseGroup.knownDatabases.setData(list);
		instance().save();
#else
		PM_UNUSED(entries);
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, known-database list is not persisted\n");
#endif
	}

}
