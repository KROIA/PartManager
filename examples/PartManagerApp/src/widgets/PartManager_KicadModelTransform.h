// @file PartManager_KicadModelTransform.h
// @brief Turns a footprint's `(rotate (xyz ...))` into the rotation KiCad draws.
//
// Its own header rather than a few lines inside the viewer because the *order*
// the three angles compose in is the whole content, it is invisible in the
// result until a part uses two of them at once, and it can be pinned by a test
// without dragging Qt3D into the test binary.
//
// KiCad's 3D renderer applies the model rotation Z, then Y, then X to the model
// *matrix*, which means a point goes through X first and Z last. QQuaternion's
// fromEulerAngles() composes Z, X, Y instead, so a footprint that rotates about
// two axes — the common `(rotate (xyz 90 0 -90))` of a vendor STEP authored
// Y-up, for one — lands on a different face in each. The angles are already
// negated by the parser (KiCad's are clockwise); only the order lives here.
// @see docs/design/ARCHITECTURE.md §13
// @see PartManager_KicadGeometry.h, PartManager_Model3DViewer.h
#pragma once

#include "kicad/PartManager_KicadGeometry.h"
#include <QQuaternion>
#include <QVector3D>

namespace PartManager
{

	inline QQuaternion kicadModelRotation(const KicadModelPlacement& placement)
	{
		// q1 * q2 rotates by q2 first, so this reads right-to-left: X, then Y, then Z.
		return QQuaternion::fromAxisAndAngle(QVector3D(0.0f, 0.0f, 1.0f),
				static_cast<float>(placement.rotateZ))
			* QQuaternion::fromAxisAndAngle(QVector3D(0.0f, 1.0f, 0.0f),
				static_cast<float>(placement.rotateY))
			* QQuaternion::fromAxisAndAngle(QVector3D(1.0f, 0.0f, 0.0f),
				static_cast<float>(placement.rotateX));
	}

}
