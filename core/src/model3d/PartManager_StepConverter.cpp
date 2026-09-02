#include "model3d/PartManager_StepConverter.h"
#include "PartManager_global.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

	std::string StepConverter::sourceHash(const std::string& sourcePath)
	{
		if (sourcePath.empty())
		{
			return std::string();
		}
		std::ifstream in(sourcePath, std::ios::binary);
		if (!in)
		{
			return std::string();
		}
		// Same FNV-1a as the path hash, over the bytes. A STEP file is a megabyte at the outside
		// and this runs once per preview, so streaming it is cheaper than the machinery a rolling
		// or partial hash would need — and a partial hash would miss an edit past the window.
		unsigned long long hash = 1469598103934665603ULL;
		char buffer[64 * 1024];
		while (in)
		{
			in.read(buffer, sizeof(buffer));
			const std::streamsize read = in.gcount();
			for (std::streamsize i = 0; i < read; ++i)
			{
				hash ^= static_cast<unsigned char>(buffer[i]);
				hash *= 1099511628211ULL;
			}
		}
		char text[17] = { 0 };
		std::snprintf(text, sizeof(text), "%016llx", hash);
		return text;
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
		// The stem keeps the name recognisable to a human poking around the folder; the hash is
		// what actually makes it unique. Content first, path only when there is no content to
		// read — a caller naming an entry for a file that does not exist yet still gets a stable
		// name, and it cannot collide with a real one.
		const std::string hash = sourceHash(sourcePath);
		// `.pmmesh`, not `.stl`: what is cached is a *set* of meshes and their colours, because
		// one mesh can only be one colour and a STEP file styles its solids separately. The
		// manifest is the entry the cache is keyed on; the meshes hang off it.
		const std::string name = source.stem().string() + "-"
			+ (hash.empty() ? hashPath(sourcePath) : hash) + ".pmmesh";
		return (std::filesystem::path(cacheRoot) / name).string();
	}

	std::vector<StepConverter::MeshPart> StepConverter::parseGeometryManifest(
		const std::string& text)
	{
		std::vector<MeshPart> parts;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			std::istringstream fields(line);
			std::string keyword;
			MeshPart part;
			if (!(fields >> keyword) || keyword != "solid")
			{
				continue;
			}
			if (!(fields >> part.file >> part.box.centreX >> part.box.centreY >> part.box.centreZ
				>> part.box.sizeX >> part.box.sizeY >> part.box.sizeZ >> part.box.volume))
			{
				continue;   // a truncated line is a conversion that died mid-write
			}
			parts.push_back(part);
		}
		return parts;
	}

	std::string StepConverter::meshSetManifest(const std::vector<MeshPart>& parts,
		const std::vector<StepColor>& colors)
	{
		std::string out = "pmmesh 1\n";
		for (size_t i = 0; i < parts.size(); ++i)
		{
			const StepColor color = i < colors.size() ? colors[i] : StepColor();
			char numbers[64] = { 0 };
			std::snprintf(numbers, sizeof(numbers), " %.5f %.5f %.5f", color.r, color.g, color.b);
			out += "mesh " + parts[i].file + numbers + "\n";
		}
		return out;
	}

	std::vector<StepConverter::MeshSetEntry> StepConverter::parseMeshSet(const std::string& text)
	{
		std::vector<MeshSetEntry> entries;
		std::istringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			std::istringstream fields(line);
			std::string keyword;
			MeshSetEntry entry;
			if (!(fields >> keyword) || keyword != "mesh")
			{
				continue;   // the "pmmesh 1" header, or a blank line
			}
			if (!(fields >> entry.file))
			{
				continue;
			}
			// A colour that will not parse leaves the default grey rather than dropping the
			// mesh: a model drawn in the wrong shade beats a model that is not there.
			if (!(fields >> entry.color.r >> entry.color.g >> entry.color.b))
			{
				entry.color = StepColor();
			}
			entries.push_back(entry);
		}
		return entries;
	}

	bool StepConverter::isCacheValid(const std::string& cacheRoot, const std::string& sourcePath)
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(sourcePath, error))
		{
			return false;
		}
		const std::string cached = cachedMeshPath(cacheRoot, sourcePath);
		if (cached.empty() || !std::filesystem::is_regular_file(cached, error))
		{
			return false;
		}
		// A conversion that was killed — the app closed, the machine slept — can leave a
		// zero-length file behind. Named after the right hash, it would be a permanent cache hit
		// for a mesh that does not exist.
		return std::filesystem::file_size(cached, error) > 0;
	}

	const int StepConverter::MaxSolids = 64;

	std::string StepConverter::conversionScript(const std::string& sourcePath,
		const std::string& manifestPath, const std::string& meshStem, double linearDeflection)
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
		script += "# Generated by PartManager - tessellates one STEP file into a mesh per solid,\n";
		script += "# plus a manifest giving each solid's bounding box so its colour can be matched.\n";
		script += "import sys\n";
		script += "import Part\n";
		script += "import MeshPart\n";
		script += "\n";
		script += "source = \"" + forwardSlashes(sourcePath) + "\"\n";
		script += "manifest = \"" + forwardSlashes(manifestPath) + "\"\n";
		script += "stem = \"" + forwardSlashes(meshStem) + "\"\n";
		script += "\n";
		script += "shape = Part.Shape()\n";
		script += "shape.read(source)\n";
		script += "solids = shape.Solids\n";
		// A shell or a surface model has no solids at all, and a housing with hundreds of them is
		// not worth an entity and a file each. Both end up as the single whole-shape mesh the
		// viewer draws in one colour, which is what this used to do for everything.
		script += "if len(solids) == 0 or len(solids) > " + std::to_string(MaxSolids)
			+ ":\n";
		script += "    solids = [shape]\n";
		script += "\n";
		script += "lines = []\n";
		script += "for index, solid in enumerate(solids):\n";
		// Relative=False so the deflection is in model units (mm for every STEP KiCad writes)
		// rather than a fraction of the bounding box, which would tessellate a small footprint
		// far more finely than a large one for no benefit.
		script += "    mesh = MeshPart.meshFromShape(Shape=solid, LinearDeflection="
			+ std::string(deflection) + ", AngularDeflection=0.5, Relative=False)\n";
		script += "    if mesh.CountFacets == 0:\n";
		script += "        continue\n";
		script += "    name = \"%s.%d.stl\" % (stem, index)\n";
		script += "    mesh.write(name)\n";
		script += "    box = solid.BoundBox\n";
		// The filename alone: the manifest sits beside its meshes, so an absolute path here
		// would stop the cache folder from ever being moved or copied.
		script += "    lines.append(\"solid %s %.6f %.6f %.6f %.6f %.6f %.6f %.6f\" % (\n";
		script += "        name.split(\"/\")[-1],\n";
		script += "        (box.XMin + box.XMax) / 2.0, (box.YMin + box.YMax) / 2.0,\n";
		script += "        (box.ZMin + box.ZMax) / 2.0,\n";
		script += "        box.XLength, box.YLength, box.ZLength, solid.Volume))\n";
		script += "\n";
		script += "if len(lines) == 0:\n";
		// No manifest at all rather than an empty one: the caller treats a missing manifest as a
		// failed conversion, and an empty one would be cached forever as "this model has nothing".
		script += "    sys.exit(2)\n";
		script += "handle = open(manifest, \"w\")\n";
		script += "handle.write(\"\\n\".join(lines) + \"\\n\")\n";
		script += "handle.close()\n";
		script += "sys.exit(0)\n";
		return script;
	}

}
