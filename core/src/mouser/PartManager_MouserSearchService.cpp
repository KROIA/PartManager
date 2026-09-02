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
