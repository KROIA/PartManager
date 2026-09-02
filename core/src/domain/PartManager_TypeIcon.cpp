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
		constexpr std::array<std::uint32_t, 12> Palette = {
			0x4E79A7,   // blue
			0xF28E2B,   // orange
			0xE15759,   // red
			0x76B7B2,   // teal
			0x59A14F,   // green
			0xB07AA1,   // purple
			0xEDC948,   // yellow
			0xFF9DA7,   // pink
			0x9C755F,   // brown
			0x8CD17D,   // light green
			0x86BCB6,   // sea green
			0x499894,   // deep teal
		};

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
			icon.colour = Palette[stableIndex(name, Palette.size())];
			icon.initials = initialsFor(typeName);
		}
		return icon;
	}

}
