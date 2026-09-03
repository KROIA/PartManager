#include "mouser/PartManager_MouserSearchService.h"
#include "units/PartManager_UnitTable.h"
#include "units/PartManager_ValueParser.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace PartManager
{

	namespace
	{
		std::string toLower(const std::string& text)
		{
			std::string out = text;
			for (char& c : out)
			{
				if (c >= 'A' && c <= 'Z')
				{
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return out;
		}

		bool contains(const std::string& haystackLower, const char* needleLower)
		{
			return haystackLower.find(needleLower) != std::string::npos;
		}

		// One Mouser AttributeName we know how to place. `unit` is the §2a dropdown unit of the
		// matching part_type_attribute in seedDefaultTypes(), which is also the unit ValueParser
		// validates any typed suffix against.
		struct AttributeMapping
		{
			const char* mouserName;   // lowercase, matched exactly against the lowercased AttributeName
			const char* key;          // our part_type_attribute.key
			const char* unit;         // "" means the Ω symbol is filled in at lookup time
		};

		// Deliberately an exact-name allow-list rather than fuzzy matching: a wrong attribute is
		// worse than a missing one, and anything unmatched is reported for manual entry.
		// ponytail: covers the attributes the seeded templates actually declare. Grows one line
		// per new template attribute; revisit if this ever needs per-type disambiguation
		// (the same Mouser name meaning different things on two different templates).
		const AttributeMapping* attributeMappings(size_t& outCount)
		{
			static const AttributeMapping mappings[] = {
				{ "resistance",                     "resistance",      "\xCE\xA9" },
				{ "resistance in ohms",             "resistance",      "\xCE\xA9" },
				{ "capacitance",                    "capacitance",     "F"  },
				{ "inductance",                     "inductance",      "H"  },
				{ "tolerance",                      "tolerance",       "%"  },
				{ "capacitance tolerance",          "tolerance",       "%"  },
				{ "power rating",                   "power",           "W"  },
				{ "power dissipation",              "power",           "W"  },
				{ "power (watts)",                  "power",           "W"  },
				{ "voltage rating",                 "voltage",         "V"  },
				{ "voltage rating dc",              "voltage",         "V"  },
				{ "voltage rating (dc)",            "voltage",         "V"  },
				{ "dc voltage rating",              "voltage",         "V"  },
				{ "current rating",                 "current_rating",  "A"  },
				{ "current rating (amps)",          "current_rating",  "A"  },
				{ "rated current",                  "current_rating",  "A"  },
				{ "saturation current",             "current_rating",  "A"  },
				{ "output voltage",                 "output_voltage",  "V"  },
				{ "output current",                 "max_current",     "A"  },
				{ "maximum output current",         "max_current",     "A"  },
				{ "drain to source voltage (vdss)", "vds_max",         "V"  },
				{ "drain-source voltage",           "vds_max",         "V"  },
				{ "vds - max",                      "vds_max",         "V"  },
				{ "continuous drain current (id)",  "id_max",          "A"  },
				{ "drain current",                  "id_max",          "A"  },
				{ "current - continuous drain (id)","id_max",          "A"  },
			};
			outCount = sizeof(mappings) / sizeof(mappings[0]);
			return mappings;
		}

		// --- Description parsing ------------------------------------------------------------
		//
		// **The attribute map above can never fire on a real part.** Measured 2026-09-03 across
		// all 38 rows of the user's stock list: `ProductAttributes` carries `Packaging` three
		// times and `Standard Pack Qty`, and no parametrics whatsoever — not on
		// /search/partnumber and not on /search/keyword. Every value a template wants is in the
		// free-text `Description`, whose tail is the manufacturer's own parametric string:
		//
		//   "Multilayer Ceramic Capacitors MLCC - SMD/SMT 100nF+/-10% 25V X7R 0402"
		//   "Thin Film Resistors - SMD 1/10watts 10K .1%"
		//   "MOSFETs 60V 200mW"
		//
		// So the map that actually fills a form is not AttributeName -> key; it is **type -> the
		// units that type's slots are measured in**, and then one token at a time. A token is
		// placed only when exactly one slot of the type can read it, which is the whole safety
		// rule: "60V" lands on a MOSFET (only Vds is in volts) and is refused on a Diode (forward
		// *and* reverse voltage are), where a guess would be a plausible wrong number.
		//
		// Keyed by suggestedTypeName()'s answer rather than the raw Mouser category, so the
		// category -> template mapping stays in one place. Inherited slots are repeated rather
		// than resolved: three duplicated lines beat an inheritance walk that would have to reach
		// into persistence from here.
		struct DimensionSlot
		{
			const char* typeName;
			const char* key;
			const char* unit;
			bool primary;   // where a unit-less token like "10K" goes; at most one per type
		};

		const DimensionSlot* dimensionSlots(size_t& outCount)
		{
			static const char* const ohm = "\xCE\xA9";
			static const DimensionSlot slots[] = {
				{ "Resistor",           "resistance",       ohm,  true  },
				{ "Resistor",           "tolerance",        "%",  false },
				{ "Resistor",           "power",            "W",  false },
				{ "Capacitor",          "capacitance",      "F",  true  },
				{ "Capacitor",          "voltage",          "V",  false },
				{ "Capacitor",          "tolerance",        "%",  false },
				{ "Ceramic Capacitor",  "capacitance",      "F",  true  },
				{ "Ceramic Capacitor",  "voltage",          "V",  false },
				{ "Ceramic Capacitor",  "tolerance",        "%",  false },
				{ "Inductor",           "inductance",       "H",  true  },
				{ "Inductor",           "current_rating",   "A",  false },
				{ "Power Regulator",    "output_voltage",   "V",  false },
				{ "Power Regulator",    "max_current",      "A",  false },
				{ "MOSFET",             "vds_max",          "V",  false },
				{ "MOSFET",             "id_max",           "A",  false },
				// Diode and LED each declare two voltage slots, so no bare voltage token is
				// placeable on them at all — deliberately, see the rule above. The current slot
				// is the only one that can ever match, and only when its unit is spelled out.
				{ "Diode",              "forward_voltage",  "V",  false },
				{ "Diode",              "reverse_voltage",  "V",  false },
				{ "Diode",              "forward_current",  "A",  false },
				{ "LED",                "forward_voltage",  "V",  false },
				{ "LED",                "forward_current",  "A",  false },
			};
			outCount = sizeof(slots) / sizeof(slots[0]);
			return slots;
		}

		// The word-shaped half of the same idea: a token that *is* the value. Matched on the whole
		// lowercased token, never a substring — "red" inside "Waterclr"/"Shielded" is exactly the
		// kind of match that would colour a part wrong. `value` is spelled as the template's enum
		// option, because a value outside the option list shows up as an extra combo entry.
		struct EnumSlot
		{
			const char* typeName;
			const char* key;
			const char* token;   // lowercase, matched whole
			const char* value;
		};

		const EnumSlot* enumSlots(size_t& outCount)
		{
			static const EnumSlot slots[] = {
				{ "Ceramic Capacitor", "dielectric",     "c0g",       "C0G"        },
				{ "Ceramic Capacitor", "dielectric",     "np0",       "NP0"        },
				{ "Ceramic Capacitor", "dielectric",     "x5r",       "X5R"        },
				{ "Ceramic Capacitor", "dielectric",     "x6s",       "X6S"        },
				{ "Ceramic Capacitor", "dielectric",     "x7r",       "X7R"        },
				{ "Ceramic Capacitor", "dielectric",     "x7s",       "X7S"        },
				{ "Ceramic Capacitor", "dielectric",     "y5v",       "Y5V"        },
				{ "Ceramic Capacitor", "dielectric",     "z5u",       "Z5U"        },
				{ "MOSFET",            "channel_type",   "n-channel", "N-Channel"  },
				{ "MOSFET",            "channel_type",   "nch",       "N-Channel"  },
				{ "MOSFET",            "channel_type",   "n-ch",      "N-Channel"  },
				{ "MOSFET",            "channel_type",   "p-channel", "P-Channel"  },
				{ "MOSFET",            "channel_type",   "pch",       "P-Channel"  },
				{ "MOSFET",            "channel_type",   "p-ch",      "P-Channel"  },
				{ "Diode",             "diode_type",     "schottky",  "Schottky"   },
				{ "Diode",             "diode_type",     "switching", "Switching"  },
				{ "Diode",             "diode_type",     "rectifier", "Rectifier"  },
				{ "Diode",             "diode_type",     "zener",     "Zener"      },
				{ "Diode",             "diode_type",     "tvs",       "TVS"        },
				{ "LED",               "color",          "red",       "Red"        },
				{ "LED",               "color",          "green",     "Green"      },
				{ "LED",               "color",          "blue",      "Blue"       },
				{ "LED",               "color",          "yellow",    "Yellow"     },
				{ "LED",               "color",          "white",     "White"      },
				{ "LED",               "color",          "orange",    "Orange"     },
				{ "LED",               "color",          "infrared",  "Infrared"   },
				{ "LED",               "color",          "uv",        "UV"         },
				// "Buck"/"Boost"/"LDO" is what the description says; Linear/Switching is what the
				// template offers, so this row is a translation and not just a lookup.
				{ "Power Regulator",   "regulator_type", "buck",      "Switching"  },
				{ "Power Regulator",   "regulator_type", "boost",     "Switching"  },
				{ "Power Regulator",   "regulator_type", "switching", "Switching"  },
				{ "Power Regulator",   "regulator_type", "ldo",       "Linear"     },
				{ "Power Regulator",   "regulator_type", "linear",    "Linear"     },
			};
			outCount = sizeof(slots) / sizeof(slots[0]);
			return slots;
		}

		// An allow-list, not a shape rule. "1038" in "Power Inductors - SMD 5uH 30% SMD 1038" is
		// a Bourns case code and not a package at all, and it is four digits like every entry
		// here — so anything outside the standard imperial chip sizes is left alone.
		bool isPackageCode(const std::string& tokenUpper)
		{
			static const char* const codes[] = {
				"01005", "0201", "0402", "0603", "0805", "1206", "1210", "1218", "1812",
				"2010", "2512", "2917",
			};
			for (const char* code : codes)
			{
				if (tokenUpper == code)
				{
					return true;
				}
			}
			return false;
		}

		// Mouser spells units as words. Rewritten longest-first so "ohms" never leaves a stray "s"
		// and "amperes" is not eaten by "amp".
		struct WordUnit
		{
			const char* word;   // lowercase
			const char* symbol;
		};

		const WordUnit* wordUnits(size_t& outCount)
		{
			static const WordUnit words[] = {
				{ "ohms",     "\xCE\xA9" },
				{ "ohm",      "\xCE\xA9" },
				{ "farads",   "F"  },
				{ "farad",    "F"  },
				{ "henries",  "H"  },
				{ "henrys",   "H"  },
				{ "henry",    "H"  },
				{ "amperes",  "A"  },
				{ "ampere",   "A"  },
				{ "amps",     "A"  },
				{ "amp",      "A"  },
				{ "volts",    "V"  },
				{ "volt",     "V"  },
				{ "watts",    "W"  },
				{ "watt",     "W"  },
				{ "hertz",    "Hz" },
				{ "seconds",  "s"  },
				{ "second",   "s"  },
			};
			outCount = sizeof(words) / sizeof(words[0]);
			return words;
		}

		std::vector<std::string> splitWhitespace(const std::string& text)
		{
			std::vector<std::string> tokens;
			std::string current;
			for (char c : text)
			{
				if (c == ' ' || c == '\t')
				{
					if (!current.empty())
					{
						tokens.push_back(current);
						current.clear();
					}
				}
				else
				{
					current += c;
				}
			}
			if (!current.empty())
			{
				tokens.push_back(current);
			}
			return tokens;
		}

		// Mouser values are sometimes compound ("1 kOhms 1%"). Longest prefix that parses wins —
		// shortest-first would happily read "4.7 kOhms" as a bare 4.7.
		bool parseAttributeValue(const std::string& rawValue, const std::string& unit, double& outValue)
		{
			std::string cleaned = MouserSearchService::normalizeUnitWords(rawValue);
			std::vector<std::string> tokens = splitWhitespace(cleaned);
			if (tokens.empty())
			{
				return false;
			}
			const size_t maxTokens = tokens.size() < 4 ? tokens.size() : 4;
			for (size_t count = maxTokens; count > 0; --count)
			{
				std::string candidate;
				for (size_t i = 0; i < count; ++i)
				{
					candidate += tokens[i];
				}
				const ValueParseResult parsed = ValueParser::parse(candidate, unit);
				if (parsed.ok)
				{
					outValue = parsed.value;
					return true;
				}
			}
			return false;
		}

		// Defined below with the other JSON helpers; used by the description parser that follows.
		std::string jsonEscape(const std::string& text);
		std::string formatNumber(double value);

		struct ParsedAttribute
		{
			std::string key;
			std::string json;   // the serialized value: {"value":..,"unit":".."} or a quoted string
		};

		bool hasKey(const std::vector<ParsedAttribute>& parsed, const std::string& key)
		{
			for (const ParsedAttribute& entry : parsed)
			{
				if (entry.key == key)
				{
					return true;
				}
			}
			return false;
		}

		// "1/10W" -> "0.1W". Resistor power is spelled as a fraction more often than not
		// ("1/10watts"), and there is no reason for ValueParser to know about fractions.
		// Anything that is not <digits>/<digits><rest> comes back unchanged.
		std::string expandFraction(const std::string& token)
		{
			const size_t slash = token.find('/');
			if (slash == std::string::npos || slash == 0)
			{
				return token;
			}
			size_t end = slash + 1;
			while (end < token.size() && token[end] >= '0' && token[end] <= '9')
			{
				++end;
			}
			if (end == slash + 1)
			{
				return token;
			}
			for (size_t i = 0; i < slash; ++i)
			{
				if (token[i] < '0' || token[i] > '9')
				{
					return token;
				}
			}
			const double numerator = std::atof(token.substr(0, slash).c_str());
			const double denominator = std::atof(token.substr(slash + 1, end - slash - 1).c_str());
			if (denominator == 0.0)
			{
				return token;
			}
			return formatNumber(numerator / denominator) + token.substr(end);
		}

		// "10K" -> "10k", "4K7" -> "4k7". SI says kilo is lowercase and ValueParser is rightly
		// case-sensitive about it ('M' and 'm' differ by a factor of a billion), but Mouser's prose
		// writes resistances with a capital K and would otherwise read as no value at all. Only a
		// token with no lowercase letter in it is folded, so "500kHz" and "mW" are left untouched.
		std::string foldCapitalKilo(const std::string& token)
		{
			for (char c : token)
			{
				if (c >= 'a' && c <= 'z')
				{
					return token;
				}
			}
			std::string out = token;
			for (char& c : out)
			{
				if (c == 'K')
				{
					c = 'k';
				}
			}
			return out;
		}

		// A number carrying an SI prefix but no unit — "10K", "4u7". This is the only shape allowed
		// to land on a type's primary slot when several slots could read it, because it is the only
		// one where the manufacturer left the unit out as understood ("10K" on a resistor is ohms).
		// A plain integer is excluded on purpose: "3" and "1206" are both readable as a value and
		// are both noise.
		bool bareprefixedNumber(const std::string& token)
		{
			bool sawDigit = false;
			bool sawPrefix = false;
			for (char c : token)
			{
				if (c >= '0' && c <= '9')
				{
					sawDigit = true;
				}
				else if (c == '.')
				{
					continue;
				}
				else if (std::string("kKmMuUnpPgGtT").find(c) != std::string::npos)
				{
					sawPrefix = true;
				}
				else
				{
					return false;
				}
			}
			return sawDigit && sawPrefix;
		}

		// The description minus its category prefix, split into candidate values. Mouser repeats
		// the category verbatim at the front of every description, so dropping it is what leaves
		// the manufacturer's parametric tail — and stops "Capacitors" being read as a value.
		std::vector<std::string> descriptionTokens(const std::string& category,
			const std::string& description)
		{
			std::string tail = description;
			if (!category.empty() && toLower(tail).rfind(toLower(category), 0) == 0)
			{
				tail = tail.substr(category.size());
			}

			std::string cleaned;
			for (size_t i = 0; i < tail.size(); ++i)
			{
				// "100nF+/-10%" is two values written as one token, and the only place a '+' or a
				// bare '-' appears between them. Split there; leave '/' alone, fractions need it.
				if (tail.compare(i, 3, "+/-") == 0)
				{
					cleaned += ' ';
					i += 2;
					continue;
				}
				const char c = tail[i];
				cleaned += (c == ',' || c == '(' || c == ')' || c == ';' || c == '+') ? ' ' : c;
			}

			std::vector<std::string> tokens;
			for (std::string token : splitWhitespace(cleaned))
			{
				while (!token.empty() && (token.back() == '.' || token.back() == '-'))
				{
					token.pop_back();
				}
				if (!token.empty())
				{
					tokens.push_back(token);
				}
			}
			return tokens;
		}

		// One token at a time into whatever slot of `typeName` can uniquely read it. Slots already
		// filled — by a real ProductAttribute or by an earlier token — are never overwritten, so
		// the first spelling of a quantity wins and the API's own data always beats the prose.
		void fillFromDescription(const std::string& typeName, const std::string& category,
			const std::string& description, std::vector<ParsedAttribute>& parsed,
			std::string& outPackage)
		{
			size_t dimensionCount = 0;
			const DimensionSlot* dimensions = dimensionSlots(dimensionCount);
			size_t enumCount = 0;
			const EnumSlot* enums = enumSlots(enumCount);

			for (const std::string& raw : descriptionTokens(category, description))
			{
				std::string upper = raw;
				for (char& c : upper)
				{
					if (c >= 'a' && c <= 'z')
					{
						c = static_cast<char>(c - 'a' + 'A');
					}
				}
				if (outPackage.empty() && isPackageCode(upper))
				{
					outPackage = upper;
					continue;
				}
				if (typeName.empty())
				{
					continue;
				}

				const std::string lower = toLower(raw);
				bool matchedEnum = false;
				for (size_t i = 0; i < enumCount; ++i)
				{
					if (typeName == enums[i].typeName && lower == enums[i].token)
					{
						if (!hasKey(parsed, enums[i].key))
						{
							parsed.push_back({ enums[i].key,
								"\"" + jsonEscape(enums[i].value) + "\"" });
						}
						matchedEnum = true;
						break;
					}
				}
				if (matchedEnum)
				{
					continue;
				}

				const std::string token =
					foldCapitalKilo(expandFraction(MouserSearchService::normalizeUnitWords(raw)));
				const DimensionSlot* hit = nullptr;
				const DimensionSlot* primary = nullptr;
				double hitValue = 0.0;
				double primaryValue = 0.0;
				int matches = 0;
				for (size_t i = 0; i < dimensionCount; ++i)
				{
					const DimensionSlot& slot = dimensions[i];
					if (typeName != slot.typeName || hasKey(parsed, slot.key))
					{
						continue;
					}
					const ValueParseResult result = ValueParser::parse(token, slot.unit);
					if (!result.ok)
					{
						continue;
					}
					++matches;
					hit = &slot;
					hitValue = result.value;
					if (slot.primary)
					{
						primary = &slot;
						primaryValue = result.value;
					}
				}

				// Exactly one slot can read it, or it is a unit-less number and the type says
				// where those belong. Two slots and no such rule is where a wrong value would
				// come from, so nothing is written.
				if (matches > 1)
				{
					if (primary == nullptr || !bareprefixedNumber(token))
					{
						continue;
					}
					hit = primary;
					hitValue = primaryValue;
				}
				else if (matches == 0)
				{
					continue;
				}
				parsed.push_back({ hit->key, "{\"value\":" + formatNumber(hitValue)
					+ ",\"unit\":\"" + jsonEscape(hit->unit) + "\"}" });
			}
		}

		// Lower rank = closer match. `extra` breaks ties by how much the candidate carries
		// beyond the query, which is exactly what separates 595-LM358DR from 595-LM358DRE4.
		struct MatchScore
		{
			int rank = 3;        // 0 exact, 1 prefix, 2 substring, 3 no match
			size_t extra = 0;

			bool operator<(const MatchScore& other) const
			{
				return rank != other.rank ? rank < other.rank : extra < other.extra;
			}
		};

		MatchScore scoreCandidate(const std::string& candidate, const std::string& queryLower)
		{
			MatchScore score;
			if (candidate.empty() || queryLower.empty())
			{
				return score;
			}
			const std::string lower = toLower(candidate);
			if (lower == queryLower)
			{
				score.rank = 0;
			}
			else if (lower.compare(0, queryLower.size(), queryLower) == 0)
			{
				score.rank = 1;
			}
			else if (lower.find(queryLower) != std::string::npos)
			{
				score.rank = 2;
			}
			else
			{
				return score;
			}
			score.extra = lower.size() - queryLower.size();
			return score;
		}

		std::string jsonEscape(const std::string& text)
		{
			std::string out;
			out.reserve(text.size() + 4);
			for (char c : text)
			{
				if (c == '"' || c == '\\')
				{
					out += '\\';
				}
				out += c;
			}
			return out;
		}

		// %.10g keeps 4700 as "4700" and 1e-7 as "1e-07"; both are valid JSON numbers and both
		// round-trip through the same parser the editor uses.
		std::string formatNumber(double value)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.10g", value);
			return std::string(buffer);
		}
	}

	std::string MouserSearchService::normalizeUnitWords(const std::string& value)
	{
		// Drop the tolerance sign Mouser prefixes onto ratio values ("\xC2\xB1 1%").
		std::string text = value;
		size_t start = 0;
		while (start < text.size())
		{
			if (text.compare(start, 2, "\xC2\xB1") == 0)
			{
				start += 2;
			}
			else if (text.compare(start, 3, "+/-") == 0)
			{
				start += 3;
			}
			else if (text[start] == ' ' || text[start] == '\t')
			{
				start += 1;
			}
			else
			{
				break;
			}
		}
		text = text.substr(start);

		size_t wordCount = 0;
		const WordUnit* words = wordUnits(wordCount);
		const std::string lower = toLower(text);
		std::string out;
		size_t pos = 0;
		while (pos < text.size())
		{
			bool replaced = false;
			for (size_t i = 0; i < wordCount; ++i)
			{
				const std::string word = words[i].word;
				if (lower.compare(pos, word.size(), word) == 0)
				{
					out += words[i].symbol;
					pos += word.size();
					replaced = true;
					break;
				}
			}
			if (!replaced)
			{
				out += text[pos];
				pos += 1;
			}
		}
		return out;
	}

	void MouserSearchService::rankByMatch(std::vector<MouserPartDto>& parts, const std::string& query)
	{
		const std::string queryLower = toLower(query);
		if (queryLower.empty() || parts.size() < 2)
		{
			return;
		}
		std::stable_sort(parts.begin(), parts.end(),
			[&queryLower](const MouserPartDto& left, const MouserPartDto& right)
			{
				const MatchScore leftScore = std::min(scoreCandidate(left.mouserPartNumber, queryLower),
					scoreCandidate(left.manufacturerPartNumber, queryLower));
				const MatchScore rightScore = std::min(scoreCandidate(right.mouserPartNumber, queryLower),
					scoreCandidate(right.manufacturerPartNumber, queryLower));
				return leftScore < rightScore;
			});
	}

	std::string MouserSearchService::suggestedTypeName(const std::string& category)
	{
		const std::string lower = toLower(category);
		if (lower.empty())
		{
			return std::string();
		}
		// Order matters: the narrower template has to win over the one it inherits from.
		if (contains(lower, "mosfet"))
		{
			return "MOSFET";
		}
		if (contains(lower, "resistor"))
		{
			return "Resistor";
		}
		if (contains(lower, "capacitor"))
		{
			return contains(lower, "ceramic") ? "Ceramic Capacitor" : "Capacitor";
		}
		if (contains(lower, "inductor"))
		{
			return "Inductor";
		}
		if (contains(lower, "regulator"))
		{
			return "Power Regulator";
		}
		if (contains(lower, "transistor"))
		{
			return "Transistor";
		}
		// "leds", never "led": "Shielded"/"Coupled"/"Sealed" all contain the three letters.
		if (contains(lower, "leds"))
		{
			return "LED";
		}
		if (contains(lower, "diode"))
		{
			return "Diode";
		}
		if (contains(lower, "sensor"))
		{
			return "Sensor";
		}
		// Everything else — connectors, crystals, MCUs — has no template yet.
		// Returning "" is the point: a wrong template is worse than none (§6).
		return std::string();
	}

	std::string MouserSearchService::attributesJson(const std::vector<MouserProductAttribute>& attributes,
		std::vector<std::string>& outUnmapped)
	{
		size_t mappingCount = 0;
		const AttributeMapping* mappings = attributeMappings(mappingCount);

		std::string json = "{";
		bool first = true;
		for (const MouserProductAttribute& attribute : attributes)
		{
			const std::string nameLower = toLower(attribute.name);
			const AttributeMapping* match = nullptr;
			for (size_t i = 0; i < mappingCount; ++i)
			{
				if (nameLower == mappings[i].mouserName)
				{
					match = &mappings[i];
					break;
				}
			}
			if (match == nullptr)
			{
				outUnmapped.push_back(attribute.name);
				continue;
			}

			double value = 0.0;
			if (!parseAttributeValue(attribute.value, match->unit, value))
			{
				// Known field, unreadable value — still the user's job, not a guess.
				outUnmapped.push_back(attribute.name);
				continue;
			}

			if (!first)
			{
				json += ",";
			}
			first = false;
			json += "\"" + jsonEscape(match->key) + "\":{\"value\":" + formatNumber(value)
				+ ",\"unit\":\"" + jsonEscape(match->unit) + "\"}";
		}
		json += "}";
		return json;
	}

	std::string MouserSearchService::attributesFromDescription(const std::string& typeName,
		const std::string& category, const std::string& description, std::string* outPackage)
	{
		std::vector<ParsedAttribute> parsed;
		std::string package;
		fillFromDescription(typeName, category, description, parsed, package);
		if (outPackage != nullptr)
		{
			*outPackage = package;
		}

		std::string json = "{";
		for (size_t i = 0; i < parsed.size(); ++i)
		{
			json += (i == 0 ? "" : ",") + ("\"" + jsonEscape(parsed[i].key) + "\":") + parsed[i].json;
		}
		return json + "}";
	}

	std::string MouserSearchService::partNumberFromUrl(const std::string& url)
	{
		const std::string lowered = toLower(url);
		// Any Mouser country domain, but it has to be Mouser's — a ProductDetail path on some
		// other site would not carry a Mouser part number.
		if (!contains(lowered, "mouser."))
		{
			return std::string();
		}

		static const char* const marker = "/productdetail/";
		const size_t markerPos = lowered.find(marker);
		if (markerPos == std::string::npos)
		{
			return std::string();
		}

		std::string path = url.substr(markerPos + std::string(marker).size());
		path = path.substr(0, path.find_first_of("?#"));

		// `<Manufacturer>/<PartNumber>`, but Mouser occasionally emits just `<PartNumber>` —
		// the last non-empty segment is the part number in both shapes.
		while (!path.empty() && path.back() == '/')
		{
			path.pop_back();
		}
		const size_t lastSlash = path.find_last_of('/');
		return lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
	}

	std::string MouserSearchService::datasheetUrlFor(const std::string& manufacturer,
		const std::string& mpn)
	{
		if (mpn.empty())
		{
			return std::string();
		}
		// An MPN with a slash or a space in it would build a URL pointing somewhere else entirely.
		if (mpn.find_first_of("/\\ ?#&") != std::string::npos)
		{
			return std::string();
		}

		const std::string maker = toLower(manufacturer);
		// Confirmed against 150120YS75000, 74437346220 and 885012207072 — all three answered with
		// a real PDF. Würth is the manufacturer the user's own stock list is fullest of, which is
		// the only reason a one-entry table earns its keep.
		// ponytail: one entry, grown by hand. Ceiling: covers exactly the manufacturers whose URL
		// is a pure function of the MPN; anything else still needs the API's DataSheetUrl or a
		// file attached by hand. Add a row only after fetching the URL and seeing a PDF come back.
		if (contains(maker, "wurth") || contains(maker, "würth"))
		{
			return "https://www.we-online.com/catalog/datasheet/" + mpn + ".pdf";
		}
		return std::string();
	}

	std::string MouserSearchService::previewImageUrl(const std::string& imagePath)
	{
		// Only the exact `/images/<vendor>/<variant>/<file>` shape is rewritten. Splitting on the
		// segment count rather than searching for "sm" or "images" by name is what keeps a vendor
		// called "images" or a file called "sm.jpg" from being mistaken for the size segment.
		const size_t schemeEnd = imagePath.find("://");
		const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
		const size_t pathStart = imagePath.find('/', hostStart);
		if (pathStart == std::string::npos)
		{
			return imagePath;
		}

		std::vector<size_t> slashes;
		for (size_t i = pathStart; i < imagePath.size(); ++i)
		{
			if (imagePath[i] == '/')
			{
				slashes.push_back(i);
			}
		}
		// Four segments means four slashes and no trailing one: /images /<vendor> /<variant> /<file>.
		if (slashes.size() != 4 || slashes.back() == imagePath.size() - 1)
		{
			return imagePath;
		}
		if (toLower(imagePath.substr(slashes[0], slashes[1] - slashes[0])) != "/images")
		{
			return imagePath;
		}

		return imagePath.substr(0, slashes[2]) + "/lrg" + imagePath.substr(slashes[3]);
	}

	MouserPartPrefill MouserSearchService::toPrefill(const MouserPartDto& dto)
	{
		MouserPartPrefill prefill;
		prefill.mouserPartNumber = dto.mouserPartNumber;
		// Mouser leaves DataSheetUrl empty for most real parts (§6) — including the Würth LED the
		// user brought this up with — so the fallback runs whenever it is missing, never over it.
		prefill.datasheetUrl = dto.dataSheetUrl.empty()
			? datasheetUrlFor(dto.manufacturer, dto.manufacturerPartNumber)
			: dto.dataSheetUrl;
		prefill.imageUrl = previewImageUrl(dto.imagePath);
		prefill.productDetailUrl = dto.productDetailUrl;
		prefill.suggestedTypeName = suggestedTypeName(dto.category);

		// partTypeId stays 0 on purpose — resolving a name to a row is persistence's job and
		// core/mouser has no database handle. The caller looks up suggestedTypeName if it wants one.
		prefill.part.name = dto.manufacturerPartNumber.empty() ? dto.description : dto.manufacturerPartNumber;
		prefill.part.manufacturer = dto.manufacturer;
		prefill.part.mpn = dto.manufacturerPartNumber;
		prefill.part.description = dto.description;

		// Mouser has no dedicated package field — it lives in the parametric table under one of
		// a handful of names.
		static const char* const packageNames[] = { "package / case", "package/case", "package", "case code (imperial)" };
		for (const char* wanted : packageNames)
		{
			for (const MouserProductAttribute& attribute : dto.productAttributes)
			{
				if (toLower(attribute.name) == wanted && !attribute.value.empty())
				{
					prefill.part.package = attribute.value;
					break;
				}
			}
			if (!prefill.part.package.empty())
			{
				break;
			}
		}

		prefill.part.attributes = attributesJson(dto.productAttributes, prefill.unmappedAttributes);

		// Then the description fills whatever the (usually empty) ProductAttributes left open.
		// Merged rather than replaced, and only for keys the API did not already answer: a real
		// attribute is a stated fact, a parsed one is a reading of prose.
		std::string describedPackage;
		std::vector<ParsedAttribute> described;
		fillFromDescription(prefill.suggestedTypeName, dto.category, dto.description, described,
			describedPackage);
		for (const ParsedAttribute& entry : described)
		{
			if (prefill.part.attributes.find("\"" + entry.key + "\":") != std::string::npos)
			{
				continue;
			}
			const std::string addition = "\"" + jsonEscape(entry.key) + "\":" + entry.json;
			prefill.part.attributes.insert(prefill.part.attributes.size() - 1,
				prefill.part.attributes.size() > 2 ? "," + addition : addition);
		}
		if (prefill.part.package.empty())
		{
			prefill.part.package = describedPackage;
		}

		prefill.priceBreaks = toPriceObservations(dto.priceBreaks);
		return prefill;
	}

	std::vector<PriceObservation> MouserSearchService::toPriceObservations(
		const std::vector<MouserPriceBreak>& breaks)
	{
		std::vector<PriceObservation> observations;
		for (const MouserPriceBreak& priceBreak : breaks)
		{
			if (priceBreak.quantity <= 0)
			{
				continue;
			}

			// "0.21 CHF" / "CHF 0.21" / "$0.21" / "1'234.50 CHF" — take the first run of digits,
			// separators and sign, and read the currency off whatever letters are left. Anything
			// that yields no number at all is skipped: a price of 0 in the history would read as
			// "it was free that day", which is worse than a gap.
			std::string number;
			std::string letters;
			bool numberDone = false;
			for (char c : priceBreak.price)
			{
				if ((c >= '0' && c <= '9') || c == '.' || c == '-')
				{
					if (!numberDone)
					{
						number += c;
					}
				}
				else if (c == ',' || c == '\'')
				{
					// A thousands separator inside the digits; a decimal comma in a locale that
					// uses one. Either way it is not a letter and must not end the number.
					if (!numberDone && !number.empty())
					{
						number += '.';
					}
				}
				else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
				{
					if (!number.empty())
					{
						numberDone = true;
					}
					letters += c;
				}
				else if (!number.empty())
				{
					numberDone = true;
				}
			}
			// A decimal comma leaves "0.21" alone but turns "1'234.50" into "1.234.50"; atof stops
			// at the second dot, so drop every separator but the last.
			const size_t lastDot = number.rfind('.');
			if (lastDot != std::string::npos)
			{
				std::string cleaned;
				for (size_t i = 0; i < number.size(); ++i)
				{
					if (number[i] != '.' || i == lastDot)
					{
						cleaned += number[i];
					}
				}
				number = cleaned;
			}
			if (number.empty())
			{
				continue;
			}

			PriceObservation observation;
			observation.quantityBreak = priceBreak.quantity;
			observation.unitPrice = std::atof(number.c_str());
			// The currency embedded in Price wins; the separate field is only the fallback,
			// because concatenating both prints it twice (found live, not in a fixture).
			observation.currency = letters.empty() ? priceBreak.currency : letters;
			observation.source = PriceSource::MouserApiQuote;
			observations.push_back(observation);
		}
		return observations;
	}

}
