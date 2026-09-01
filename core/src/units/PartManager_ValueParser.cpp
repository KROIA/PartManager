#include "units/PartManager_ValueParser.h"
#include "units/PartManager_UnitTable.h"
#include <charconv>
#include <cmath>
#include <cstdio>

namespace PartManager
{

	namespace
	{
		// Exact powers of ten for the eight §2a prefixes plus 1e0, so scaling for display never
		// goes through std::pow and never introduces a rounding step of its own.
		double decadeMultiplier(int exponent)
		{
			switch (exponent)
			{
			case -12: return 1e-12;
			case  -9: return 1e-9;
			case  -6: return 1e-6;
			case  -3: return 1e-3;
			case   3: return 1e3;
			case   6: return 1e6;
			case   9: return 1e9;
			default:  return 1.0;
			}
		}

		bool isDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		// §2a input normalization, applied before anything is interpreted:
		// whitespace dropped, "'" thousands grouping dropped, "," folded to the canonical ".".
		// ponytail: grouping positions are not validated — "1'2'3'4" reads as 1234 rather than an
		// error. Rejecting misplaced separators needs the grouping rules per locale for no gain
		// here; tighten it if the entry field ever needs to red-flag the input as you type.
		std::string normalize(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (char c : text)
			{
				if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'')
					continue;
				out.push_back(c == ',' ? '.' : c);
			}
			return out;
		}

		bool toDouble(const std::string& digits, double& outValue)
		{
			if (digits.empty())
				return false;
			const char* begin = digits.data();
			const char* end = begin + digits.size();
			std::from_chars_result result = std::from_chars(begin, end, outValue, std::chars_format::fixed);
			return result.ec == std::errc() && result.ptr == end;
		}

		// Parses the numeric body (everything left after the unit suffix was removed) into a
		// mantissa and an SI multiplier. Accepts all three §2a forms: "100", "4.7k", "4k7".
		bool parseBody(const std::string& body, double& outMantissa, double& outMultiplier, std::string& outPrefix)
		{
			size_t pos = 0;
			bool negative = false;
			if (pos < body.size() && (body[pos] == '+' || body[pos] == '-'))
			{
				negative = (body[pos] == '-');
				++pos;
			}

			std::string before;   // digits ahead of the prefix letter
			std::string after;    // digits behind it — the "4k7" shorthand's fractional part
			bool dotSeen = false;
			bool prefixSeen = false;
			double multiplier = 1.0;
			std::string prefix;

			while (pos < body.size())
			{
				const char c = body[pos];
				if (isDigit(c))
				{
					(prefixSeen ? after : before).push_back(c);
					++pos;
					continue;
				}
				if (c == '.')
				{
					// A prefix letter already stands in for the decimal point, so a second one is nonsense.
					if (dotSeen || prefixSeen)
						return false;
					dotSeen = true;
					before.push_back('.');
					++pos;
					continue;
				}

				double prefixMultiplier = 1.0;
				size_t prefixLength = 0;
				if (!prefixSeen && UnitTable::prefixAt(body, pos, prefixMultiplier, prefixLength))
				{
					prefixSeen = true;
					multiplier = prefixMultiplier;
					prefix = body.substr(pos, prefixLength);
					pos += prefixLength;
					continue;
				}
				return false;   // stray character, or a second prefix as in "1k2k"
			}

			std::string number = before;
			if (!after.empty())
			{
				// "4k7" — the prefix replaced the decimal point, so there must be a whole part
				// in front of it and no explicit point anywhere.
				if (before.empty() || dotSeen)
					return false;
				number += ".";
				number += after;
			}

			double magnitude = 0.0;
			if (!toDouble(number, magnitude))
				return false;

			outMantissa = negative ? -magnitude : magnitude;
			outMultiplier = multiplier;
			outPrefix = prefix;
			return true;
		}

		ValueParseResult parseImpl(const std::string& text, const std::string& fieldUnit, bool checkFieldUnit)
		{
			ValueParseResult result;

			const std::string normalized = normalize(text);
			if (normalized.empty())
				return result;

			std::string explicitUnit;
			std::string body = normalized;
			const size_t unitLength = UnitTable::matchUnitSuffix(normalized, explicitUnit);
			// A suffix that swallows the whole input ("Ω") leaves no number, so it is not a suffix.
			if (unitLength > 0 && unitLength < normalized.size())
			{
				body = normalized.substr(0, normalized.size() - unitLength);
				result.hasExplicitUnit = true;
			}
			else
			{
				explicitUnit.clear();
			}

			double mantissa = 0.0;
			double multiplier = 1.0;
			std::string prefix;
			if (!parseBody(body, mantissa, multiplier, prefix))
				return ValueParseResult();

			if (checkFieldUnit)
			{
				if (result.hasExplicitUnit && !UnitTable::unitsEqual(explicitUnit, fieldUnit))
					return ValueParseResult();
				result.unit = fieldUnit;
			}
			else
			{
				result.unit = explicitUnit;
			}

			result.ok = true;
			result.mantissa = mantissa;
			result.value = mantissa * multiplier;
			result.prefix = prefix;
			result.hasPrefix = !prefix.empty();
			return result;
		}
	}

	// Parses a value typed into a field whose §2a dropdown unit is `fieldUnit`.
	ValueParseResult ValueParser::parse(const std::string& text, const std::string& fieldUnit)
	{
		return parseImpl(text, fieldUnit, true);
	}

	// Parses a value with no declared unit to check against (search bar).
	ValueParseResult ValueParser::parse(const std::string& text)
	{
		return parseImpl(text, std::string(), false);
	}

	// Base-SI value -> shortest human-readable prefixed form, e.g. 4700 -> "4.7 kΩ".
	// ponytail: prefixes only, never scientific notation — a value beyond 1e12 or below 1e-15 just
	// prints with a saturated p/G prefix and a long mantissa. Nothing in an electronics parts list
	// goes there; add an exponent fallback if a future domain (§2b screws, woodsheets) does.
	std::string ValueParser::format(double value, const std::string& unit)
	{
		if (!std::isfinite(value))
			return std::string();

		int exponent = 0;
		if (value != 0.0)
		{
			const double decades = std::log10(std::fabs(value)) / 3.0;
			exponent = static_cast<int>(std::floor(decades)) * 3;
			if (exponent < -12) exponent = -12;
			if (exponent > 9)   exponent = 9;
		}

		const double scaled = value / decadeMultiplier(exponent);

		// 10 significant digits: enough that parse(format(v)) lands back on v, short enough that
		// 4.7e-6 / 1e-6 prints as "4.7" instead of its binary tail.
		char buffer[64] = { 0 };
		std::snprintf(buffer, sizeof(buffer), "%.10g", scaled);

		std::string out = buffer;
		const std::string prefix = UnitTable::prefixForExponent(exponent);
		if (unit.empty())
			return out + prefix;
		return out + " " + prefix + unit;
	}

}
