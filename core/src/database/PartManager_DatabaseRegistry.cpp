#include "database/PartManager_DatabaseRegistry.h"
#include "settings/PartManager_Settings.h"

#include <algorithm>

namespace PartManager
{

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
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		bool alreadyPresent = std::any_of(entries.begin(), entries.end(),
			[&pmdbPath](const KnownDatabaseEntry& e) { return e.path == pmdbPath; });
		if (alreadyPresent)
		{
			return;
		}
		entries.push_back({ pmdbPath, std::string() });
		Settings::setKnownDatabases(entries);
	}

	void DatabaseRegistry::remove(const std::string& pmdbPath)
	{
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[&pmdbPath](const KnownDatabaseEntry& e) { return e.path == pmdbPath; }), entries.end());
		Settings::setKnownDatabases(entries);
	}

	void DatabaseRegistry::updateLastOpenedAt(const std::string& pmdbPath, const std::string& lastOpenedAtIso)
	{
		std::vector<KnownDatabaseEntry> entries = Settings::getKnownDatabases();
		for (KnownDatabaseEntry& e : entries)
		{
			if (e.path == pmdbPath)
			{
				e.lastOpenedAt = lastOpenedAtIso;
				break;
			}
		}
		Settings::setKnownDatabases(entries);
	}

}
