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
		double sizeX = 0.0;
		double sizeY = 0.0;
		std::string layer;          // footprints only: "F.Cu", "F.SilkS", "F.CrtYd", ...
		std::string label;          // a pin's number, or a pad's
	};

	struct PART_MANAGER_API KicadDrawing
	{
		std::vector<KicadShape> shapes;
		std::string name;
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

		// What a part with no symbol attached yet will look like once its library is generated:
		// the embedded base its type maps to. Shared by the editor and the main window preview
		// so the two cannot disagree about what "no symbol yet" means.
		static KicadDrawing genericSymbolForType(const std::string& typeName);
	};

}
