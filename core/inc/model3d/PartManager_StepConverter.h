// @file PartManager_StepConverter.h
// @brief Turns a STEP file into a mesh the viewer can draw, once, via an external CAD tool (§13).
//
// STEP is trimmed-NURBS boundary representation: drawing it means tessellating
// analytic surfaces, which is a CAD kernel's job. Rather than linking one
// (OpenCASCADE is a large dependency for one panel), this shells out to a tool
// the user very likely already has — FreeCAD's command-line binary — and caches
// the resulting STL beside the source in the filestore. Converted once, drawn
// every time after.
//
// **Everything here is pure path and string handling.** Locating the converter,
// naming the cache file and generating the script are all testable without
// running anything; actually running it is the caller's job (the app runs it
// asynchronously through QProcess so a slow tessellation does not freeze the
// window). That split is deliberate — the parts that can be wrong quietly are
// the paths, not the subprocess.
//
// **No converter is a normal state, not an error.** `converterPath()` returns
// empty and the viewer says the STEP file is stored and how to preview it. A
// STEP file is never rejected for want of a converter: §5a's KiCad export wants
// exactly those files.
// @see docs/design/ARCHITECTURE.md §13, §5a
// @see PartManager_Model3DFormat.h
#pragma once

#include "PartManager_global.h"
#include "model3d/PartManager_StepColors.h"
#include <string>
#include <vector>

namespace PartManager
{

	class PART_MANAGER_API StepConverter
	{
		StepConverter() = delete;
	public:
		// Overrides the search below with an explicit binary path. Set this when FreeCAD lives
		// somewhere unusual, or to point at a different converter that takes the same
		// `<binary> <script.py>` shape.
		static const char* const ConverterEnvVar;

		// The converter to run, or empty when none was found. Search order: the environment
		// variable, then the usual Windows install locations, then whatever is on PATH.
		static std::string converterPath();
		static bool isAvailable();

		// The candidate locations searched, in order — exposed so the "no converter" message can
		// say where PartManager looked instead of just that it failed.
		static std::vector<std::string> searchedLocations();

		// A hash of the source file's *contents*, or empty when it cannot be read. This is what
		// keys the cache: a STEP file that changed hashes differently and therefore names a
		// different mesh, so a stale mesh is never reachable rather than merely detected.
		static std::string sourceHash(const std::string& sourcePath);

		// One tessellated solid, as the converter's own manifest reports it. The box is what
		// pairs the mesh with the colour the STEP gives that solid.
		struct PART_MANAGER_API MeshPart
		{
			std::string file;       // filename only, relative to the manifest's folder
			MeshSolidBox box;
		};

		// One entry of the cached mesh set: a mesh file and the colour to draw it in.
		struct PART_MANAGER_API MeshSetEntry
		{
			std::string file;
			StepColor color;
		};

		// The geometry manifest the conversion script writes, read back. One line per solid.
		static std::vector<MeshPart> parseGeometryManifest(const std::string& text);

		// The cached mesh set, written next to the meshes it names and read back by the viewer.
		static std::string meshSetManifest(const std::vector<MeshPart>& parts,
			const std::vector<StepColor>& colors);
		static std::vector<MeshSetEntry> parseMeshSet(const std::string& text);

		// Where the mesh for `sourcePath` is cached: `<cacheRoot>/<stem>-<hash>.stl`, with the
		// hash from sourceHash(). Two parts whose models are both called `model.step` cannot
		// collide, two parts sharing one model share one mesh, and a model that was replaced
		// converts again instead of resolving to the old shape.
		//
		// The hash falls back to one of the path when the file is not readable, so callers that
		// only want the name of a not-yet-existing entry still get a stable, distinct one.
		static std::string cachedMeshPath(const std::string& cacheRoot, const std::string& sourcePath);

		// The FreeCAD script that does the tessellation, with the paths already embedded. Written
		// to a temporary file and handed to the converter as its only argument.
		//
		// **One mesh per solid, not one per model.** A STEP file styles its solids separately —
		// a black moulding and three tinned leads — and a single mesh can only be one colour.
		// The script writes `<meshStem>.<i>.stl` for each solid and a manifest at
		// `manifestPath` giving each one's bounding box, which is what lets the colours in the
		// STEP text be matched to the meshes that came out.
		//
		// `linearDeflection` is the tessellation tolerance in model units (mm for every STEP file
		// KiCad produces). 0.1 mm is far finer than a component footprint needs on screen and
		// still fast; the knob exists because a large mechanical part may want coarser.
		static std::string conversionScript(const std::string& sourcePath,
			const std::string& manifestPath, const std::string& meshStem,
			double linearDeflection = 0.1);

		// Above this a model is meshed whole instead of per solid. A component is a handful of
		// solids; a connector housing can be hundreds, and one entity and one file each is a
		// cost with no matching gain — nobody is reading the colour of the 200th pin.
		static const int MaxSolids;

		// True when the mesh for this exact source content is already on disk, i.e. nothing to do.
		// Timestamps are not consulted: the entry is named after the source's content hash, so an
		// edited source asks for a name that does not exist yet and converts. An empty file is not
		// a hit — a conversion killed halfway leaves one, and it would otherwise be cached forever.
		static bool isCacheValid(const std::string& cacheRoot, const std::string& sourcePath);
	};

}
