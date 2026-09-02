#pragma once

#include "UnitTest.h"
#include "settings/PartManager_Settings.h"
#include "PartManager_AppStartup.h"
#include <QApplication>
#include <QCoreApplication>
#include <filesystem>

class TST_Settings : public UnitTest::Test
{
	TEST_CLASS(TST_Settings)
public:
	TST_Settings()
		: Test("TST_Settings")
	{
		ADD_TEST(TST_Settings::settingsFileIsOutsideWorkingDirectory);
		ADD_TEST(TST_Settings::knownDatabasesRoundTrip);
		ADD_TEST(TST_Settings::preferencesRoundTripAndClamp);
		ADD_TEST(TST_Settings::germanTranslationIsActuallyInstalled);
	}

private:

	// Tests
	TEST_FUNCTION(settingsFileIsOutsideWorkingDirectory)
	{
		TEST_START;

		std::filesystem::path path(PartManager::Settings::getSettingsFilePath());
		TEST_ASSERT_M(!path.empty(), "settings file path is empty");
		TEST_ASSERT_M(path.is_absolute(), "settings file path is not absolute: " + path.string());

		// The whole point of the fix: the file must not be resolved relative to wherever the
		// process happens to have been started from.
		std::filesystem::path workingDirectory = std::filesystem::current_path();
		TEST_ASSERT_M(path.parent_path() != workingDirectory,
			"settings file lives in the working directory: " + path.string());
	}

	TEST_FUNCTION(knownDatabasesRoundTrip)
	{
		TEST_START;

		// This writes the real user-level settings file, so put back whatever was there.
		std::vector<PartManager::KnownDatabaseEntry> original = PartManager::Settings::getKnownDatabases();

		std::vector<PartManager::KnownDatabaseEntry> written;
		written.push_back({ "C:\\somewhere\\alpha.pmdb", "2026-08-31T09:00:00" });
		written.push_back({ "C:\\somewhere\\beta.pmdb", std::string() });
		PartManager::Settings::setKnownDatabases(written);

		std::vector<PartManager::KnownDatabaseEntry> readBack = PartManager::Settings::getKnownDatabases();
		TEST_COMPARE(readBack.size(), written.size());
		for (size_t i = 0; i < readBack.size() && i < written.size(); ++i)
		{
			TEST_COMPARE(readBack[i].path, written[i].path);
			TEST_COMPARE(readBack[i].lastOpenedAt, written[i].lastOpenedAt);
		}

		PartManager::Settings::setKnownDatabases(original);
	}

	// §9 preferences. The clamps matter because the settings file is plain text in the user's
	// data folder: a hand-edited 0 must not become a zero-hour backup loop, or a retention that
	// deletes every snapshot the moment it is taken.
	TEST_FUNCTION(preferencesRoundTripAndClamp)
	{
		TEST_START;

		const PartManager::AppPreferences original = PartManager::Settings::getPreferences();

		PartManager::AppPreferences written;
		written.language = "de";
		written.theme = PartManager::ThemeName::Dark;
		written.currency = "EUR";
		written.backupsEnabled = false;
		written.backupIntervalHours = 0;        // below the floor
		written.backupRetentionCount = 99999;   // above the ceiling
		PartManager::Settings::setPreferences(written);

		const PartManager::AppPreferences read = PartManager::Settings::getPreferences();
		TEST_COMPARE(read.language, std::string("de"));
		TEST_COMPARE(read.theme, std::string(PartManager::ThemeName::Dark));
		TEST_COMPARE(read.currency, std::string("EUR"));
		TEST_ASSERT(!read.backupsEnabled);
		TEST_COMPARE(read.backupIntervalHours, PartManager::Settings::MinBackupIntervalHours);
		TEST_COMPARE(read.backupRetentionCount, PartManager::Settings::MaxBackupRetentionCount);

		// This suite writes to the user's real settings file, so it puts it back.
		PartManager::Settings::setPreferences(original);
	}

	// The German .qm is compiled by the app's CMake step, which is easy to break without anyone
	// noticing until the language combo silently does nothing — which is exactly what happened
	// before it existed. This fails loudly instead.
	TEST_FUNCTION(germanTranslationIsActuallyInstalled)
	{
		TEST_START;

		QApplication* app = qobject_cast<QApplication*>(QCoreApplication::instance());
		if (app == nullptr)
		{
			TEST_MESSAGE("no QApplication - skipping the translation check");
			return;
		}

		// Everything is measured first and asserted afterwards, with the translator removed in
		// between. A failing TEST_COMPARE aborts the function, so asserting while German is still
		// installed leaks it into every suite that runs later — which is exactly what happened
		// the first time this test was written: three unrelated suites started failing because
		// their labels had turned German.
		const bool germanLoaded = PartManager::applyLanguage(*app, "de");
		// The context is the class name lupdate recorded, not the fully qualified one.
		const QString appearance = QCoreApplication::translate("SettingsDialog", "Appearance");
		const QString sourceLabel = QCoreApplication::translate("QObject", "Manual");
		const bool englishRestored = PartManager::applyLanguage(*app, "en");
		const QString appearanceAfter = QCoreApplication::translate("SettingsDialog", "Appearance");

		TEST_ASSERT_M(germanLoaded,
			"translations/PartManager_de.qm was not found next to the test binary - the "
			"lrelease step in examples/PartManagerApp/CMakeLists.txt did not run");
		// Loading is not enough: an empty .qm loads fine and translates nothing.
		TEST_COMPARE(appearance, QStringLiteral("Darstellung"));
		// A second context, so a .qm covering only one file cannot pass this.
		TEST_COMPARE(sourceLabel, QStringLiteral("Manuell"));

		// English is the source language, so there is no .qm for it and installing nothing is
		// success, not failure.
		TEST_ASSERT(englishRestored);
		TEST_COMPARE(appearanceAfter, QStringLiteral("Appearance"));
	}

};

TEST_INSTANTIATE(TST_Settings);
