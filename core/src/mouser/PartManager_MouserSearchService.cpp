#include "mouser/PartManager_MouserSearchService.h"
#include "units/PartManager_UnitTable.h"
#include "units/PartManager_ValueParser.h"

#include <cstdio>

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
		// Everything else — diodes, LEDs, hall sensors, connectors — has no template yet.
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

	MouserPartPrefill MouserSearchService::toPrefill(const MouserPartDto& dto)
	{
		MouserPartPrefill prefill;
		prefill.mouserPartNumber = dto.mouserPartNumber;
		prefill.datasheetUrl = dto.dataSheetUrl;
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
		return prefill;
	}

}
