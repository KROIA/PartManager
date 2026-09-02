// @file PartManager_Model3DFormat.h
// @brief What a 3D model file is, and whether this build can draw it (§13, §5a).
//
// Pure string handling — no Qt, no filesystem, no renderer. It exists because
// "can we show this?" has to be answerable at the point a file is *attached*,
// not at the point someone clicks a viewer and gets a blank panel.
//
// **STEP is not a mesh format.** `.step`/`.stp` files are boundary
// representations (trimmed NURBS surfaces), so drawing one means tessellating
// analytic geometry — a CAD kernel's job, not a mesh loader's. Qt3D's geometry
// loaders read OBJ, PLY, STL and glTF, all of which are already triangles. That
// is the whole reason this enum distinguishes `renderable` from `recognised`:
// a STEP file is a perfectly valid, correctly-stored 3D model that this build
// cannot draw, and saying so is very different from failing to recognise it.
//
// KiCad ships both forms for its 3D models — a `.step` for mechanical CAD export
// and a `.wrl` for the board viewer. Neither is renderable here; the `.wrl` is
// VRML, which Qt3D also does not load.
// @see docs/design/ARCHITECTURE.md §5a, §13
// @see PartManager_PartFileRole.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	enum class PART_MANAGER_API Model3DFormat
	{
		Unknown,
		Obj,        // Wavefront OBJ    — renderable
		Stl,        // stereolithography — renderable
		Ply,        // Stanford polygon  — renderable
		Gltf,       // glTF / GLB        — renderable
		Step,       // STEP / STP        — recognised, needs a CAD kernel
		Vrml,       // VRML / X3D (.wrl) — recognised, KiCad's board-viewer model
		Iges        // IGES              — recognised, needs a CAD kernel
	};

	// Format from a filename or path, by extension, case-insensitively. Content is not sniffed:
	// every format here is identified by extension in every tool that produces them.
	PART_MANAGER_API Model3DFormat model3DFormatOf(const std::string& fileNameOrPath);

	// True when this build can actually put triangles on screen for it.
	PART_MANAGER_API bool isRenderableModel3D(Model3DFormat format);
	// True when the file is a 3D model at all — the test for "should this go in the 3D slot?".
	// A STEP file is stored and kept like any other model; it just cannot be drawn yet.
	PART_MANAGER_API bool isModel3D(Model3DFormat format);

	// Human name, e.g. "STEP". Not translated: these are format names, the same in every locale.
	PART_MANAGER_API std::string model3DFormatName(Model3DFormat format);

	// Every extension the 3D slot accepts, without the dot, in the order a file dialog should
	// list them (renderable first). One list, so the dialog filter and the validation cannot
	// disagree about what is allowed.
	PART_MANAGER_API std::vector<std::string> model3DExtensions();

}
