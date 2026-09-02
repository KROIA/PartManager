#pragma once

#include "UnitTest.h"
#include "model3d/PartManager_StepConverter.h"
#include <filesystem>
#include <fstream>

// §13's STEP -> mesh conversion, minus the subprocess. Everything here is path and script
// handling, which is where the quiet mistakes live: a cache key that collides across two models
// both called `model.step`, or a generated script that will not parse because a path had a
// backslash in it.
class TST_StepConverter : public UnitTest::Test
{
	TEST_CLASS(TST_StepConverter)
public:
	TST_StepConverter()
		: Test("TST_StepConverter")
	{
		ADD_TEST(TST_StepConverter::cacheKeysDoNotCollideAcrossFolders);
		ADD_TEST(TST_StepConverter::scriptSurvivesWindowsPaths);
		ADD_TEST(TST_StepConverter::staleCacheIsNotTreatedAsValid);
		ADD_TEST(TST_StepConverter::aMissingConverterIsReportedNotGuessed);
	}

private:

	static std::filesystem::path scratch(const std::string& name)
	{
		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / ("PartManager_TST_StepConverter_" + name);
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);
		return folder;
	}

	static void touch(const std::filesystem::path& path, const std::string& contents)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		out << contents;
	}

	// Tests

	TEST_FUNCTION(cacheKeysDoNotCollideAcrossFolders)
	{
		TEST_START;

		// Every KiCad library names its models after the footprint, so two parts really can both
		// have a `model.step` — keyed on the filename alone, the second would silently display
		// the first one's mesh.
		const std::string a = PartManager::StepConverter::cachedMeshPath("C:/cache",
			"C:/db/filestore/aa/model.step");
		const std::string b = PartManager::StepConverter::cachedMeshPath("C:/cache",
			"C:/db/filestore/bb/model.step");
		TEST_ASSERT_M(a != b, "two models with the same filename must not share a cache entry");

		// Same source always resolves to the same entry, or nothing would ever hit the cache.
		TEST_COMPARE(PartManager::StepConverter::cachedMeshPath("C:/cache",
			"C:/db/filestore/aa/model.step"), a);

		// The name stays recognisable to a human looking in the folder, and it is an STL.
		TEST_ASSERT(a.find("model-") != std::string::npos);
		TEST_ASSERT(a.rfind(".stl") == a.size() - 4);

		// No cache root means no cache path, rather than one rooted at the filesystem root.
		TEST_ASSERT(PartManager::StepConverter::cachedMeshPath("", "C:/a/model.step").empty());
		TEST_ASSERT(PartManager::StepConverter::cachedMeshPath("C:/cache", "").empty());
	}

	TEST_FUNCTION(scriptSurvivesWindowsPaths)
	{
		TEST_START;

		// A Windows path inside a Python string literal is the trap: "C:\db\new\model.step"
		// contains \n and \d, so it either fails to parse or points somewhere else entirely.
		const std::string script = PartManager::StepConverter::conversionScript(
			"C:\\db\\new\\model.step", "C:\\cache\\model-0123.stl");

		TEST_ASSERT_M(script.find("\\") == std::string::npos,
			"no backslash may survive into the generated script:\n" + script);
		TEST_ASSERT(script.find("C:/db/new/model.step") != std::string::npos);
		TEST_ASSERT(script.find("C:/cache/model-0123.stl") != std::string::npos);

		// The pieces that actually do the work.
		TEST_ASSERT(script.find("import MeshPart") != std::string::npos);
		TEST_ASSERT(script.find("shape.read(source)") != std::string::npos);
		TEST_ASSERT(script.find("mesh.write(target)") != std::string::npos);
		// Relative=False keeps the deflection in millimetres rather than a fraction of the
		// bounding box, so a small footprint is not tessellated absurdly finely.
		TEST_ASSERT(script.find("Relative=False") != std::string::npos);

		// A quote in a path would close the literal early; it cannot occur in a Windows filename
		// and is dropped rather than allowed to produce a script that will not parse.
		const std::string quoted = PartManager::StepConverter::conversionScript(
			"/tmp/we\"ird.step", "/tmp/out.stl");
		TEST_ASSERT(quoted.find("we\"ird") == std::string::npos);
		TEST_ASSERT(quoted.find("/tmp/weird.step") != std::string::npos);
	}

	TEST_FUNCTION(staleCacheIsNotTreatedAsValid)
	{
		TEST_START;

		const std::filesystem::path folder = scratch("cache");
		const std::filesystem::path source = folder / "model.step";
		const std::filesystem::path cacheRoot = folder / "meshcache";
		touch(source, "ISO-10303-21;");

		// Nothing converted yet.
		TEST_ASSERT(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()));

		const std::filesystem::path cached = PartManager::StepConverter::cachedMeshPath(
			cacheRoot.string(), source.string());
		touch(cached, "solid\nendsolid\n");
		TEST_ASSERT_M(PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()),
			"a mesh newer than its source is a usable cache entry");

		// A source replaced after conversion must re-convert rather than show the old shape.
		std::filesystem::last_write_time(source,
			std::filesystem::last_write_time(cached) + std::chrono::hours(1));
		TEST_ASSERT_M(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()),
			"a source newer than its mesh must invalidate the cache");

		// A source that is gone is not a valid cache either, however fresh the mesh looks.
		std::filesystem::remove(source);
		TEST_ASSERT(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()));
	}

	TEST_FUNCTION(aMissingConverterIsReportedNotGuessed)
	{
		TEST_START;

		// Whatever this machine has, the two must agree — isAvailable() deciding one way while
		// converterPath() returns the other is how the viewer ends up starting nothing and
		// waiting forever.
		const std::string path = PartManager::StepConverter::converterPath();
		TEST_COMPARE(PartManager::StepConverter::isAvailable(), !path.empty());
		if (path.empty())
		{
			TEST_MESSAGE("No STEP converter on this machine - install FreeCAD, or set "
				"PARTMANAGER_STEP_CONVERTER, to exercise the conversion path");
		}
		else
		{
			TEST_ASSERT_M(std::filesystem::is_regular_file(path),
				"converterPath() must only ever name a file that exists: " + path);
		}

		// The "no converter" message lists these, so the user is told where to look rather than
		// only that it failed.
		const std::vector<std::string> locations = PartManager::StepConverter::searchedLocations();
		TEST_ASSERT(locations.size() >= 2);
		TEST_ASSERT_M(locations.front().find(PartManager::StepConverter::ConverterEnvVar)
			!= std::string::npos, "the override variable must be named first");
	}
};

TEST_INSTANTIATE(TST_StepConverter);
