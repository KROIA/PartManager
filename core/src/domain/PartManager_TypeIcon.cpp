#include "domain/PartManager_TypeIcon.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace PartManager
{

	namespace
	{
		std::string lower(const std::string& text)
		{
			std::string out = text;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		bool contains(const std::string& haystack, const char* needle)
		{
			return haystack.find(needle) != std::string::npos;
		}

		// Substring matching is fine for a word like "capacitor", which cannot turn up inside
		// something unrelated. It is wrong for the short ones: "ic" hides in "silicone" and
		// "ceramic", "led" in "modelled", "nut" in "walnut". Those have to match whole words or
		// a sheet of silicone rubber is filed as an integrated circuit.
		bool containsWord(const std::string& haystack, const std::string& word)
		{
			for (std::size_t at = haystack.find(word); at != std::string::npos;
				at = haystack.find(word, at + 1))
			{
				const bool startsClean = (at == 0)
					|| !std::isalnum(static_cast<unsigned char>(haystack[at - 1]));
				const std::size_t after = at + word.size();
				const bool endsClean = (after >= haystack.size())
					|| !std::isalnum(static_cast<unsigned char>(haystack[after]));
				if (startsClean && endsClean) { return true; }
			}
			return false;
		}

		// Hand-checked: every entry is dark enough for white text and distinct enough from its
		// neighbours to tell apart in a 28-pixel square. Unknown types index into this, so the
		// list is what guarantees a readable colour rather than luck.
		//
		// The names are not decoration: they are the vocabulary the icon picker and
		// set_category_icon offer, so this table is the single place a palette slot is spelled.
		// **The order is load-bearing** — stableIndex() indexes into it, so moving an entry
		// re-colours every Generic type in every existing database.
		struct PaletteEntry
		{
			const char* name;
			std::uint32_t rgb;
		};
		constexpr std::array<PaletteEntry, 12> Palette = { {
			{ "blue",        0x4E79A7 },
			{ "orange",      0xF28E2B },
			{ "red",         0xE15759 },
			{ "teal",        0x76B7B2 },
			{ "green",       0x59A14F },
			{ "purple",      0xB07AA1 },
			{ "yellow",      0xEDC948 },
			{ "pink",        0xFF9DA7 },
			{ "brown",       0x9C755F },
			{ "light green", 0x8CD17D },
			{ "sea green",   0x86BCB6 },
			{ "deep teal",   0x499894 },
		} };

		// Every TypeGlyph by the name it is stored and offered under. One entry per enumerator;
		// glyphName() returns the first match, so a missing row would come back empty rather than
		// wrong — which the round-trip test is there to catch.
		struct GlyphEntry
		{
			TypeGlyph glyph;
			const char* name;
		};
		constexpr std::array<GlyphEntry, 15> Glyphs = { {
			{ TypeGlyph::Resistor,   "Resistor"   },
			{ TypeGlyph::Capacitor,  "Capacitor"  },
			{ TypeGlyph::Inductor,   "Inductor"   },
			{ TypeGlyph::Diode,      "Diode"      },
			{ TypeGlyph::Led,        "Led"        },
			{ TypeGlyph::Transistor, "Transistor" },
			{ TypeGlyph::Ic,         "Ic"         },
			{ TypeGlyph::Connector,  "Connector"  },
			{ TypeGlyph::Crystal,    "Crystal"    },
			{ TypeGlyph::Switch,     "Switch"     },
			{ TypeGlyph::Relay,      "Relay"      },
			{ TypeGlyph::Fuse,       "Fuse"       },
			{ TypeGlyph::Sensor,     "Sensor"     },
			{ TypeGlyph::Mechanical, "Mechanical" },
			{ TypeGlyph::Generic,    "Generic"    },
		} };

		// FNV-1a, the same one FileStore uses for content hashes. Any stable hash works; the
		// requirement is only that a given type name always lands on the same colour, so the
		// table does not reshuffle its colours between runs.
		std::uint32_t stableIndex(const std::string& text, std::size_t buckets)
		{
			std::uint64_t hash = 14695981039346656037ULL;
			for (char c : text)
			{
				hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
				hash *= 1099511628211ULL;
			}
			return static_cast<std::uint32_t>(hash % buckets);
		}
	}

	std::string TypeIconStyle::initialsFor(const std::string& typeName)
	{
		std::string letters;
		bool atWordStart = true;
		for (char c : typeName)
		{
			const unsigned char raw = static_cast<unsigned char>(c);
			if (std::isalnum(raw))
			{
				if (atWordStart)
				{
					letters.push_back(static_cast<char>(std::toupper(raw)));
					atWordStart = false;
					if (letters.size() == 2) { return letters; }
				}
				continue;
			}
			atWordStart = true;
		}

		// A single word gives only one initial, which reads as an accident next to the two-letter
		// ones — so take its first two letters instead.
		if (letters.size() == 1)
		{
			for (char c : typeName)
			{
				const unsigned char raw = static_cast<unsigned char>(c);
				if (std::isalnum(raw) && letters.size() < 2 && letters[0] != std::toupper(raw))
				{
					letters.push_back(static_cast<char>(std::toupper(raw)));
					break;
				}
			}
		}
		return letters;
	}

	TypeIcon TypeIconStyle::forType(const std::string& typeName)
	{
		const std::string name = lower(typeName);
		TypeIcon icon;

		// Order matters, and the traps are all near-misses of each other:
		//   "LED" before "diode"          -- an LED is a diode by name
		//   "ceramic capacitor" is caught by "capacitor", which is the intent
		//   "mosfet" carries no "transistor" in it, so both spellings are listed
		//   "photoresistor" would match "resistor"; acceptable, it is one
		if (containsWord(name, "led") || contains(name, "leuchtdiode"))
		{
			icon.glyph = TypeGlyph::Led;        icon.colour = 0xEDC948;
		}
		else if (contains(name, "resistor") || contains(name, "widerstand"))
		{
			icon.glyph = TypeGlyph::Resistor;   icon.colour = 0x9C755F;
		}
		else if (contains(name, "capacitor") || contains(name, "kondensator"))
		{
			icon.glyph = TypeGlyph::Capacitor;  icon.colour = 0x4E79A7;
		}
		else if (contains(name, "inductor") || contains(name, "choke") || contains(name, "spule"))
		{
			icon.glyph = TypeGlyph::Inductor;   icon.colour = 0x59A14F;
		}
		else if (contains(name, "transistor") || contains(name, "mosfet") || contains(name, "igbt"))
		{
			icon.glyph = TypeGlyph::Transistor; icon.colour = 0xB07AA1;
		}
		else if (contains(name, "diode") || contains(name, "rectifier"))
		{
			icon.glyph = TypeGlyph::Diode;      icon.colour = 0x555555;
		}
		else if (contains(name, "connector") || contains(name, "header") || contains(name, "socket")
			|| contains(name, "terminal") || contains(name, "stecker"))
		{
			icon.glyph = TypeGlyph::Connector;  icon.colour = 0xF28E2B;
		}
		else if (contains(name, "crystal") || contains(name, "oscillator") || contains(name, "resonator")
			|| contains(name, "quarz"))
		{
			icon.glyph = TypeGlyph::Crystal;    icon.colour = 0x76B7B2;
		}
		else if (contains(name, "relay") || contains(name, "relais"))
		{
			icon.glyph = TypeGlyph::Relay;      icon.colour = 0xFF9DA7;
		}
		else if (contains(name, "switch") || contains(name, "button") || contains(name, "taster")
			|| contains(name, "schalter"))
		{
			icon.glyph = TypeGlyph::Switch;     icon.colour = 0x499894;
		}
		else if (contains(name, "fuse") || contains(name, "sicherung"))
		{
			icon.glyph = TypeGlyph::Fuse;       icon.colour = 0xE15759;
		}
		else if (contains(name, "sensor"))
		{
			icon.glyph = TypeGlyph::Sensor;     icon.colour = 0x8CD17D;
		}
		// Anything with pins in a package: microcontrollers, op-amps, logic, regulators.
		else if (containsWord(name, "ic") || contains(name, "microcontroller") || containsWord(name, "mcu")
			|| contains(name, "op-amp") || contains(name, "opamp") || contains(name, "amplifier")
			|| contains(name, "regulator") || contains(name, "logic") || contains(name, "memory")
			|| contains(name, "processor"))
		{
			icon.glyph = TypeGlyph::Ic;         icon.colour = 0x3C4B5A;
		}
		// The mechanical domain. Not seeded by default, but §2 supports it and a box of screws
		// should not look like a box of unknowns.
		else if (contains(name, "screw") || contains(name, "bolt") || containsWord(name, "nut")
			|| contains(name, "washer") || contains(name, "standoff") || contains(name, "spacer")
			|| contains(name, "schraube") || contains(name, "mutter"))
		{
			icon.glyph = TypeGlyph::Mechanical; icon.colour = 0x7F7F7F;
		}
		else
		{
			icon.glyph = TypeGlyph::Generic;
			icon.colour = Palette[stableIndex(name, Palette.size())].rgb;
			icon.initials = initialsFor(typeName);
		}
		return icon;
	}

	TypeIcon TypeIconStyle::resolve(const std::string& typeName, const std::string& storedGlyph,
		std::uint32_t storedColour)
	{
		TypeIcon icon = forType(typeName);

		TypeGlyph chosen = TypeGlyph::Generic;
		if (glyphFromName(storedGlyph, chosen))
		{
			icon.glyph = chosen;
			// The initials are only ever drawn on the Generic body, and forType() only fills them
			// when it landed there itself. A category whose name reads as a resistor but that was
			// deliberately given the Generic glyph would otherwise come out as an empty box.
			icon.initials = (chosen == TypeGlyph::Generic) ? initialsFor(typeName) : std::string();
		}
		if (storedColour != 0)
		{
			icon.colour = storedColour & 0xFFFFFFu;
		}
		return icon;
	}

	std::vector<std::string> TypeIconStyle::glyphNames()
	{
		std::vector<std::string> names;
		names.reserve(Glyphs.size());
		for (const GlyphEntry& entry : Glyphs)
		{
			names.push_back(entry.name);
		}
		return names;
	}

	std::string TypeIconStyle::glyphName(TypeGlyph glyph)
	{
		for (const GlyphEntry& entry : Glyphs)
		{
			if (entry.glyph == glyph)
			{
				return entry.name;
			}
		}
		return std::string();
	}

	bool TypeIconStyle::glyphFromName(const std::string& name, TypeGlyph& outGlyph)
	{
		if (name.empty())
		{
			return false;
		}
		const std::string wanted = lower(name);
		for (const GlyphEntry& entry : Glyphs)
		{
			if (lower(entry.name) == wanted)
			{
				outGlyph = entry.glyph;
				return true;
			}
		}
		return false;
	}

	std::vector<std::string> TypeIconStyle::paletteColourNames()
	{
		std::vector<std::string> names;
		names.reserve(Palette.size());
		for (const PaletteEntry& entry : Palette)
		{
			names.push_back(entry.name);
		}
		return names;
	}

	bool TypeIconStyle::paletteColourFromName(const std::string& name, std::uint32_t& outColour)
	{
		if (name.empty())
		{
			return false;
		}
		const std::string wanted = lower(name);
		for (const PaletteEntry& entry : Palette)
		{
			if (lower(entry.name) == wanted)
			{
				outColour = entry.rgb;
				return true;
			}
		}
		return false;
	}

	std::string TypeIconStyle::paletteColourName(std::uint32_t colour)
	{
		for (const PaletteEntry& entry : Palette)
		{
			if (entry.rgb == (colour & 0xFFFFFFu))
			{
				return entry.name;
			}
		}
		return std::string();
	}

}
