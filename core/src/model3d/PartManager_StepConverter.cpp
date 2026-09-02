#include "model3d/PartManager_StepConverter.h"
#include "PartManager_global.h"

#include <cstdio>
#include <filesystem>
#include <system_error>

#if QT_ENABLED
	#include <QString>
#endif

namespace PartManager
{

	const char* const StepConverter::ConverterEnvVar = "PARTMANAGER_STEP_CONVERTER";

	namespace
	{
		// FreeCAD's console binary. `FreeCADCmd` is the Windows name; `freecadcmd` the one used
		// by most Linux packages. Both take a .py file as their only argument and exit when it
		// finishes, which is exactly the shape needed here.
		const char* const ConverterNames[] = { "FreeCADCmd.exe", "FreeCADCmd", "freecadcmd" };

		// Where FreeCAD normally installs itself. Globbed rather than fixed-version because the
		// folder carries the version ("FreeCAD 0.21"), and hard-coding one would go stale.
		const char* const InstallRoots[] = {
			"C:/Program Files",
			"C:/Program Files (x86)",
		};

		std::string readEnv(const char* name)
		{
#if QT_ENABLED
			// qEnvironmentVariable rather than std::getenv: getenv is C4996 under MSVC and this
			// library promotes C4996 to an error.
			return qEnvironmentVariable(name).toStdString();
#else
			PM_UNUSED(name);
			return std::string();
#endif
		}

		bool isExecutableFile(const std::filesystem::path& path)
		{
			std::error_code error;
			return std::filesystem::is_regular_file(path, error);
		}

		// A stable, short hash of a path, so two models both called `model.step` get distinct
		// cache entries. Same FNV-1a the filestore uses, and for the same reason: this is a name,
		// not an integrity check.
		std::string hashPath(const std::string& text)
		{
			unsigned long long hash = 1469598103934665603ULL;
			for (unsigned char c : text)
			{
				hash ^= c;
				hash *= 1099511628211ULL;
			}
			char buffer[17] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%016llx", hash);
			return buffer;
		}
	}

	std::vector<std::string> StepConverter::searchedLocations()
	{
		std::vector<std::string> locations;
		locations.push_back(std::string("$") + ConverterEnvVar);
		for (const char* root : InstallRoots)
		{
			locations.push_back(std::string(root) + "/FreeCAD */bin/FreeCADCmd.exe");
		}
		locations.push_back("FreeCADCmd on PATH");
		return locations;
	}

	std::string StepConverter::converterPath()
	{
		// 1. An explicit override always wins, including over a working install — that is what
		// makes pointing at a different build or a different converter possible at all.
		const std::string configured = readEnv(ConverterEnvVar);
		if (!configured.empty() && isExecutableFile(configured))
		{
			return configured;
		}

		// 2. The usual install locations. The version is in the folder name, so this globs rather
		// than guessing a version that will be wrong in six months.
		std::error_code error;
		for (const char* root : InstallRoots)
		{
			if (!std::filesystem::is_directory(root, error))
			{
				continue;
			}
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::directory_iterator(root, error))
			{
				if (!entry.is_directory(error))
				{
					continue;
				}
				const std::string name = entry.path().filename().string();
				if (name.rfind("FreeCAD", 0) != 0)
				{
					continue;
				}
				for (const char* binary : ConverterNames)
				{
					const std::filesystem::path candidate = entry.path() / "bin" / binary;
					if (isExecutableFile(candidate))
					{
						return candidate.string();
					}
				}
			}
		}

		// 3. PATH. Returned as a bare name for QProcess to resolve — checking every PATH entry
		// here would duplicate what the process launcher already does correctly.
		const std::string pathVar = readEnv("PATH");
		if (!pathVar.empty())
		{
			size_t start = 0;
			while (start <= pathVar.size())
			{
				const size_t end = pathVar.find(';', start);
				const std::string directory = pathVar.substr(start,
					end == std::string::npos ? std::string::npos : end - start);
				if (!directory.empty())
				{
					for (const char* binary : ConverterNames)
					{
						if (isExecutableFile(std::filesystem::path(directory) / binary))
						{
							return (std::filesystem::path(directory) / binary).string();
						}
					}
				}
				if (end == std::string::npos)
				{
					break;
				}
				start = end + 1;
			}
		}
		return std::string();
	}

	bool StepConverter::isAvailable()
	{
		return !converterPath().empty();
	}

	std::string StepConverter::cachedMeshPath(const std::string& cacheRoot,
		const std::string& sourcePath)
	{
		if (cacheRoot.empty() || sourcePath.empty())
		{
			return std::string();
		}
		const std::filesystem::path source(sourcePath);
		// The stem keeps the name recognisable to a human poking around the folder; the hash of
		// the full path is what actually makes it unique.
		const std::string name = source.stem().string() + "-" + hashPath(sourcePath) + ".stl";
		return (std::filesystem::path(cacheRoot) / name).string();
	}

	bool StepConverter::isCacheValid(const std::string& cacheRoot, const std::string& sourcePath)
	{
		const std::string cached = cachedMeshPath(cacheRoot, sourcePath);
		std::error_code error;
		if (cached.empty() || !std::filesystem::is_regular_file(cached, error)
			|| !std::filesystem::is_regular_file(sourcePath, error))
		{
			return false;
		}
		// A source replaced after conversion re-converts. The filestore is content-addressed so
		// this should never happen through the app, but a hand-edited filestore should not leave
		// the viewer showing the wrong shape.
		return std::filesystem::last_write_time(cached, error)
			>= std::filesystem::last_write_time(sourcePath, error);
	}

	std::string StepConverter::conversionScript(const std::string& sourcePath,
		const std::string& outputPath, double linearDeflection)
	{
		// Paths go in as Python raw-ish strings with backslashes normalised to forward slashes,
		// which Windows accepts everywhere and which cannot be mistaken for an escape.
		auto forwardSlashes = [](const std::string& path)
		{
			std::string out;
			out.reserve(path.size());
			for (char c : path)
			{
				// A quote in a path would break out of the literal. They are not legal in Windows
				// filenames and the filestore never generates one, so dropping is safe and beats
				// emitting a script that will not parse.
				if (c == '"' || c == '\'')
				{
					continue;
				}
				out += (c == '\\') ? '/' : c;
			}
			return out;
		};

		char deflection[32] = { 0 };
		std::snprintf(deflection, sizeof(deflection), "%.4f", linearDeflection);

		std::string script;
		script += "# Generated by PartManager - tessellates one STEP file into an STL the viewer can draw.\n";
		script += "import sys\n";
		script += "import Part\n";
		script += "import MeshPart\n";
		script += "\n";
		script += "source = \"" + forwardSlashes(sourcePath) + "\"\n";
		script += "target = \"" + forwardSlashes(outputPath) + "\"\n";
		script += "\n";
		script += "shape = Part.Shape()\n";
		script += "shape.read(source)\n";
		// Relative=False so the deflection is in model units (mm for every STEP KiCad writes)
		// rather than a fraction of the bounding box, which would tessellate a small footprint
		// far more finely than a large one for no benefit.
		script += "mesh = MeshPart.meshFromShape(Shape=shape, LinearDeflection=" + std::string(deflection)
			+ ", AngularDeflection=0.5, Relative=False)\n";
		script += "mesh.write(target)\n";
		script += "sys.exit(0)\n";
		return script;
	}

}
