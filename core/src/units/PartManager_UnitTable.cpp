#include "units/PartManager_UnitTable.h"

namespace PartManager
{

	const char* const UnitTable::OhmSymbol = "\xCE\xA9";  // U+03A9
	const char* const UnitTable::MicroSign = "\xC2\xB5";  // U+00B5

	namespace
	{
		// One row of the §2a SI-prefix table. `token` is matched byte-exact and case-sensitively:
		// swapping 'm' (milli) and 'M' (mega) is a factor of 10^9 error, so no case folding here, ever.
		struct PrefixEntry
		{
			const char* token;
			int exponent;
			double multiplier;
		};

		const PrefixEntry g_prefixes[] = {
			{ "p",          -12, 1e-12 },
			{ "n",           -9, 1e-9  },
			{ "\xC2\xB5",    -6, 1e-6  },  // µ, listed first so it is the one prefixForExponent() returns
			{ "u",           -6, 1e-6  },  // same prefix as µ, ASCII spelling
			{ "m",           -3, 1e-3  },
			{ "k",            3, 1e3   },
			{ "M",            6, 1e6   },
			{ "G",            9, 1e9   },
		};

		// ASCII-only lowercase. UTF-8 continuation bytes are >= 0x80 and never touched.
		char toLowerAscii(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}
	}

	// The §2a dropdown, in spec order. Index 0 is "" = "(no unit)".
	const std::vector<std::string>& UnitTable::units()
	{
		static const std::vector<std::string> table = {
			"",             // (no unit)
			OhmSymbol,      // Ω  resistance
			"F",            // capacitance
			"H",            // inductance
			"V",            // voltage
			"A",            // current
			"W",            // power
			"Hz",           // frequency
			"s",            // time
			"%",            // ratio
			"mm"            // length
		};
		return table;
	}

	// True for any dropdown value, including "" ("(no unit)").
	bool UnitTable::isValidUnit(const std::string& unit)
	{
		for (const std::string& known : units())
			if (known == unit)
				return true;
		return false;
	}

	// Case-insensitive unit compare ("uf" == "F"), ASCII-only folding.
	bool UnitTable::unitsEqual(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (toLowerAscii(a[i]) != toLowerAscii(b[i]))
				return false;
		return true;
	}

	// Longest known unit symbol that `text` ends with, case-insensitive. 0 when there is none.
	size_t UnitTable::matchUnitSuffix(const std::string& text, std::string& outUnit)
	{
		size_t bestLength = 0;
		for (const std::string& unit : units())
		{
			if (unit.empty() || unit.size() > text.size() || unit.size() <= bestLength)
				continue;
			if (unitsEqual(text.substr(text.size() - unit.size()), unit))
			{
				bestLength = unit.size();
				outUnit = unit;
			}
		}
		return bestLength;
	}

	// SI prefix token starting at byte `pos`. Case-sensitive (see g_prefixes).
	bool UnitTable::prefixAt(const std::string& text, size_t pos, double& outMultiplier, size_t& outLength)
	{
		for (const PrefixEntry& entry : g_prefixes)
		{
			const size_t length = std::char_traits<char>::length(entry.token);
			if (text.compare(pos, length, entry.token) == 0)
			{
				outMultiplier = entry.multiplier;
				outLength = length;
				return true;
			}
		}
		return false;
	}

	// Canonical display prefix for an exponent. µ is spelled with the MICRO SIGN, matching the mockups;
	// the parser accepts 'u' and µ interchangeably so this still round-trips.
	std::string UnitTable::prefixForExponent(int exponent)
	{
		if (exponent == 0)
			return std::string();
		for (const PrefixEntry& entry : g_prefixes)
			if (entry.exponent == exponent)
				return entry.token;
		return std::string();
	}

}
