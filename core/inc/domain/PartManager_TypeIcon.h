// @file PartManager_TypeIcon.h
// @brief Which glyph and colour stand in for a part that has no photo.
//
// Most parts never get a product photo — a CSV import brings none, and Mouser's
// CDN does not always give one up — so "no image" is the normal state, not an
// exception. A blank first column makes every such row look alike, which is
// exactly what the thumbnail column exists to prevent.
//
// **Classification lives here, painting lives in the app.** This header is a pure
// name-to-style mapping with no Qt in it, so the rules are testable without a
// screen and `core/` stays widget-free (§12a). It deliberately does not know how
// a resistor is drawn.
//
// **Colours come from a fixed palette, never computed freely.** An arbitrary
// hash-to-RGB produces unreadable mud about a third of the time; picking a slot
// out of a hand-checked palette cannot. Types nobody anticipated still get a
// stable, distinct colour that way — which matters, because the user can create
// their own types and they should not all come out grey. That property is why
// both the icon picker and the `set_category_icon` tool offer the palette *by
// name* and take no free RGB: a model picking hex would do no better than the
// hash did, and the twelve names are a vocabulary it can answer from.
//
// **A category may also store its own glyph and colour (v13).** `resolve()` is
// what everything paints through: stored values win, and anything left unset
// falls back to `forType()`. `forType()` is unchanged and stays the answer for
// every category that predates the stored columns, which is all of them.
// @see docs/design/ARCHITECTURE.md §2, §7b, §7c, §12a, §14c
// @see PartManager_KicadSymbolWriter.h, PartManager_PartType.h
#pragma once

#include "PartManager_global.h"
#include <cstdint>
#include <string>
#include <vector>

namespace PartManager
{

	// The shapes the app knows how to draw. Coarser than the type list on purpose: a Logic IC
	// and a Microcontroller are both a chip in a 28-pixel square, and pretending otherwise
	// would be detail nobody can see.
	enum class TypeGlyph
	{
		Resistor,
		Capacitor,
		Inductor,
		Diode,
		Led,
		Transistor,
		Ic,
		Connector,
		Crystal,
		Switch,
		Relay,
		Fuse,
		Sensor,
		Mechanical,
		Generic       // a plain body carrying `initials` — the honest "no idea what this is"
	};

	struct PART_MANAGER_API TypeIcon
	{
		TypeGlyph glyph = TypeGlyph::Generic;
		std::uint32_t colour = 0;   // 0xRRGGBB
		std::string initials;       // 1-2 upper-case letters, drawn on the Generic glyph
	};

	class PART_MANAGER_API TypeIconStyle
	{
		TypeIconStyle() = delete;
	public:
		// The placeholder for `typeName`. Matching is case-insensitive and by substring, in the
		// same conservative spirit as KicadSymbolWriter::baseSymbolForType() — an unrecognised
		// name falls through to Generic rather than to whichever bucket happens to be close.
		//
		// An empty name is a valid input (a part whose type row is gone) and yields Generic.
		static TypeIcon forType(const std::string& typeName);

		// What a category actually draws: `part_type.icon_glyph` / `icon_colour` where they are
		// set, forType() for whatever is not. An empty `storedGlyph` and a `storedColour` of 0
		// mean "unset", so this is forType() exactly for every category that has never been given
		// an icon — which is what makes it safe to route every painter through here.
		static TypeIcon resolve(const std::string& typeName, const std::string& storedGlyph,
			std::uint32_t storedColour);

		// Up to two letters standing in for a type nobody anticipated: the initials of its first
		// two words, or its first two letters when it is a single word.
		static std::string initialsFor(const std::string& typeName);

		// ---- the two stored vocabularies -------------------------------------------------------
		// Both are lists of names rather than raw values, and both round-trip: what is stored is
		// the name, what is offered is the name, and an unknown one is a refusal rather than a
		// silent default. The glyph picker, the colour picker and the two LLM tools all read
		// these, so there is one spelling of each vocabulary and not four.

		// Every TypeGlyph by name, in enum order. These are the identifiers themselves
		// ("Resistor", "Ic", "Generic"), so the stored column reads as what it means.
		static std::vector<std::string> glyphNames();
		// The name of one glyph. Never empty — every enumerator is in the table.
		static std::string glyphName(TypeGlyph glyph);
		// Name -> glyph, case-insensitively ("led" and "LED" both reach TypeGlyph::Led). False for
		// an empty name and for anything not in the list, which is how "unset" and "the model
		// answered with an emoji" stay distinguishable from a real choice at the call site.
		static bool glyphFromName(const std::string& name, TypeGlyph& outGlyph);

		// The twelve hand-checked palette colours by name, in palette order.
		static std::vector<std::string> paletteColourNames();
		// Name -> 0xRRGGBB, case-insensitively. False for anything not in the palette.
		static bool paletteColourFromName(const std::string& name, std::uint32_t& outColour);
		// The palette name of `colour`, or "" when it is not a palette entry — which the glyph
		// colours deliberately are not (a Diode is grey, and grey is not on offer as a choice).
		static std::string paletteColourName(std::uint32_t colour);
	};

}
