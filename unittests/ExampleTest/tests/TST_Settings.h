#pragma once

#include "UnitTest.h"
#include "settings/PartManager_Settings.h"
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

};

TEST_INSTANTIATE(TST_Settings);
