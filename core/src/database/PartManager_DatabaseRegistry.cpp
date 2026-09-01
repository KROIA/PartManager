#include "database/PartManager_DatabaseRegistry.h"
#include "settings/PartManager_Settings.h"

#include <algorithm>
#include <filesystem>

namespace PartManager
{
	namespace
	{
		// Entries are matched by plain string compare, so every path entering the registry has
		// to be spelled the same way first: a file picker hands out forward slashes while a
		// command-line/native path uses backslashes, and without this the same database would
		// get registered twice. weakly_canonical() also collapses "." / ".." and is happy with
		// paths that don't exist (yet).
		std::string normalized(const std::string& pmdbPath)
		{
			std::error_code errorCode;
			std::filesystem::path canonical = std::filesystem::weakly_canonical(pmdbPath, errorCode);
			return errorCode ? pmdbPath : canonical.make_preferred().string();
		}
	}

	std::vector<RegisteredDatabase> DatabaseRegistry::list()
	{
		std::vector<RegisteredDatabase> result;
		for (const KnownDatabaseEntry& entry : Settings::getKnownDatabases())
		{
			result.push_back({ entry.path, entry.lastOpenedAt });
		}
		return result;
	}

	void DatabaseRegistry::add(const std::string& pmdbPath)
	{
		std::string path = normalized(pmdbPath);
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		bool alreadyPresent = std::any_of(entries.begin(), entries.end(),
			[&path](const KnownDatabaseEntry& e) { return e.path == path; });
		if (alreadyPresent)
		{
			return;
		}
		entries.push_back({ path, std::string() });
		Settings::setKnownDatabases(entries);
	}

	void DatabaseRegistry::remove(const std::string& pmdbPath)
	{
		std::string path = normalized(pmdbPath);
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&path](const KnownDatabaseEntry& e) { return e.path == path; }), entries.end());
		Settings::setKnownDatabases(entries);
	}

	void DatabaseRegistry::updateLastOpenedAt(const std::string& pmdbPath, const std::string& lastOpenedAtIso)
	{
		std::string path = normalized(pmdbPath);
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		for (KnownDatabaseEntry& e : entries)
		{
			if (e.path == path)
			{
				e.lastOpenedAt = lastOpenedAtIso;
				break;
			}
		}
		Settings::setKnownDatabases(entries);
	}

}
