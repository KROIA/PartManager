#pragma once

#include "UnitTest.h"
#include "model3d/PartManager_StepColors.h"
#include <cmath>
#include <string>
#include <vector>

// §13's per-solid colour. An STL carries none, so without this every model is one lump of grey;
// with it wrong, a part is drawn in confidently incorrect colours, which is worse. The pairing is
// the part that can be quietly wrong — see `fileOrderIsNotTheAnswer`.
class TST_StepColors : public UnitTest::Test
{
	TEST_CLASS(TST_StepColors)
public:
	TST_StepColors()
		: Test("TST_StepColors")
	{
		ADD_TEST(TST_StepColors::stylingIsFollowedToItsColour);
		ADD_TEST(TST_StepColors::solidsArePairedByWhereTheyAre);
		ADD_TEST(TST_StepColors::fileOrderIsNotTheAnswer);
		ADD_TEST(TST_StepColors::theFallbackUsesTheFilesOwnPalette);
		ADD_TEST(TST_StepColors::rubbishInNothingOut);
	}

private:

	static bool roughly(double actual, double expected)
	{
		return std::abs(actual - expected) < 1e-6;
	}

	// A two-solid STEP: a dark body around the origin and a light lead off to +x. Trimmed to the
	// entities that carry colour and position, which is all this reads.
	static std::string twoSolids()
	{
		return
			"ISO-10303-21;\nHEADER;\nENDSEC;\nDATA;\n"
			"#10 = CARTESIAN_POINT('',(-1.0,-1.0,0.0));\n"
			"#11 = CARTESIAN_POINT('',(1.0,1.0,2.0));\n"
			"#12 = VERTEX_POINT('',#10);\n"
			"#13 = VERTEX_POINT('',#11);\n"
			"#14 = CLOSED_SHELL('',(#12,#13));\n"
			"#15 = MANIFOLD_SOLID_BREP('body',#14);\n"
			"#20 = CARTESIAN_POINT('',(4.0,-0.5,0.0));\n"
			"#21 = CARTESIAN_POINT('',(5.0,0.5,0.5));\n"
			"#22 = VERTEX_POINT('',#20);\n"
			"#23 = VERTEX_POINT('',#21);\n"
			"#24 = CLOSED_SHELL('',(#22,#23));\n"
			"#25 = MANIFOLD_SOLID_BREP('lead',#24);\n"
			"#30 = COLOUR_RGB('',0.3,0.3,0.3);\n"
			"#31 = FILL_AREA_STYLE_COLOUR('',#30);\n"
			"#32 = FILL_AREA_STYLE('',(#31));\n"
			"#33 = SURFACE_STYLE_FILL_AREA(#32);\n"
			"#34 = SURFACE_SIDE_STYLE('',(#33));\n"
			"#35 = SURFACE_STYLE_USAGE(.BOTH.,#34);\n"
			"#36 = PRESENTATION_STYLE_ASSIGNMENT((#35));\n"
			"#40 = COLOUR_RGB('',0.734,0.773,0.797);\n"
			"#41 = FILL_AREA_STYLE_COLOUR('',#40);\n"
			"#42 = FILL_AREA_STYLE('',(#41));\n"
			"#43 = SURFACE_STYLE_FILL_AREA(#42);\n"
			"#44 = SURFACE_SIDE_STYLE('',(#43));\n"
			"#45 = SURFACE_STYLE_USAGE(.BOTH.,#44);\n"
			"#46 = PRESENTATION_STYLE_ASSIGNMENT((#45));\n"
			// Deliberately styled in the order lead-then-body, so a reader that pairs by file
			// order gets the wrong answer and this test says so.
			"#50 = STYLED_ITEM('',(#46),#25);\n"
			"#51 = STYLED_ITEM('',(#36),#15);\n"
			"ENDSEC;\nEND-ISO-10303-21;\n";
	}

	// Tests

	TEST_FUNCTION(stylingIsFollowedToItsColour)
	{
		TEST_START;

		const PartManager::StepStyles styles = PartManager::stepStylesOf(twoSolids());
		TEST_ASSERT(styles.ok);
		TEST_COMPARE(styles.solids.size(), static_cast<size_t>(2));
		TEST_COMPARE(styles.palette.size(), static_cast<size_t>(2));

		// Darkest first, which is what the fallback's "biggest solid is the housing" relies on.
		TEST_ASSERT(roughly(styles.palette.front().r, 0.3));
		TEST_ASSERT(roughly(styles.palette.back().r, 0.734));
		TEST_ASSERT(styles.palette.front().luminance() < styles.palette.back().luminance());

		// Each styled solid is measured from its own points, which is what the pairing needs.
		for (const PartManager::StepStyledSolid& solid : styles.solids)
		{
			if (roughly(solid.color.r, 0.3))
			{
				TEST_ASSERT_M(roughly(solid.centreX, 0.0), "the body sits on the origin");
				TEST_ASSERT(roughly(solid.sizeZ, 2.0));
			}
			else
			{
				TEST_ASSERT_M(roughly(solid.centreX, 4.5), "the lead sits off to +x");
			}
		}

		// A named colour instead of an RGB triple, which some exporters write.
		const PartManager::StepStyles named = PartManager::stepStylesOf(
			"ISO-10303-21;\nDATA;\n"
			"#1 = CARTESIAN_POINT('',(0.0,0.0,0.0));\n"
			"#2 = CLOSED_SHELL('',(#1));\n"
			"#3 = MANIFOLD_SOLID_BREP('x',#2);\n"
			"#4 = DRAUGHTING_PRE_DEFINED_COLOUR('black');\n"
			"#5 = FILL_AREA_STYLE_COLOUR('',#4);\n"
			"#6 = PRESENTATION_STYLE_ASSIGNMENT((#5));\n"
			"#7 = STYLED_ITEM('',(#6),#3);\n"
			"ENDSEC;\n");
		TEST_COMPARE(named.solids.size(), static_cast<size_t>(1));
		TEST_ASSERT(roughly(named.solids.front().color.r, 0.0));
	}

	TEST_FUNCTION(solidsArePairedByWhereTheyAre)
	{
		TEST_START;

		const PartManager::StepStyles styles = PartManager::stepStylesOf(twoSolids());

		// The meshes as the tessellator hands them over: body first, lead second — the opposite
		// order to the styling above.
		std::vector<PartManager::MeshSolidBox> solids = {
			{ 0.0, 0.0, 1.0, 2.0, 2.0, 2.0, 8.0 },     // body
			{ 4.5, 0.0, 0.25, 1.0, 1.0, 0.5, 0.5 },    // lead
		};

		bool exact = false;
		const std::vector<PartManager::StepColor> colors =
			PartManager::colorsForSolids(styles, solids, &exact);
		TEST_ASSERT_M(exact, "two solids in plainly different places must pair exactly");
		TEST_COMPARE(colors.size(), static_cast<size_t>(2));
		TEST_ASSERT_M(roughly(colors[0].r, 0.3), "the body takes the dark colour");
		TEST_ASSERT_M(roughly(colors[1].r, 0.734), "the lead takes the light one");
	}

	// The pairing this replaced, and why. Taking the nth styled solid as the nth mesh matches on
	// one real part and is wrong on the next — so the test is that position wins over order.
	TEST_FUNCTION(fileOrderIsNotTheAnswer)
	{
		TEST_START;

		const PartManager::StepStyles styles = PartManager::stepStylesOf(twoSolids());
		// The STEP styles the lead first and the body second; the meshes arrive body first.
		// Pairing by order would give the body the lead's colour.
		TEST_ASSERT_M(roughly(styles.solids.front().color.r, 0.734),
			"the fixture styles the lead first, on purpose");

		std::vector<PartManager::MeshSolidBox> solids = {
			{ 0.0, 0.0, 1.0, 2.0, 2.0, 2.0, 8.0 },
			{ 4.5, 0.0, 0.25, 1.0, 1.0, 0.5, 0.5 },
		};
		const std::vector<PartManager::StepColor> colors =
			PartManager::colorsForSolids(styles, solids);
		TEST_ASSERT_M(roughly(colors[0].r, 0.3),
			"the first mesh must take the colour of the solid it sits on, not the first style");
	}

	TEST_FUNCTION(theFallbackUsesTheFilesOwnPalette)
	{
		TEST_START;

		const PartManager::StepStyles styles = PartManager::stepStylesOf(twoSolids());

		// Meshes nowhere near the styled solids — what happens when solids share topology and
		// the boxes read from the text come out inflated, which is the real TNPW0603 case.
		std::vector<PartManager::MeshSolidBox> strangers = {
			{ 40.0, 40.0, 40.0, 1.0, 1.0, 1.0, 9.0 },   // biggest: the housing
			{ 50.0, 40.0, 40.0, 1.0, 1.0, 1.0, 0.5 },
			{ 60.0, 40.0, 40.0, 1.0, 1.0, 1.0, 0.5 },
		};
		bool exact = true;
		const std::vector<PartManager::StepColor> colors =
			PartManager::colorsForSolids(styles, strangers, &exact);
		TEST_ASSERT_M(!exact, "a pairing that does not resolve must say so rather than guess");
		TEST_COMPARE(colors.size(), static_cast<size_t>(3));
		// Still the file's own colours: the darkest for the body, the lightest for the rest.
		TEST_ASSERT_M(roughly(colors[0].r, 0.3), "the largest solid is the housing");
		TEST_ASSERT(roughly(colors[1].r, 0.734));
		TEST_ASSERT(roughly(colors[2].r, 0.734));

		// A file with no styling at all still has to produce something drawable: near-black
		// moulding and tinned metal, rather than everything one shade.
		const PartManager::StepStyles bare = PartManager::stepStylesOf(
			"ISO-10303-21;\nDATA;\n#1 = CARTESIAN_POINT('',(0.0,0.0,0.0));\nENDSEC;\n");
		TEST_ASSERT(bare.ok);
		TEST_ASSERT(bare.solids.empty() && bare.palette.empty());
		const std::vector<PartManager::StepColor> plain =
			PartManager::colorsForSolids(bare, strangers);
		TEST_ASSERT_M(plain[0].luminance() < plain[1].luminance(),
			"the housing must still come out darker than the leads");
	}

	TEST_FUNCTION(rubbishInNothingOut)
	{
		TEST_START;

		TEST_ASSERT(!PartManager::stepStylesOf("").ok);
		TEST_ASSERT(!PartManager::stepStylesOf("this is not a STEP file").ok);
		// Recognisably STEP but with nothing in it is ok-but-empty, which is a different thing
		// from unreadable and must not be reported as a failure.
		TEST_ASSERT(PartManager::stepStylesOf("ISO-10303-21;\nDATA;\n#1 = FOO('x');\nENDSEC;\n").ok);

		// No solids means no colours, and no crash.
		const PartManager::StepStyles styles = PartManager::stepStylesOf(twoSolids());
		TEST_ASSERT(PartManager::colorsForSolids(styles, {}).empty());

		// A '#' inside a string is not a reference, and digits inside one are not coordinates.
		// Counting either sends the walk somewhere arbitrary and the box with it.
		const PartManager::StepStyles quoted = PartManager::stepStylesOf(
			"ISO-10303-21;\nDATA;\n"
			"#1 = CARTESIAN_POINT('part #99 rev 7',(1.0,2.0,3.0));\n"
			"#2 = CLOSED_SHELL('',(#1));\n"
			"#3 = MANIFOLD_SOLID_BREP('x',#2);\n"
			"#4 = COLOUR_RGB('',1.0,0.0,0.0);\n"
			"#5 = FILL_AREA_STYLE_COLOUR('',#4);\n"
			"#6 = PRESENTATION_STYLE_ASSIGNMENT((#5));\n"
			"#7 = STYLED_ITEM('',(#6),#3);\n"
			"ENDSEC;\n");
		TEST_COMPARE(quoted.solids.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(roughly(quoted.solids.front().centreX, 1.0),
			"the name is a string; its digits are not coordinates");
		TEST_ASSERT(roughly(quoted.solids.front().color.r, 1.0));
	}
};

TEST_INSTANTIATE(TST_StepColors);
