// @file PartManager_MeshBounds.h
// @brief The bounding box of a mesh file, so the viewer can frame it (§13).
//
// Qt3D does expose extents on a loaded geometry, but only once its bounding
// volume job has run — a frame or two after the mesh reports itself Ready, and
// through a frontend property that is empty until then. Framing the camera on
// that means either framing an empty box or waiting on a signal whose timing is
// not ours. Reading the file is deterministic and happens before the mesh is
// even handed to Qt3D, so the first frame drawn is already the right one.
//
// **Millimetres, Z up, part sitting on z = 0.** That is what KiCad's STEP models
// and FreeCAD's tessellation of them produce — measured, not assumed: the
// 2N7002 mesh comes out 2.4 x 2.9 x 1.2 with minZ exactly 0. It is what lets the
// board be drawn under the part rather than through it.
//
// Only the formats whose vertices can be read without a parser worth the name:
// STL (binary and ASCII) and OBJ. PLY and glTF report `ok == false` and the
// caller falls back to a fixed standoff — a viewer that frames those badly is a
// smaller problem than a mesh reader nobody asked for.
// @see docs/design/ARCHITECTURE.md §13
// @see PartManager_StepConverter.h, PartManager_Model3DFormat.h
#pragma once

#include "PartManager_global.h"
#include <string>

namespace PartManager
{

	struct PART_MANAGER_API MeshBounds
	{
		bool ok = false;
		double minX = 0.0, minY = 0.0, minZ = 0.0;
		double maxX = 0.0, maxY = 0.0, maxZ = 0.0;

		double sizeX() const { return maxX - minX; }
		double sizeY() const { return maxY - minY; }
		double sizeZ() const { return maxZ - minZ; }
		double centreX() const { return (minX + maxX) / 2.0; }
		double centreY() const { return (minY + maxY) / 2.0; }
		double centreZ() const { return (minZ + maxZ) / 2.0; }

		// Half the diagonal — the radius of the sphere that contains the whole mesh, which is
		// what a camera has to fit. Never zero for a valid box, so it is safe to divide by.
		double radius() const;
	};

	// Reads `path` and measures it. `ok` is false for an unreadable file, an unsupported format
	// or a file with no vertices in it — all of which mean "frame it some other way", not "fail".
	PART_MANAGER_API MeshBounds meshBoundsOf(const std::string& path);

}
