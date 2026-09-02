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

		// Where the mesh for `sourcePath` is cached: `<cacheRoot>/<stem>-<hash>.stl`. Keyed by a
		// hash of the full source path, so two parts whose models are both called `model.step`
		// cannot collide, and the same file always resolves to the same cache entry.
		static std::string cachedMeshPath(const std::string& cacheRoot, const std::string& sourcePath);

		// The FreeCAD script that does the tessellation, with the two paths already embedded.
		// Written to a temporary file and handed to the converter as its only argument.
		//
		// `linearDeflection` is the tessellation tolerance in model units (mm for every STEP file
		// KiCad produces). 0.1 mm is far finer than a component footprint needs on screen and
		// still fast; the knob exists because a large mechanical part may want coarser.
		static std::string conversionScript(const std::string& sourcePath,
			const std::string& outputPath, double linearDeflection = 0.1);

		// True when the cache entry exists and is newer than the source, i.e. nothing to do.
		// A source edited after conversion re-converts rather than showing a stale mesh.
		static bool isCacheValid(const std::string& cacheRoot, const std::string& sourcePath);
	};

}
