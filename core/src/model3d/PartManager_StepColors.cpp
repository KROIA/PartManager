#include "model3d/PartManager_StepColors.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace PartManager
{
	namespace
	{
		// One `#id = TYPE(args);` record. Arguments are kept as text and re-scanned on demand:
		// the only things ever wanted out of them are references and numbers, and parsing STEP's
		// full argument grammar to get those would be a great deal of code for no more answers.
		struct Entity
		{
			std::string type;
			std::string args;
		};

		// A walk down from one solid touches every point in it and nothing above it, so the
		// recursion is bounded by the model. This cap is only for a file whose references form a
		// cycle, which is malformed but must not hang the app.
		constexpr size_t MaxVisited = 400000;

		bool isNameChar(char c)
		{
			return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
				|| c == '_';
		}

		// Splits the DATA section into entities. Handles the two things that break a naive
		// line-based scan: records wrapped across lines, and `/* ... */` comments — plus
		// apostrophes inside STEP strings, which are doubled rather than backslash-escaped.
		std::map<long, Entity> readEntities(const std::string& text)
		{
			std::map<long, Entity> entities;

			size_t cursor = text.find("DATA;");
			cursor = (cursor == std::string::npos) ? 0 : cursor + 5;

			while (cursor < text.size())
			{
				// Find the next "#<digits> =".
				const size_t hash = text.find('#', cursor);
				if (hash == std::string::npos) { break; }
				size_t index = hash + 1;
				long id = 0;
				bool anyDigit = false;
				while (index < text.size() && text[index] >= '0' && text[index] <= '9')
				{
					id = id * 10 + (text[index] - '0');
					anyDigit = true;
					++index;
				}
				while (index < text.size() && (text[index] == ' ' || text[index] == '\t'
					|| text[index] == '\r' || text[index] == '\n'))
				{
					++index;
				}
				if (!anyDigit || index >= text.size() || text[index] != '=')
				{
					cursor = hash + 1;
					continue;   // a reference inside another record, not a definition
				}
				++index;

				// The type name.
				while (index < text.size() && (text[index] == ' ' || text[index] == '\t'
					|| text[index] == '\r' || text[index] == '\n'))
				{
					++index;
				}
				const size_t typeStart = index;
				while (index < text.size() && isNameChar(text[index])) { ++index; }
				if (index == typeStart)
				{
					cursor = hash + 1;
					continue;   // a complex "#7=(A()B());" instance, which carries no colour
				}
				Entity entity;
				entity.type = text.substr(typeStart, index - typeStart);

				// The argument list, to the matching close paren.
				while (index < text.size() && text[index] != '(' && text[index] != ';') { ++index; }
				if (index >= text.size() || text[index] != '(')
				{
					cursor = index;
					continue;
				}
				const size_t argsStart = ++index;
				int depth = 1;
				bool inString = false;
				while (index < text.size() && depth > 0)
				{
					const char c = text[index];
					if (inString)
					{
						// '' inside a string is a literal apostrophe, not the end of it.
						if (c == '\'')
						{
							if (index + 1 < text.size() && text[index + 1] == '\'') { ++index; }
							else { inString = false; }
						}
					}
					else if (c == '\'') { inString = true; }
					else if (c == '(') { ++depth; }
					else if (c == ')') { --depth; }
					++index;
				}
				entity.args = text.substr(argsStart,
					index > argsStart ? index - argsStart - 1 : 0);
				entities[id] = std::move(entity);
				cursor = index;
			}
			return entities;
		}

		// Every `#n` in an argument list, in order. Outside strings: a STEP string may legally
		// contain a '#', and counting that as a reference sends the walk somewhere arbitrary.
		std::vector<long> referencesIn(const std::string& args)
		{
			std::vector<long> refs;
			bool inString = false;
			for (size_t i = 0; i < args.size(); ++i)
			{
				const char c = args[i];
				if (inString)
				{
					if (c == '\'')
					{
						if (i + 1 < args.size() && args[i + 1] == '\'') { ++i; }
						else { inString = false; }
					}
					continue;
				}
				if (c == '\'') { inString = true; continue; }
				if (c != '#') { continue; }
				long id = 0;
				bool any = false;
				while (i + 1 < args.size() && args[i + 1] >= '0' && args[i + 1] <= '9')
				{
					id = id * 10 + (args[++i] - '0');
					any = true;
				}
				if (any) { refs.push_back(id); }
			}
			return refs;
		}

		// The numbers in an argument list, skipping anything inside a string — a
		// CARTESIAN_POINT's own name is a string and often contains digits.
		std::vector<double> numbersIn(const std::string& args)
		{
			std::vector<double> numbers;
			bool inString = false;
			for (size_t i = 0; i < args.size(); ++i)
			{
				const char c = args[i];
				if (inString)
				{
					if (c == '\'')
					{
						if (i + 1 < args.size() && args[i + 1] == '\'') { ++i; }
						else { inString = false; }
					}
					continue;
				}
				if (c == '\'') { inString = true; continue; }
				if (c == '#')
				{
					while (i + 1 < args.size() && args[i + 1] >= '0' && args[i + 1] <= '9') { ++i; }
					continue;
				}
				const bool startsNumber = (c >= '0' && c <= '9')
					|| ((c == '-' || c == '+' || c == '.') && i + 1 < args.size()
						&& ((args[i + 1] >= '0' && args[i + 1] <= '9') || args[i + 1] == '.'));
				if (!startsNumber) { continue; }
				char* end = nullptr;
				const double value = std::strtod(args.c_str() + i, &end);
				if (end != nullptr && end != args.c_str() + i)
				{
					numbers.push_back(value);
					i = static_cast<size_t>(end - args.c_str()) - 1;
				}
			}
			return numbers;
		}

		// The handful of named colours real files use instead of an RGB triple.
		bool namedColor(const std::string& args, StepColor& out)
		{
			const size_t open = args.find('\'');
			if (open == std::string::npos) { return false; }
			const size_t close = args.find('\'', open + 1);
			if (close == std::string::npos) { return false; }
			std::string name = args.substr(open + 1, close - open - 1);
			for (char& c : name) { c = static_cast<char>(::tolower(c)); }

			if (name == "black")   { out = { 0.0, 0.0, 0.0 }; return true; }
			if (name == "white")   { out = { 1.0, 1.0, 1.0 }; return true; }
			if (name == "red")     { out = { 1.0, 0.0, 0.0 }; return true; }
			if (name == "green")   { out = { 0.0, 1.0, 0.0 }; return true; }
			if (name == "blue")    { out = { 0.0, 0.0, 1.0 }; return true; }
			if (name == "yellow")  { out = { 1.0, 1.0, 0.0 }; return true; }
			if (name == "magenta") { out = { 1.0, 0.0, 1.0 }; return true; }
			if (name == "cyan")    { out = { 0.0, 1.0, 1.0 }; return true; }
			return false;
		}

		// Follows a style reference down to whatever colour is at the bottom of it. The chain is
		// long and its shape varies between exporters, so this searches rather than walking a
		// fixed path: every route from a PRESENTATION_STYLE_ASSIGNMENT ends at a colour or at
		// nothing, and there is only ever one.
		bool colorUnder(const std::map<long, Entity>& entities, long start, StepColor& out)
		{
			std::set<long> seen;
			std::vector<long> stack{ start };
			while (!stack.empty())
			{
				const long id = stack.back();
				stack.pop_back();
				if (!seen.insert(id).second) { continue; }
				const std::map<long, Entity>::const_iterator it = entities.find(id);
				if (it == entities.end()) { continue; }

				if (it->second.type == "COLOUR_RGB")
				{
					const std::vector<double> numbers = numbersIn(it->second.args);
					if (numbers.size() >= 3)
					{
						out = { numbers[0], numbers[1], numbers[2] };
						return true;
					}
				}
				if (it->second.type == "DRAUGHTING_PRE_DEFINED_COLOUR"
					|| it->second.type == "PRE_DEFINED_COLOUR")
				{
					if (namedColor(it->second.args, out)) { return true; }
				}
				for (const long ref : referencesIn(it->second.args)) { stack.push_back(ref); }
			}
			return false;
		}

		// The bounding box of every CARTESIAN_POINT reachable from `start`. References in STEP
		// run downward — a solid to its shell, its faces, its curves, its points — so walking
		// down from a MANIFOLD_SOLID_BREP stays inside that solid and reaches all of it.
		bool boxUnder(const std::map<long, Entity>& entities, long start, StepStyledSolid& out)
		{
			std::set<long> seen;
			std::vector<long> stack{ start };
			bool any = false;
			double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;

			while (!stack.empty() && seen.size() < MaxVisited)
			{
				const long id = stack.back();
				stack.pop_back();
				if (!seen.insert(id).second) { continue; }
				const std::map<long, Entity>::const_iterator it = entities.find(id);
				if (it == entities.end()) { continue; }

				if (it->second.type == "CARTESIAN_POINT")
				{
					const std::vector<double> numbers = numbersIn(it->second.args);
					if (numbers.size() >= 3)
					{
						if (!any)
						{
							minX = maxX = numbers[0];
							minY = maxY = numbers[1];
							minZ = maxZ = numbers[2];
							any = true;
						}
						else
						{
							minX = std::min(minX, numbers[0]); maxX = std::max(maxX, numbers[0]);
							minY = std::min(minY, numbers[1]); maxY = std::max(maxY, numbers[1]);
							minZ = std::min(minZ, numbers[2]); maxZ = std::max(maxZ, numbers[2]);
						}
					}
					continue;   // a point references nothing
				}
				for (const long ref : referencesIn(it->second.args)) { stack.push_back(ref); }
			}

			if (!any) { return false; }
			out.centreX = (minX + maxX) / 2.0;
			out.centreY = (minY + maxY) / 2.0;
			out.centreZ = (minZ + maxZ) / 2.0;
			out.sizeX = maxX - minX;
			out.sizeY = maxY - minY;
			out.sizeZ = maxZ - minZ;
			return true;
		}

		bool sameColor(const StepColor& a, const StepColor& b)
		{
			return std::abs(a.r - b.r) < 1e-6 && std::abs(a.g - b.g) < 1e-6
				&& std::abs(a.b - b.b) < 1e-6;
		}
	}

	StepStyles stepStylesOf(const std::string& stepText)
	{
		StepStyles styles;
		if (stepText.find("STYLED_ITEM") == std::string::npos
			&& stepText.find("ISO-10303-21") == std::string::npos)
		{
			return styles;   // not a STEP file at all
		}

		const std::map<long, Entity> entities = readEntities(stepText);
		if (entities.empty())
		{
			return styles;
		}
		styles.ok = true;

		for (const std::pair<const long, Entity>& entry : entities)
		{
			if (entry.second.type != "STYLED_ITEM") { continue; }
			const std::vector<long> refs = referencesIn(entry.second.args);
			if (refs.size() < 2) { continue; }

			// "STYLED_ITEM('', (#style), #target)" — the target is always last.
			const long target = refs.back();
			StepColor color;
			if (!colorUnder(entities, refs.front(), color)) { continue; }

			// Every distinct colour goes in the palette, whatever it styles — a per-face colour
			// is still one of the model's own colours and is worth having for the fallback.
			bool known = false;
			for (const StepColor& existing : styles.palette)
			{
				if (sameColor(existing, color)) { known = true; break; }
			}
			if (!known) { styles.palette.push_back(color); }

			const std::map<long, Entity>::const_iterator targetIt = entities.find(target);
			if (targetIt == entities.end() || targetIt->second.type != "MANIFOLD_SOLID_BREP")
			{
				// A face or a whole representation. Not something a per-solid mesh can honour.
				continue;
			}
			StepStyledSolid solid;
			solid.color = color;
			if (boxUnder(entities, target, solid))
			{
				styles.solids.push_back(solid);
			}
		}

		std::sort(styles.palette.begin(), styles.palette.end(),
			[](const StepColor& a, const StepColor& b) { return a.luminance() < b.luminance(); });
		return styles;
	}

	std::vector<StepColor> colorsForSolids(const StepStyles& styles,
		const std::vector<MeshSolidBox>& solids, bool* outExact)
	{
		if (outExact != nullptr) { *outExact = false; }
		std::vector<StepColor> colors(solids.size());
		if (solids.empty())
		{
			return colors;
		}

		// How far apart two centres may be and still be the same solid. Relative to the model,
		// because "close" for a 2 mm transistor is not "close" for a 50 mm connector.
		double span = 0.0;
		for (const MeshSolidBox& box : solids)
		{
			span = std::max(span, std::max(box.sizeX, std::max(box.sizeY, box.sizeZ)));
		}
		const double tolerance = std::max(span * 0.10, 1e-4);

		// Pair each mesh solid with the styled solid nearest it, and only accept the lot if
		// every pairing is unambiguous. Partial credit is not worth having here: half a model in
		// its real colours and half in guessed ones looks like a rendering fault.
		if (!styles.solids.empty() && styles.solids.size() == solids.size())
		{
			std::vector<bool> taken(styles.solids.size(), false);
			bool allMatched = true;
			for (size_t i = 0; i < solids.size() && allMatched; ++i)
			{
				size_t best = styles.solids.size();
				double bestDistance = 0.0;
				for (size_t j = 0; j < styles.solids.size(); ++j)
				{
					if (taken[j]) { continue; }
					const double distance = std::sqrt(
						std::pow(styles.solids[j].centreX - solids[i].centreX, 2.0)
						+ std::pow(styles.solids[j].centreY - solids[i].centreY, 2.0)
						+ std::pow(styles.solids[j].centreZ - solids[i].centreZ, 2.0));
					if (best == styles.solids.size() || distance < bestDistance)
					{
						best = j;
						bestDistance = distance;
					}
				}
				if (best == styles.solids.size() || bestDistance > tolerance)
				{
					allMatched = false;
					break;
				}
				taken[best] = true;
				colors[i] = styles.solids[best].color;
			}
			if (allMatched)
			{
				if (outExact != nullptr) { *outExact = true; }
				return colors;
			}
		}

		// Fallback: the biggest solid is the housing and the rest are metal. Colours still come
		// from the file's own palette where there is one, so even an unpaired model is drawn in
		// the shades its author chose.
		const StepColor housing = styles.palette.empty()
			? StepColor{ 0.16, 0.16, 0.17 }        // moulded plastic, near black
			: styles.palette.front();              // darkest
		const StepColor metal = styles.palette.size() < 2
			? StepColor{ 0.76, 0.78, 0.80 }        // tinned lead
			: styles.palette.back();               // lightest

		size_t largest = 0;
		for (size_t i = 1; i < solids.size(); ++i)
		{
			if (solids[i].volume > solids[largest].volume) { largest = i; }
		}
		for (size_t i = 0; i < solids.size(); ++i)
		{
			colors[i] = (i == largest) ? housing : metal;
		}
		return colors;
	}

}
