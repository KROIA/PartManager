// @file PartManager_StepColors.h
// @brief The colours a STEP file gives its solids, and which mesh each one belongs to (§13).
//
// A STEP file carries colour — `STYLED_ITEM` -> `PRESENTATION_STYLE_ASSIGNMENT` ->
// ... -> `COLOUR_RGB` — and an STL carries none, so a model tessellated the
// usual way arrives as one lump of grey. Reading it back means reading the STEP
// text, because **FreeCAD's colour-aware STEP reader is GUI-only**: `ImportGui`
// refuses to load in both `FreeCADCmd` and `FreeCAD --console` (measured), and
// the console `Import` module drops colour entirely.
//
// **Pairing colour with geometry is the hard half, and file order does not do
// it.** The obvious approach — nth styled solid is the nth mesh — matches on a
// 2N7002 and is wrong on a TNPW0603, where it gives the two end terminations
// different colours and makes the ceramic body light instead of dark. So each
// styled solid is measured (the bounding box of its own points) and paired with
// the mesh solid it actually sits on top of. Positions come from the same file
// on both sides, so this is arithmetic rather than inference — and when it does
// not resolve cleanly, `colorsForSolids` says so and falls back rather than
// handing back a confident wrong answer.
//
// **Per-face styling is out of scope.** A TNPW0603 also styles three
// `ADVANCED_FACE`s (the printed marking); honouring those needs per-face
// tessellation, so those faces take their solid's colour.
//
// Pure text in, numbers out — no Qt, no filesystem (§12a).
// @see docs/design/ARCHITECTURE.md §13
// @see PartManager_StepConverter.h, PartManager_MeshBounds.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	struct PART_MANAGER_API StepColor
	{
		// 0..1, as STEP stores them. The default is the neutral grey a model with no styling
		// at all gets, so an unset colour is never black-on-black.
		double r = 0.60, g = 0.62, b = 0.65;

		// Rough perceptual lightness, for telling a housing from a lead.
		double luminance() const { return 0.2126 * r + 0.7152 * g + 0.0722 * b; }
	};

	// One styled solid: its colour and where it is, so it can be paired with a mesh.
	struct PART_MANAGER_API StepStyledSolid
	{
		StepColor color;
		double centreX = 0.0, centreY = 0.0, centreZ = 0.0;
		double sizeX = 0.0, sizeY = 0.0, sizeZ = 0.0;
	};

	struct PART_MANAGER_API StepStyles
	{
		// False when the text could not be read as STEP at all. A valid file with no styling is
		// ok == true with everything empty, which is a different thing and not an error.
		bool ok = false;
		std::vector<StepStyledSolid> solids;
		// Every distinct colour in the file, darkest first. What the fallback coloring works
		// from, so even an unpairable model is drawn in its own palette rather than in ours.
		std::vector<StepColor> palette;
	};

	// One mesh solid as the tessellator measured it. The pairing is on these numbers.
	struct PART_MANAGER_API MeshSolidBox
	{
		double centreX = 0.0, centreY = 0.0, centreZ = 0.0;
		double sizeX = 0.0, sizeY = 0.0, sizeZ = 0.0;
		double volume = 0.0;
	};

	PART_MANAGER_API StepStyles stepStylesOf(const std::string& stepText);

	// A colour for every entry of `solids`, in the same order.
	//
	// Exact when every mesh solid pairs with a styled solid close enough to be certain — set
	// `outExact` to see which happened. Otherwise the housing/metal fallback: the largest solid
	// is the body and takes the palette's darkest colour, the rest are leads and take its
	// lightest. On the two real parts measured that fallback lands on the right answer anyway,
	// which is what makes it safe to prefer over drawing nothing.
	PART_MANAGER_API std::vector<StepColor> colorsForSolids(const StepStyles& styles,
		const std::vector<MeshSolidBox>& solids, bool* outExact = nullptr);

}
