#include "settings/PartManager_Settings.h"
#include "PartManager_debug.h"

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
	#include "ApplicationSettings.h"
	#include "SettingsGroup.h"
	#include "ListSetting.h"
	#include "Setting.h"
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

		// The §9/§9a user preferences. A second group rather than more entries in the first, so
		// the settings file stays readable and a database-list migration can never disturb them.
		class PreferencesSettingsGroup : public AppSettings::SettingsGroup
		{
		public:
			PreferencesSettingsGroup()
				: AppSettings::SettingsGroup("Preferences")
				, language("language", QStringLiteral("en"))
				, theme("theme", QString::fromLatin1(ThemeName::System))
				, currency("currency", QStringLiteral("CHF"))
				, kicadLibraryPath("kicadLibraryPath", QString())
				, backupsEnabled("backupsEnabled", true)
				, backupIntervalHours("backupIntervalHours", 6)
				, backupRetentionCount("backupRetentionCount", 20)
				, backupFolder("backupFolder", QString())
				, hideEmptyCategories("hideEmptyCategories", false)
				, llmProvider("llmProvider", QStringLiteral("ollama"))
				, llmOllamaUrl("llmOllamaUrl", QString())
				, llmOllamaModel("llmOllamaModel", QString())
				, llmClaudeEndpoint("llmClaudeEndpoint", QString())
				, llmClaudeModel("llmClaudeModel", QString())
				, llmSystemPrompt("llmSystemPrompt", QString())
				, llmShowToolCalls("llmShowToolCalls", true)
				, llmFontSizePercent("llmFontSizePercent", 100)
			{
				addSetting(language);
				addSetting(theme);
				addSetting(currency);
				addSetting(kicadLibraryPath);
				addSetting(backupsEnabled);
				addSetting(backupIntervalHours);
				addSetting(backupRetentionCount);
				addSetting(backupFolder);
				addSetting(hideEmptyCategories);
				addSetting(llmProvider);
				addSetting(llmOllamaUrl);
				addSetting(llmOllamaModel);
				addSetting(llmClaudeEndpoint);
				addSetting(llmClaudeModel);
				addSetting(llmSystemPrompt);
				addSetting(llmShowToolCalls);
				addSetting(llmFontSizePercent);
			}

			AppSettings::Setting language;
			AppSettings::Setting theme;
			AppSettings::Setting currency;
			AppSettings::Setting kicadLibraryPath;
			AppSettings::Setting backupsEnabled;
			AppSettings::Setting backupIntervalHours;
			AppSettings::Setting backupRetentionCount;
			AppSettings::Setting backupFolder;
			AppSettings::Setting hideEmptyCategories;
			AppSettings::Setting llmProvider;
			AppSettings::Setting llmOllamaUrl;
			AppSettings::Setting llmOllamaModel;
			AppSettings::Setting llmClaudeEndpoint;
			AppSettings::Setting llmClaudeModel;
			AppSettings::Setting llmSystemPrompt;
			AppSettings::Setting llmShowToolCalls;
			AppSettings::Setting llmFontSizePercent;
		};

		// Keys inside one remembered-mapping QVariantMap.
		const QString SIGNATURE_KEY = QStringLiteral("headers");
		const QString DESIGNATORS_KEY = QStringLiteral("designators");
		const QString MPN_KEY = QStringLiteral("mpn");
		const QString MPN_ALT_KEY = QStringLiteral("mpnAlt");
		const QString QUANTITY_KEY = QStringLiteral("quantity");
		const QString NAME_KEY = QStringLiteral("name");

		// §5's remembered column mappings. Its own group for the same reason the preferences
		// have one: a list that grows with use has no business sharing a group with settings a
		// dialog rewrites wholesale.
		class ImportSettingsGroup : public AppSettings::SettingsGroup
		{
		public:
			ImportSettingsGroup()
				: AppSettings::SettingsGroup("Import")
				, columnMappings("columnMappings")
			{
				addSetting(columnMappings);
			}

			AppSettings::ListSetting columnMappings;
		};

		// ApplicationSettings subclass owning the group above — protected addGroup() access
		// works the same way, through ordinary inheritance.
		class PartManagerAppSettings : public AppSettings::ApplicationSettings
		{
		public:
			PartManagerAppSettings()
				: AppSettings::ApplicationSettings(settingsDirectory(), "PartManager")
			{
				addGroup(m_databaseGroup);
				addGroup(m_preferencesGroup);
				addGroup(m_importGroup);
			}

			DatabaseSettingsGroup m_databaseGroup;
			PreferencesSettingsGroup m_preferencesGroup;
			ImportSettingsGroup m_importGroup;
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

	namespace
	{
		// Clamped on read as well as on write: the settings file is plain text in the user's data
		// folder, so a hand-edited 0 must not become a zero-hour backup loop or a retention that
		// deletes every snapshot the moment it is taken.
		int clamped(int value, int low, int high)
		{
			return value < low ? low : (value > high ? high : value);
		}
	}

	AppPreferences Settings::getPreferences()
	{
		AppPreferences preferences;

#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		instance().load();
		PreferencesSettingsGroup& group = instance().m_preferencesGroup;
		preferences.language = group.language.getValue().toString().toStdString();
		preferences.theme = group.theme.getValue().toString().toStdString();
		preferences.currency = group.currency.getValue().toString().toStdString();
		preferences.kicadLibraryPath = group.kicadLibraryPath.getValue().toString().toStdString();
		preferences.backupsEnabled = group.backupsEnabled.getValue().toBool();
		preferences.hideEmptyCategories = group.hideEmptyCategories.getValue().toBool();
		preferences.backupIntervalHours = clamped(group.backupIntervalHours.getValue().toInt(),
			MinBackupIntervalHours, MaxBackupIntervalHours);
		preferences.backupRetentionCount = clamped(group.backupRetentionCount.getValue().toInt(),
			MinBackupRetentionCount, MaxBackupRetentionCount);
		preferences.backupFolder = group.backupFolder.getValue().toString().toStdString();

		// §14b. The four url/model strings are deliberately *not* defaulted here: empty means
		// "use whatever the default is now", which is what lets a changed environment variable be
		// followed rather than overridden by a stale copy of itself.
		preferences.llmProvider = group.llmProvider.getValue().toString().toStdString();
		preferences.llmOllamaUrl = group.llmOllamaUrl.getValue().toString().toStdString();
		preferences.llmOllamaModel = group.llmOllamaModel.getValue().toString().toStdString();
		preferences.llmClaudeEndpoint = group.llmClaudeEndpoint.getValue().toString().toStdString();
		preferences.llmClaudeModel = group.llmClaudeModel.getValue().toString().toStdString();
		preferences.llmSystemPrompt = group.llmSystemPrompt.getValue().toString().toStdString();
		preferences.llmShowToolCalls = group.llmShowToolCalls.getValue().toBool();
		preferences.llmFontSizePercent = clamped(group.llmFontSizePercent.getValue().toInt(),
			MinLlmFontSizePercent, MaxLlmFontSizePercent);

		// An empty string is what a never-written setting reads back as on some AppSettings
		// versions; the struct's own defaults are the right answer then, not "no language".
		if (preferences.language.empty()) { preferences.language = "en"; }
		if (preferences.theme.empty())    { preferences.theme = ThemeName::System; }
		if (preferences.currency.empty()) { preferences.currency = "CHF"; }
		// The provider is the one LLM string with a vocabulary rather than a default, so it gets
		// the same guard: an empty value is "never chosen", which is Ollama (§14b).
		if (preferences.llmProvider.empty()) { preferences.llmProvider = "ollama"; }
#else
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, preferences are not persisted\n");
#endif
		return preferences;
	}

	void Settings::setPreferences(const AppPreferences& preferences)
	{
#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		PreferencesSettingsGroup& group = instance().m_preferencesGroup;
		group.language.setValue(QString::fromStdString(preferences.language));
		group.theme.setValue(QString::fromStdString(preferences.theme));
		group.currency.setValue(QString::fromStdString(preferences.currency));
		group.kicadLibraryPath.setValue(QString::fromStdString(preferences.kicadLibraryPath));
		group.backupsEnabled.setValue(preferences.backupsEnabled);
		group.hideEmptyCategories.setValue(preferences.hideEmptyCategories);
		group.backupIntervalHours.setValue(clamped(preferences.backupIntervalHours,
			MinBackupIntervalHours, MaxBackupIntervalHours));
		group.backupRetentionCount.setValue(clamped(preferences.backupRetentionCount,
			MinBackupRetentionCount, MaxBackupRetentionCount));
		group.backupFolder.setValue(QString::fromStdString(preferences.backupFolder));
		group.llmProvider.setValue(QString::fromStdString(preferences.llmProvider));
		group.llmOllamaUrl.setValue(QString::fromStdString(preferences.llmOllamaUrl));
		group.llmOllamaModel.setValue(QString::fromStdString(preferences.llmOllamaModel));
		group.llmClaudeEndpoint.setValue(QString::fromStdString(preferences.llmClaudeEndpoint));
		group.llmClaudeModel.setValue(QString::fromStdString(preferences.llmClaudeModel));
		group.llmSystemPrompt.setValue(QString::fromStdString(preferences.llmSystemPrompt));
		group.llmShowToolCalls.setValue(preferences.llmShowToolCalls);
		group.llmFontSizePercent.setValue(clamped(preferences.llmFontSizePercent,
			MinLlmFontSizePercent, MaxLlmFontSizePercent));
		instance().save();
#else
		PM_UNUSED(preferences);
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, preferences are not persisted\n");
#endif
	}

	bool Settings::getImportMapping(const std::string& headerSignature,
		ImportMappingMemory& outMapping)
	{
		if (headerSignature.empty())
		{
			return false;
		}
#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		instance().load();
		const QString wanted = QString::fromStdString(headerSignature);
		for (const QVariant& entry : instance().m_importGroup.columnMappings.getData())
		{
			const QVariantMap map = entry.toMap();
			if (map.value(SIGNATURE_KEY).toString() != wanted)
			{
				continue;
			}
			outMapping = ImportMappingMemory();
			outMapping.headerSignature = headerSignature;
			outMapping.designators = map.value(DESIGNATORS_KEY, -1).toInt();
			outMapping.mpn = map.value(MPN_KEY, -1).toInt();
			outMapping.mpnAlt = map.value(MPN_ALT_KEY, -1).toInt();
			outMapping.quantity = map.value(QUANTITY_KEY, -1).toInt();
			outMapping.name = map.value(NAME_KEY, -1).toInt();
			return true;
		}
#else
		PM_UNUSED(outMapping);
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, import mappings are not persisted\n");
#endif
		return false;
	}

	void Settings::rememberImportMapping(const ImportMappingMemory& mapping)
	{
		if (mapping.headerSignature.empty())
		{
			return;
		}
#if APP_SETTINGS_LIBRARY_AVAILABLE == 1
		instance().load();
		const QString signature = QString::fromStdString(mapping.headerSignature);

		QVariantMap entry;
		entry[SIGNATURE_KEY] = signature;
		entry[DESIGNATORS_KEY] = mapping.designators;
		entry[MPN_KEY] = mapping.mpn;
		entry[MPN_ALT_KEY] = mapping.mpnAlt;
		entry[QUANTITY_KEY] = mapping.quantity;
		entry[NAME_KEY] = mapping.name;

		// Most recently used first, so the trim below drops the mapping nobody has needed in
		// twenty imports rather than the one from this morning.
		std::vector<QVariant> list;
		list.push_back(entry);
		for (const QVariant& existing : instance().m_importGroup.columnMappings.getData())
		{
			if (existing.toMap().value(SIGNATURE_KEY).toString() == signature)
			{
				continue;   // replaced by the entry just pushed
			}
			if (static_cast<int>(list.size()) >= MaxRememberedImportMappings)
			{
				break;
			}
			list.push_back(existing);
		}
		instance().m_importGroup.columnMappings.setData(list);
		instance().save();
#else
		PM_CONSOLE("PartManager::Settings: AppSettings library not available, import mappings are not persisted\n");
#endif
	}

}
