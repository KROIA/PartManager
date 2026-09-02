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
		ADD_TEST(TST_StepConverter::theMeshSetRoundTrips);
		ADD_TEST(TST_StepConverter::staleCacheIsNotTreatedAsValid);
		ADD_TEST(TST_StepConverter::identicalModelsShareOneMesh);
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
		// The cache entry is a mesh *set* manifest, not a single mesh.
		TEST_ASSERT(a.rfind(".pmmesh") == a.size() - 7);

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
			"C:\\db\\new\\model.step", "C:\\cache\\model-0123.pmmesh",
			"C:\\cache\\model-0123");

		TEST_ASSERT_M(script.find("\\\\") == std::string::npos,
			"no path backslash may survive into the generated script:\n" + script);
		TEST_ASSERT(script.find("C:/db/new/model.step") != std::string::npos);
		TEST_ASSERT(script.find("C:/cache/model-0123.pmmesh") != std::string::npos);
		TEST_ASSERT(script.find("C:/cache/model-0123\"") != std::string::npos);

		// The pieces that actually do the work.
		TEST_ASSERT(script.find("import MeshPart") != std::string::npos);
		TEST_ASSERT(script.find("shape.read(source)") != std::string::npos);
		TEST_ASSERT(script.find("mesh.write(name)") != std::string::npos);
		// Relative=False keeps the deflection in millimetres rather than a fraction of the
		// bounding box, so a small footprint is not tessellated absurdly finely.
		TEST_ASSERT(script.find("Relative=False") != std::string::npos);

		// One mesh per solid, which is what makes per-solid colour possible at all — a single
		// mesh can only ever be one colour.
		TEST_ASSERT(script.find("shape.Solids") != std::string::npos);
		TEST_ASSERT_M(script.find("for index, solid in enumerate(solids)") != std::string::npos,
			"the script must tessellate each solid separately");
		// ...and the box that lets the STEP's colours be matched to what came out.
		TEST_ASSERT(script.find("solid.BoundBox") != std::string::npos);
		TEST_ASSERT(script.find("solid.Volume") != std::string::npos);
		// A shell with no solids, or a housing with hundreds, falls back to one whole-shape mesh.
		TEST_ASSERT(script.find("solids = [shape]") != std::string::npos);
		// Nothing written means a failed conversion, not an empty model cached forever.
		TEST_ASSERT(script.find("sys.exit(2)") != std::string::npos);

		// A quote in a path would close the literal early; it cannot occur in a Windows filename
		// and is dropped rather than allowed to produce a script that will not parse.
		const std::string quoted = PartManager::StepConverter::conversionScript(
			"/tmp/we\"ird.step", "/tmp/out.pmmesh", "/tmp/out");
		TEST_ASSERT(quoted.find("we\"ird") == std::string::npos);
		TEST_ASSERT(quoted.find("/tmp/weird.step") != std::string::npos);
	}

	// The two files the conversion produces and the viewer reads back. A manifest that round
	// trips wrongly is a model drawn in the wrong colours, or not drawn at all.
	TEST_FUNCTION(theMeshSetRoundTrips)
	{
		TEST_START;

		// What the converter's script writes: one line per solid, with the box that pairs it
		// with a colour.
		const std::vector<PartManager::StepConverter::MeshPart> parts =
			PartManager::StepConverter::parseGeometryManifest(
				"solid model-abc.0.stl 0.000000 0.000000 0.628300 1.300000 2.900000 1.143500 4.059110\n"
				"solid model-abc.1.stl -0.925000 0.960000 0.303700 0.550000 0.440000 0.607300 0.085280\n"
				"garbage line that must be skipped\n"
				"solid model-abc.2.stl 0.925000\n");   // truncated: a conversion that died
		TEST_COMPARE(parts.size(), static_cast<size_t>(2));
		TEST_COMPARE(parts[0].file, std::string("model-abc.0.stl"));
		TEST_ASSERT(std::abs(parts[0].box.volume - 4.05911) < 1e-6);
		TEST_ASSERT(std::abs(parts[1].box.centreY - 0.96) < 1e-9);

		// ...and what it becomes once the colours are attached.
		std::vector<PartManager::StepColor> colors = {
			{ 0.3, 0.3, 0.3 }, { 0.734, 0.773, 0.797 }
		};
		const std::string manifest = PartManager::StepConverter::meshSetManifest(parts, colors);
		TEST_ASSERT(manifest.find("pmmesh 1") != std::string::npos);

		const std::vector<PartManager::StepConverter::MeshSetEntry> entries =
			PartManager::StepConverter::parseMeshSet(manifest);
		TEST_COMPARE(entries.size(), static_cast<size_t>(2));
		TEST_COMPARE(entries[0].file, std::string("model-abc.0.stl"));
		TEST_ASSERT(std::abs(entries[0].color.r - 0.3) < 1e-5);
		TEST_ASSERT(std::abs(entries[1].color.b - 0.797) < 1e-5);

		// Filenames only, never absolute paths — the cache folder has to survive being moved.
		TEST_ASSERT_M(manifest.find(":/") == std::string::npos && manifest.find("//") == std::string::npos,
			"the manifest must name its meshes relatively:\n" + manifest);

		// A colour that will not parse leaves the default rather than dropping the mesh: a model
		// in the wrong shade beats a model that is not there.
		const std::vector<PartManager::StepConverter::MeshSetEntry> damaged =
			PartManager::StepConverter::parseMeshSet("pmmesh 1\nmesh only-a-name.stl\n");
		TEST_COMPARE(damaged.size(), static_cast<size_t>(1));
		TEST_COMPARE(damaged[0].color.r, PartManager::StepColor().r);
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
			"a converted mesh for this exact source is a usable cache entry");

		// A source replaced after conversion must re-convert rather than show the old shape. The
		// entry is keyed on content, so the new content asks for a name that is not there — and
		// touching the file without changing it must *not* throw the mesh away, which is the half
		// a timestamp check gets wrong every time a filestore is copied or restored.
		const std::filesystem::file_time_type before = std::filesystem::last_write_time(cached);
		std::filesystem::last_write_time(source, before + std::chrono::hours(1));
		TEST_ASSERT_M(PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()),
			"a source that was only touched must keep its mesh");

		touch(source, "ISO-10303-21;\n/* now a different model */");
		TEST_ASSERT_M(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()),
			"changed source content must invalidate the cache");
		TEST_ASSERT_M(PartManager::StepConverter::cachedMeshPath(cacheRoot.string(), source.string())
			!= cached.string(), "changed content must name a different entry");

		// A conversion killed halfway leaves an empty file. Named after the right hash, it would
		// otherwise be a cache hit forever for a mesh that was never written.
		touch(source, "ISO-10303-21;");
		touch(cached, "");
		TEST_ASSERT(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()));

		// A source that is gone is not a valid cache either, however fresh the mesh looks.
		touch(cached, "solid\nendsolid\n");
		std::filesystem::remove(source);
		TEST_ASSERT(!PartManager::StepConverter::isCacheValid(cacheRoot.string(), source.string()));
	}

	TEST_FUNCTION(identicalModelsShareOneMesh)
	{
		TEST_START;

		// KiCad ships one model per footprint, so a reel of the same package really does give
		// several parts byte-identical STEP files. Keyed on content, they convert once.
		const std::filesystem::path folder = scratch("share");
		const std::filesystem::path a = folder / "a" / "model.step";
		const std::filesystem::path b = folder / "b" / "model.step";
		const std::filesystem::path c = folder / "c" / "model.step";
		touch(a, "ISO-10303-21;shape");
		touch(b, "ISO-10303-21;shape");
		touch(c, "ISO-10303-21;other");

		TEST_COMPARE(PartManager::StepConverter::cachedMeshPath("C:/cache", a.string()),
			PartManager::StepConverter::cachedMeshPath("C:/cache", b.string()));
		TEST_ASSERT_M(PartManager::StepConverter::cachedMeshPath("C:/cache", a.string())
			!= PartManager::StepConverter::cachedMeshPath("C:/cache", c.string()),
			"different content in the same filename must not share a mesh");

		// An unreadable source still gets a name, and still a distinct one per path — that is
		// what lets a caller ask where a not-yet-converted model *will* land.
		TEST_ASSERT(PartManager::StepConverter::sourceHash(
			(folder / "missing.step").string()).empty());
		TEST_ASSERT(PartManager::StepConverter::cachedMeshPath("C:/cache",
			(folder / "missing.step").string())
			!= PartManager::StepConverter::cachedMeshPath("C:/cache",
				(folder / "gone.step").string()));
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
