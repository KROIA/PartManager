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
