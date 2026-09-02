// @file PartManager_KicadGeometry.h
// @brief Turns `.kicad_sym` symbols and `.kicad_mod` footprints into flat shape lists to draw.
//
// **Why parse rather than shell out to KiCad.** The preview has to work for a
// part whose symbol PartManager generated and never wrote to disk, on a machine
// with no KiCad installed, inside a list that repaints while the user scrolls.
// Handing a file to an external renderer fails all three. The formats are
// s-expressions and the drawing subset is small, so reading them directly is
// less code than driving a tool would be.
//
// **What this deliberately is not.** It is a preview, not a schematic editor: no
// text layout, no fonts, no fill styles beyond "filled or not", no net or
// electrical meaning. Anything it does not recognise is skipped rather than
// approximated, so an unusual file draws less rather than drawing something
// misleading.
//
// Pure string in, shapes out — no Qt, no filesystem, no database, which is what
// keeps it testable and keeps `core/` widget-free (§12a).
//
// **Coordinates are KiCad's, unchanged.** Millimetres, and the two formats do not
// agree on which way is up: a symbol's Y axis points up, a footprint's points
// down. Normalising here would mean the numbers no longer matched the file they
// came from, so the flip stays with the painter — see `yAxisPointsUp`.
// @see docs/design/ARCHITECTURE.md §5a, §12a
// @see PartManager_KicadSymbolWriter.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	struct PART_MANAGER_API KicadPoint
	{
		double x = 0.0;
		double y = 0.0;
	};

	enum class KicadShapeKind
	{
		Polyline,       // points, in order; `closed` says whether to join the last to the first
		Rectangle,      // points[0] and points[1] are opposite corners
		Circle,         // points[0] is the centre, `radius` the radius
		Arc,            // points are start, mid, end — KiCad's three-point form
		Pin,            // points[0] is the connection point, points[1] the body end
		Pad             // points[0] is the centre, `sizeX`/`sizeY` the extent
	};

	// One thing to draw. Deliberately one struct rather than a class hierarchy: the painter
	// switches on `kind` once, and a preview never needs to do anything else with these.
	struct PART_MANAGER_API KicadShape
	{
		KicadShapeKind kind = KicadShapeKind::Polyline;
		std::vector<KicadPoint> points;
		double radius = 0.0;
		double strokeWidth = 0.0;   // millimetres; 0 means "the painter picks a hairline"
		bool filled = false;
		bool closed = false;
		bool roundPad = false;      // circle/oval pads, as opposed to rectangular ones
		// Pads only. A through-hole pad exists on *both* sides of the board and has a hole
		// through it, which a flat preview can ignore and a 3D one cannot: drawn as surface
		// copper it puts a THT part's pins on top of a board they are supposed to pass through.
		bool throughHole = false;
		double drillDiameter = 0.0;  // millimetres, 0 when the pad has no hole
		// Pads only: the third number of `(at x y angle)`, degrees anticlockwise in KiCad's
		// board view. Ignoring it draws every rotated pad at 90 degrees to itself — an SOT-23's
		// pads come out tall and narrow instead of wide and short, which looks like a plausible
		// footprint for a different package.
		double rotationDegrees = 0.0;
		double sizeX = 0.0;
		double sizeY = 0.0;
		std::string layer;          // footprints only: "F.Cu", "F.SilkS", "F.CrtYd", ...
		std::string label;          // a pin's number, or a pad's
	};

	// Where a footprint's `(model ...)` entry says its 3D model goes. **A model file's own
	// origin is not where the part sits.** Vendor libraries routinely author a STEP around the
	// top of the body, or on its side, and put the correction here — so a viewer that loads the
	// mesh and draws it raw plants half the library inside the board or lying down.
	//
	// Measured: the TNPW0603 resistor's model runs z = -0.55 .. 0 and its footprint offsets it
	// by 0.021653543776415, which is 0.55 mm expressed in inches. Applied, the body lands on
	// z = 0 where every other part is.
	struct PART_MANAGER_API KicadModelPlacement
	{
		// False when the footprint names no model. Everything below is then the identity, so a
		// caller may apply it unconditionally rather than branching.
		bool present = false;
		// Millimetres, already converted — the legacy `(at (xyz ...))` form is in *inches* and
		// the current `(offset (xyz ...))` form is in millimetres, which is the trap: read the
		// old one as millimetres and the correction becomes a fortieth of what it should be,
		// close enough to zero to look like no offset at all.
		double offsetX = 0.0, offsetY = 0.0, offsetZ = 0.0;
		double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
		// Degrees, already negated. KiCad stores these the opposite way round from the rotation
		// it then applies — a VRML-era convention its own 3D viewer still honours, so a file's
		// numbers only match the picture after the sign flip.
		double rotateX = 0.0, rotateY = 0.0, rotateZ = 0.0;
	};

	struct PART_MANAGER_API KicadDrawing
	{
		std::vector<KicadShape> shapes;
		std::string name;
		// Footprints only: where the part's 3D model goes relative to this footprint. Not a
		// shape, so it is not in `shapes` and never affects `bounds()` — it comes from the same
		// file and there is no sense parsing that file twice to get it.
		KicadModelPlacement model3D;
		// Symbols measure Y upward, footprints downward. The painter needs to know which,
		// because getting it wrong mirrors the part instead of failing visibly.
		bool yAxisPointsUp = true;

		bool empty() const { return shapes.empty(); }
		// The extent of everything in `shapes`, for fitting the drawing to a widget. False when
		// there is nothing to measure, which spares every caller a divide-by-zero guard.
		bool bounds(KicadPoint& outMin, KicadPoint& outMax) const;
	};

	class PART_MANAGER_API KicadGeometry
	{
		KicadGeometry() = delete;
	public:
		// The drawing for one symbol out of a `.kicad_sym` library.
		//
		// Resolves `(extends "<base>")` against the same library text, which is where a
		// PartManager-generated symbol keeps its body — every generated part is an `extends`
		// of an embedded base, so without this the common case draws nothing at all.
		//
		// An empty `symbolName` takes the first symbol that is not a base, which is what a
		// single-symbol vendor file wants.
		static KicadDrawing symbol(const std::string& libraryText, const std::string& symbolName);

		// The drawing for a `.kicad_mod` footprint file.
		static KicadDrawing footprint(const std::string& footprintText);

		// KiCad stores an arc as three points on it. Anything that wants to draw one — a painter
		// with a bounding box and two angles, a mesh builder walking it in steps — needs the
		// circle behind those three points first, so the circumcentre maths lives here rather
		// than once per renderer.
		//
		// `outSpanAngle` sweeps from start to end *the way that passes through the middle point*,
		// which is the whole reason KiCad stores three: the short way round is wrong for exactly
		// the arcs worth drawing. Angles are radians in the drawing's own coordinates.
		//
		// False for collinear points, where there is no circle at all — the caller draws the
		// straight run instead, which is what a zero-curvature arc looks like anyway.
		static bool arcCircle(const KicadPoint& start, const KicadPoint& mid, const KicadPoint& end,
			KicadPoint& outCentre, double& outRadius, double& outStartAngle, double& outSpanAngle);

		// What a part with no symbol attached yet will look like once its library is generated:
		// the embedded base its type maps to. Shared by the editor and the main window preview
		// so the two cannot disagree about what "no symbol yet" means.
		static KicadDrawing genericSymbolForType(const std::string& typeName);
	};

}
