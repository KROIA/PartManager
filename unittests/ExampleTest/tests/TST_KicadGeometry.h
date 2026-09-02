#pragma once

#include "UnitTest.h"
#include "kicad/PartManager_KicadGeometry.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include <cmath>
#include <string>

// The preview draws what KiCad would draw. The test that matters most is the `extends` one:
// every PartManager-generated symbol is a bare `(extends "PM_...")` with no body of its own, so
// a parser that only reads the block it was handed renders an empty rectangle for every part in
// the database and looks like a broken preview rather than a broken parser.
class TST_KicadGeometry : public UnitTest::Test
{
	TEST_CLASS(TST_KicadGeometry)
public:
	TST_KicadGeometry()
		: Test("TST_KicadGeometry")
	{
		ADD_TEST(TST_KicadGeometry::aGeneratedSymbolDrawsItsBaseBody);
		ADD_TEST(TST_KicadGeometry::aVendorSymbolKeepsItsOwnPins);
		ADD_TEST(TST_KicadGeometry::footprintPadsAndSilkscreen);
		ADD_TEST(TST_KicadGeometry::throughHolePadsCarryTheirHole);
		ADD_TEST(TST_KicadGeometry::aTurnedPadKeepsItsAngle);
		ADD_TEST(TST_KicadGeometry::aFootprintSaysWhereItsModelGoes);
		ADD_TEST(TST_KicadGeometry::anArcSweepsThroughItsMiddlePoint);
		ADD_TEST(TST_KicadGeometry::rubbishInNothingOut);
	}

private:

	static int countOf(const PartManager::KicadDrawing& drawing, PartManager::KicadShapeKind kind)
	{
		int count = 0;
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind == kind) { ++count; }
		}
		return count;
	}

	// A generated part carries no graphics — it extends an embedded base. Resolving that is the
	// whole reason this parser takes the library rather than the symbol block.
	TEST_FUNCTION(aGeneratedSymbolDrawsItsBaseBody)
	{
		TEST_START;

		PartManager::KicadSymbolSpec spec;
		spec.name = "R_10k";
		spec.baseSymbol = "PM_R";
		spec.reference = "R";
		spec.value = "10k";
		std::vector<std::string> blocks = PartManager::KicadSymbolWriter::baseSymbolBlocks();
		blocks.push_back(PartManager::KicadSymbolWriter::symbolBlock(spec));
		const std::string library = PartManager::KicadSymbolWriter::library(blocks);

		const PartManager::KicadDrawing drawing =
			PartManager::KicadGeometry::symbol(library, "R_10k");
		TEST_COMPARE(drawing.name, std::string("R_10k"));
		TEST_ASSERT_M(!drawing.empty(), "a generated symbol must draw its base's body");
		// The PM_R base is a box with a pin at each end.
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Rectangle), 1);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Pin), 2);
		TEST_ASSERT_M(drawing.yAxisPointsUp, "symbol coordinates measure Y upward");

		// A pin runs from its connection point *into* the body. Drawn the other way round every
		// pin would sit inside the box, which looks plausible enough to ship unnoticed.
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pin) { continue; }
			TEST_COMPARE(shape.points.size(), static_cast<std::size_t>(2));
			TEST_ASSERT_M(std::abs(shape.points[1].y) < std::abs(shape.points[0].y),
				"a pin's far end must be closer to the body than its connection point");
		}

		PartManager::KicadPoint min, max;
		TEST_ASSERT_M(drawing.bounds(min, max), "a non-empty drawing must have bounds");
		TEST_ASSERT_M(max.x > min.x && max.y > min.y, "bounds must have real extent");

		// Asking for nothing in particular must not return one of the embedded bases — every
		// generated library contains all of them, so "the first symbol" is always a resistor.
		const PartManager::KicadDrawing unnamed =
			PartManager::KicadGeometry::symbol(library, "");
		TEST_COMPARE(unnamed.name, std::string("R_10k"));
	}

	// A vendor symbol out of an ECAD zip has a real body and must be drawn as-is.
	TEST_FUNCTION(aVendorSymbolKeepsItsOwnPins)
	{
		TEST_START;

		// The shape SamacSys writes, trimmed to what is drawn.
		const std::string library =
			"(kicad_symbol_lib (version 20211014) (generator SamacSys_ECAD_Model)\n"
			"  (symbol \"74HC4051PW\" (in_bom yes) (on_board yes)\n"
			"    (symbol \"74HC4051PW_0_1\"\n"
			"      (rectangle (start -12.7 10.16) (end 12.7 -10.16)\n"
			"        (stroke (width 0.254) (type default)) (fill (type none)))\n"
			"      (circle (center 0 5.08) (radius 1.27)\n"
			"        (stroke (width 0.2) (type default)) (fill (type none)))\n"
			"      (polyline (pts (xy -2.54 0) (xy 2.54 0) (xy 0 2.54))\n"
			"        (stroke (width 0.2) (type default)) (fill (type none)))\n"
			"    )\n"
			"    (symbol \"74HC4051PW_1_1\"\n"
			"      (pin input line (at -17.78 7.62 0) (length 5.08)\n"
			"        (name \"A0\" (effects (font (size 1.27 1.27))))\n"
			"        (number \"1\" (effects (font (size 1.27 1.27)))))\n"
			"      (pin input line (at 17.78 7.62 180) (length 5.08)\n"
			"        (name \"Y0\" (effects (font (size 1.27 1.27))))\n"
			"        (number \"2\" (effects (font (size 1.27 1.27)))))\n"
			"    )\n"
			"  )\n"
			")\n";

		const PartManager::KicadDrawing drawing =
			PartManager::KicadGeometry::symbol(library, "74HC4051PW");
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Rectangle), 1);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Circle), 1);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Polyline), 1);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Pin), 2);

		// Angle 0 runs to +x, angle 180 to -x. Getting this backwards mirrors the pinout.
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pin) { continue; }
			// Tolerance, not equality: the end point is computed as at + length * cos(angle),
			// so it lands a few ULPs off the round number the file implies.
			if (shape.label == "1")
			{
				TEST_ASSERT_M(std::abs(shape.points[1].x - (-12.7)) < 1e-9,
					"pin 1 runs rightward, into the body");
			}
			else if (shape.label == "2")
			{
				TEST_ASSERT_M(std::abs(shape.points[1].x - 12.7) < 1e-9,
					"pin 2 runs leftward, into the body");
			}
		}
	}

	TEST_FUNCTION(footprintPadsAndSilkscreen)
	{
		TEST_START;

		const std::string footprint =
			"(footprint \"SOP65P640X110-16N\" (version 20221018) (layer \"F.Cu\")\n"
			"  (fp_line (start -3.2 -2.4) (end 3.2 -2.4)\n"
			"    (stroke (width 0.12) (type solid)) (layer \"F.SilkS\"))\n"
			"  (fp_circle (center -2.5 -1.8) (end -2.3 -1.8)\n"
			"    (stroke (width 0.12) (type solid)) (fill none) (layer \"F.SilkS\"))\n"
			"  (pad \"1\" smd roundrect (at -2.7 -1.95) (size 1.5 0.45)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (pad \"2\" smd oval (at -2.7 -1.3) (size 1.5 0.45)\n"
			"    (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n"
			"  (fp_text reference \"REF**\" (at 0 -3.2) (layer \"F.SilkS\"))\n"
			")\n";

		const PartManager::KicadDrawing drawing = PartManager::KicadGeometry::footprint(footprint);
		TEST_COMPARE(drawing.name, std::string("SOP65P640X110-16N"));
		TEST_ASSERT_M(!drawing.yAxisPointsUp, "footprint coordinates measure Y downward");
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Pad), 2);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Polyline), 1);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Circle), 1);

		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pad) { continue; }
			TEST_COMPARE(shape.sizeX, 1.5);
			TEST_COMPARE(shape.sizeY, 0.45);
			TEST_COMPARE(shape.layer, std::string("F.Cu"));
			// Only oval and circle pads are round; a roundrect is close enough to a rectangle
			// at preview size and must not be drawn as an ellipse.
			TEST_COMPARE(shape.roundPad, shape.label == "2");
		}

		// A circle's radius comes from a point on the rim, not from a radius field.
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind == PartManager::KicadShapeKind::Circle)
			{
				TEST_ASSERT_M(std::abs(shape.radius - 0.2) < 1e-9, "radius comes from the rim point");
			}
		}

		// A pad's extent is its size, not its centre — bounds built from centres alone would
		// clip exactly the parts a footprint is mostly made of.
		PartManager::KicadPoint min, max;
		TEST_ASSERT(drawing.bounds(min, max));
		TEST_ASSERT_M(min.x <= -3.45, "the leftmost pad's own width must be inside the bounds");

		// An SMD pad has copper on one face and no hole. The 3D board draws the bottom face only
		// for through-hole pads, so getting this wrong doubles every SMD pad onto the underside.
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pad) { continue; }
			TEST_ASSERT(!shape.throughHole);
			TEST_COMPARE(shape.drillDiameter, 0.0);
		}
	}

	// Every side-entry package in the KiCad library turns its pads. Dropped, an SOT-23's pads
	// come out tall and narrow instead of wide and short — a plausible-looking footprint for a
	// package that is not the one on screen.
	TEST_FUNCTION(aTurnedPadKeepsItsAngle)
	{
		TEST_START;

		// The real 2N7002 footprint's pad line, verbatim.
		const std::string footprint =
			"(module \"SOT96P240X120-3N\" (layer F.Cu)\n"
			"  (pad 1 smd rect (at -1.05 -0.96 90) (size 0.65 1.2) (layers F.Cu F.Paste F.Mask))\n"
			"  (pad 3 smd rect (at 1.05 0) (size 0.65 1.2) (layers F.Cu F.Paste F.Mask))\n"
			")\n";

		const PartManager::KicadDrawing drawing = PartManager::KicadGeometry::footprint(footprint);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Pad), 2);
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pad) { continue; }
			TEST_COMPARE(shape.rotationDegrees, shape.label == "1" ? 90.0 : 0.0);
			// The size itself is untouched — it is the *drawing* that turns, so a renderer that
			// ignores the angle is visibly wrong rather than quietly given pre-swapped numbers.
			TEST_COMPARE(shape.sizeX, 0.65);
			TEST_COMPARE(shape.sizeY, 1.2);
		}

		// Bounds have to be of the turned pad, or the fit clips the wide axis of every one.
		// Pad 1 turned 90 degrees reaches 1.2/2 in x from -1.05, i.e. to -1.65.
		PartManager::KicadPoint min, max;
		TEST_ASSERT(drawing.bounds(min, max));
		TEST_ASSERT_M(std::abs(min.x + 1.65) < 1e-9,
			"a pad turned 90 degrees is 1.2 wide, not 0.65");
		// Pad 3 is upright, so the right edge is the plain half-width.
		TEST_ASSERT_M(std::abs(max.x - 1.375) < 1e-9, "an upright pad must not be turned too");
		// And in y the turned pad is now the short one: -0.96 - 0.65/2.
		TEST_ASSERT(std::abs(min.y + 1.285) < 1e-9);
	}

	// Through-hole is what makes a DIP a DIP: copper on both faces and a hole between them. Read
	// as surface copper it puts the leads on top of a board they are supposed to pass through.
	TEST_FUNCTION(throughHolePadsCarryTheirHole)
	{
		TEST_START;

		const std::string footprint =
			"(footprint \"DIP-8_W7.62mm\" (version 20221018) (layer \"F.Cu\")\n"
			"  (pad \"1\" thru_hole rect (at -3.81 -3.81) (size 1.6 1.6) (drill 0.8)\n"
			"    (layers \"*.Cu\" \"*.Mask\"))\n"
			"  (pad \"2\" thru_hole oval (at -3.81 -1.27) (size 1.6 1.6) (drill oval 0.9 1.4)\n"
			"    (layers \"*.Cu\" \"*.Mask\"))\n"
			"  (pad \"MP\" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2)\n"
			"    (layers \"*.Cu\" \"*.Mask\"))\n"
			"  (pad \"9\" smd rect (at 2.54 0) (size 1 1) (layers \"F.Cu\"))\n"
			")\n";

		const PartManager::KicadDrawing drawing = PartManager::KicadGeometry::footprint(footprint);
		TEST_COMPARE(countOf(drawing, PartManager::KicadShapeKind::Pad), 4);

		int throughHoles = 0;
		for (const PartManager::KicadShape& shape : drawing.shapes)
		{
			if (shape.kind != PartManager::KicadShapeKind::Pad) { continue; }
			if (shape.label == "1")
			{
				TEST_ASSERT(shape.throughHole);
				TEST_COMPARE(shape.drillDiameter, 0.8);
			}
			else if (shape.label == "2")
			{
				TEST_ASSERT(shape.throughHole);
				// "(drill oval 0.9 1.4)" — atom 1 is the word "oval", so reading it as a number
				// gives a hole of zero, i.e. a through-hole pad that renders as solid copper.
				TEST_ASSERT_M(std::abs(shape.drillDiameter - 0.9) < 1e-9,
					"an oval drill's size starts one atom later");
			}
			else if (shape.label == "MP")
			{
				TEST_ASSERT_M(shape.throughHole, "np_thru_hole is still a hole through the board");
				TEST_COMPARE(shape.drillDiameter, 3.2);
			}
			else
			{
				TEST_ASSERT_M(!shape.throughHole, "an smd pad has no hole and one copper face");
			}
			if (shape.throughHole) { ++throughHoles; }
		}
		TEST_COMPARE(throughHoles, 3);
	}

	// Where the 3D model goes. A model file's own origin is not where the part sits: the TNPW0603
	// resistor's STEP runs z = -0.55 .. 0, entirely below the board, and only the footprint's
	// offset puts it back on top. Both footprints below are the real ones, verbatim.
	TEST_FUNCTION(aFootprintSaysWhereItsModelGoes)
	{
		TEST_START;

		// The 2N7002's, whose placement is the identity — which is exactly why ignoring the
		// placement altogether looked like it worked.
		const PartManager::KicadDrawing upright = PartManager::KicadGeometry::footprint(
			"(module \"SOT96P240X120-3N\" (layer F.Cu)\n"
			"  (model 2N7002-7-F.stp\n"
			"    (at (xyz 0 0 0))\n"
			"    (scale (xyz 1 1 1))\n"
			"    (rotate (xyz 0 0 0))\n"
			"  )\n"
			")\n");
		TEST_ASSERT(upright.model3D.present);
		TEST_COMPARE(upright.model3D.offsetZ, 0.0);
		TEST_COMPARE(upright.model3D.scaleX, 1.0);
		TEST_COMPARE(upright.model3D.rotateZ, 0.0);

		// The resistor's. The legacy "(at (xyz ...))" is in *inches*: 0.021653543776415 in is
		// 0.55 mm, which is exactly the height of a model that would otherwise sit entirely
		// inside the board. Read as millimetres it is a fortieth of that — near enough to zero
		// to look like no offset at all, which is the whole failure.
		const PartManager::KicadDrawing sunken = PartManager::KicadGeometry::footprint(
			"(module \"RESC1608X55N\" (layer F.Cu)\n"
			"  (model TNPW060330K0BXEA.stp\n"
			"    (at (xyz 0 0 0.021653543776415))\n"
			"    (scale (xyz 1 1 1))\n"
			"    (rotate (xyz 0 0 0))\n"
			"  )\n"
			")\n");
		TEST_ASSERT(sunken.model3D.present);
		TEST_ASSERT_M(std::abs(sunken.model3D.offsetZ - 0.55) < 1e-6,
			"the legacy model offset is in inches and must be converted");
		// Applied to that model's own box, the body lands on the board rather than under it.
		TEST_ASSERT(std::abs((-0.55 + sunken.model3D.offsetZ) - 0.0) < 1e-6);

		// The current spelling is millimetres and must *not* be multiplied.
		const PartManager::KicadDrawing modern = PartManager::KicadGeometry::footprint(
			"(footprint \"X\" (layer \"F.Cu\")\n"
			"  (model \"X.step\" (offset (xyz 0 0 0.55)) (scale (xyz 1 1 1))\n"
			"    (rotate (xyz 0 0 90)))\n"
			")\n");
		TEST_ASSERT(std::abs(modern.model3D.offsetZ - 0.55) < 1e-9);
		// KiCad stores the rotation the opposite way round from the one it applies; negated once
		// here so no renderer has to remember it.
		TEST_COMPARE(modern.model3D.rotateZ, -90.0);

		// No model entry is the identity, so a caller can apply the placement unconditionally.
		const PartManager::KicadDrawing none = PartManager::KicadGeometry::footprint(
			"(footprint \"X\" (layer \"F.Cu\")\n"
			"  (pad \"1\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\"))\n"
			")\n");
		TEST_ASSERT(!none.model3D.present);
		TEST_COMPARE(none.model3D.offsetZ, 0.0);
		TEST_COMPARE(none.model3D.scaleX, 1.0);

		// A zero scale would collapse the part to a point; treated as unscaled instead.
		const PartManager::KicadDrawing broken = PartManager::KicadGeometry::footprint(
			"(footprint \"X\" (layer \"F.Cu\")\n"
			"  (model \"X.step\" (offset (xyz 0 0 0)) (scale (xyz 0 0 0)))\n"
			")\n");
		TEST_COMPARE(broken.model3D.scaleX, 1.0);
		TEST_COMPARE(broken.model3D.scaleZ, 1.0);

		// The placement is not a shape and must not move the 2D bounds.
		PartManager::KicadPoint min, max;
		TEST_ASSERT(!sunken.bounds(min, max));
	}

	// The circumcentre behind KiCad's three-point arc. It used to live in the preview widget's
	// anonymous namespace with no test at all; the 3D board walks the same arc, so one wrong
	// sweep would now be wrong in two places at once.
	TEST_FUNCTION(anArcSweepsThroughItsMiddlePoint)
	{
		TEST_START;

		PartManager::KicadPoint centre;
		double radius = 0.0, start = 0.0, span = 0.0;

		// A half circle of radius 1 about the origin, from (1,0) up over (0,1) to (-1,0).
		TEST_ASSERT(PartManager::KicadGeometry::arcCircle({ 1.0, 0.0 }, { 0.0, 1.0 },
			{ -1.0, 0.0 }, centre, radius, start, span));
		TEST_ASSERT(std::abs(centre.x) < 1e-9 && std::abs(centre.y) < 1e-9);
		TEST_ASSERT(std::abs(radius - 1.0) < 1e-9);
		TEST_ASSERT(std::abs(start) < 1e-9);
		TEST_ASSERT_M(std::abs(span - 3.14159265358979323846) < 1e-9,
			"the sweep must go the way the middle point lies, anticlockwise here");

		// The same two endpoints with the middle point on the other side must sweep the other
		// way. This is the whole reason KiCad stores three points, and the case a "shortest arc"
		// implementation gets wrong while looking perfectly correct on the first one.
		TEST_ASSERT(PartManager::KicadGeometry::arcCircle({ 1.0, 0.0 }, { 0.0, -1.0 },
			{ -1.0, 0.0 }, centre, radius, start, span));
		TEST_ASSERT_M(span < 0.0, "a middle point below the axis must sweep clockwise");
		TEST_ASSERT(std::abs(span + 3.14159265358979323846) < 1e-9);

		// Walking the sweep must actually land on the middle point, which is the property both
		// renderers rely on and neither would notice losing.
		const double midAngle = start + span / 2.0;
		TEST_ASSERT(std::abs(centre.x + radius * std::cos(midAngle) - 0.0) < 1e-9);
		TEST_ASSERT(std::abs(centre.y + radius * std::sin(midAngle) + 1.0) < 1e-9);

		// Three points on a line have no circle. Reported, not approximated with a vast radius
		// that would make a straight silkscreen edge into a thousand-segment curve.
		TEST_ASSERT(!PartManager::KicadGeometry::arcCircle({ 0.0, 0.0 }, { 1.0, 1.0 },
			{ 2.0, 2.0 }, centre, radius, start, span));
	}

	// Anything unreadable must come back empty rather than half-drawn: a preview that renders
	// three stray lines of a corrupt file is worse than one that says nothing.
	TEST_FUNCTION(rubbishInNothingOut)
	{
		TEST_START;

		TEST_ASSERT(PartManager::KicadGeometry::symbol("", "X").empty());
		TEST_ASSERT(PartManager::KicadGeometry::symbol("not an s-expression", "X").empty());
		TEST_ASSERT(PartManager::KicadGeometry::symbol("(kicad_symbol_lib", "X").empty());
		TEST_ASSERT(PartManager::KicadGeometry::footprint("").empty());
		TEST_ASSERT(PartManager::KicadGeometry::footprint("(kicad_symbol_lib (symbol \"a\"))").empty());

		// A symbol that extends itself must not hang the UI.
		const std::string loop =
			"(kicad_symbol_lib (symbol \"A\" (extends \"B\")) (symbol \"B\" (extends \"A\")))";
		TEST_ASSERT(PartManager::KicadGeometry::symbol(loop, "A").empty());

		PartManager::KicadPoint min, max;
		TEST_ASSERT_M(!PartManager::KicadDrawing().bounds(min, max),
			"an empty drawing has no bounds to report");
	}
};

TEST_INSTANTIATE(TST_KicadGeometry);
