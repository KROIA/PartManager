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
// their own types and they should not all come out grey.
// @see docs/design/ARCHITECTURE.md §2, §7b, §7c, §12a
// @see PartManager_KicadSymbolWriter.h
#pragma once

#include "PartManager_global.h"
#include <cstdint>
#include <string>

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

		// Up to two letters standing in for a type nobody anticipated: the initials of its first
		// two words, or its first two letters when it is a single word.
		static std::string initialsFor(const std::string& typeName);
	};

}
