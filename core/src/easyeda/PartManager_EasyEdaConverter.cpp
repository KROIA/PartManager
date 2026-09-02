#include "easyeda/PartManager_EasyEdaConverter.h"
#include "kicad/PartManager_KicadSymbolWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace PartManager
{

	namespace
	{
		// Both EasyEDA documents measure in 10-mil units, which is easy to get wrong by a factor of
		// ten because the PCB numbers *look* like mils. The SOIC-8 fixture settles it: its pads sit
		// 5 units apart and a SOIC-8 pitch is 1.27 mm, so one unit is 10 mil; its body outline
		// spans 19.685 units, which at 10 mil is the 5.0 mm the package name claims.
		constexpr double SchematicUnitMm = 0.254;
		constexpr double PcbUnitMm = 0.254;
		constexpr double Pi = 3.14159265358979323846;

		// Two decimals is KiCad's own precision for symbols; footprints get four because a
		// 1-mil pad offset rounds to nothing at two and the pads would land on top of each other.
		std::string number(double value, int decimals = 4)
		{
			// -0 prints as "-0", which KiCad reads fine but makes a diff of two identical
			// conversions look like a change.
			if (std::fabs(value) < 1e-9)
			{
				value = 0.0;
			}
			char buffer[64] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
			std::string text(buffer);
			// Trailing zeros are noise in a format a human reads in a diff.
			if (text.find('.') != std::string::npos)
			{
				while (!text.empty() && text.back() == '0') { text.pop_back(); }
				if (!text.empty() && text.back() == '.') { text.pop_back(); }
			}
			return text.empty() ? "0" : text;
		}

		std::vector<std::string> split(const std::string& text, char separator)
		{
			std::vector<std::string> parts;
			std::string current;
			for (char c : text)
			{
				if (c == separator)
				{
					parts.push_back(current);
					current.clear();
				}
				else
				{
					current += c;
				}
			}
			parts.push_back(current);
			return parts;
		}

		// EasyEDA separates a pin's sub-parts with "^^" rather than a single character.
		std::vector<std::string> splitOnCaret(const std::string& text)
		{
			std::vector<std::string> parts;
			size_t start = 0;
			for (;;)
			{
				const size_t hit = text.find("^^", start);
				if (hit == std::string::npos)
				{
					parts.push_back(text.substr(start));
					return parts;
				}
				parts.push_back(text.substr(start, hit - start));
				start = hit + 2;
			}
		}

		double toDouble(const std::vector<std::string>& fields, size_t index, double fallback = 0.0)
		{
			if (index >= fields.size() || fields[index].empty())
			{
				return fallback;
			}
			try { return std::stod(fields[index]); }
			catch (...) { return fallback; }
		}

		std::string field(const std::vector<std::string>& fields, size_t index)
		{
			return index < fields.size() ? fields[index] : std::string();
		}

		// Every number in a string, in order. EasyEDA writes point lists as "x y x y" and SVG
		// paths as "M 3990.1575,3002.86 A 2.86,2.86 0 0 0 3990.17,2997.14"; both reduce to this.
		std::vector<double> numbersIn(const std::string& text)
		{
			std::vector<double> values;
			const char* cursor = text.c_str();
			const char* const end = cursor + text.size();
			while (cursor < end)
			{
				// A sign only starts a number when a digit or dot follows it.
				const bool signStart = (*cursor == '-' || *cursor == '+')
					&& (cursor + 1 < end)
					&& (std::isdigit(static_cast<unsigned char>(cursor[1])) || cursor[1] == '.');
				if (std::isdigit(static_cast<unsigned char>(*cursor)) || signStart
					|| (*cursor == '.' && cursor + 1 < end
						&& std::isdigit(static_cast<unsigned char>(cursor[1]))))
				{
					char* stop = nullptr;
					const double value = std::strtod(cursor, &stop);
					if (stop == cursor) { ++cursor; continue; }
					values.push_back(value);
					cursor = stop;
				}
				else
				{
					++cursor;
				}
			}
			return values;
		}

		void note(std::vector<std::string>& skipped, std::map<std::string, int>& counts,
			const std::string& kind)
		{
			PM_UNUSED(skipped);
			++counts[kind];
		}

		void flushSkipped(std::vector<std::string>& skipped, const std::map<std::string, int>& counts)
		{
			for (const auto& entry : counts)
			{
				skipped.push_back(entry.second > 1
					? entry.first + " x" + std::to_string(entry.second)
					: entry.first);
			}
		}

		const char* const FontEffects =
			"\n\t\t\t\t\t(effects\n\t\t\t\t\t\t(font\n\t\t\t\t\t\t\t(size 1.27 1.27)\n"
			"\t\t\t\t\t\t)\n\t\t\t\t\t)\n\t\t\t\t";
	}

	std::string EasyEdaConverter::sanitize(const std::string& name)
	{
		std::string out;
		out.reserve(name.size());
		for (unsigned char c : name)
		{
			// KiCad accepts far more than this, but a library entry that travels through a file
			// name too is safest limited to what every filesystem also accepts.
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.')
			{
				out += static_cast<char>(c);
			}
			else if (c == ' ' || c == '/' || c == '\\' || c == ',')
			{
				out += '_';
			}
		}
		while (!out.empty() && (out.front() == '_' || out.front() == '.')) { out.erase(out.begin()); }
		while (!out.empty() && out.back() == '_') { out.pop_back(); }
		return out.empty() ? "Unnamed" : out;
	}

	std::string EasyEdaConverter::kicadLayer(int easyEdaLayerId)
	{
		switch (easyEdaLayerId)
		{
		case 1:  return "F.Cu";
		case 2:  return "B.Cu";
		case 3:  return "F.SilkS";
		case 4:  return "B.SilkS";
		case 5:  return "F.Paste";
		case 6:  return "B.Paste";
		case 7:  return "F.Mask";
		case 8:  return "B.Mask";
		case 10: return "Edge.Cuts";
		case 11: return "Edge.Cuts";
		case 12: return "Cmts.User";
		case 13: return "F.Fab";
		case 14: return "B.Fab";
		case 15: return "Dwgs.User";
		// EasyEDA's own assembly/courtyard-ish layers. 100/101 carry the component outline and
		// pin-1 marker, which are worth having; 99 is its assembly drawing.
		case 99:  return "Cmts.User";
		case 100: return "F.Fab";
		case 101: return "F.Fab";
		default: return std::string();
		}
	}

	std::string EasyEdaConverter::symbolBlock(const EasyEdaComponent& component,
		const std::string& name, int& outPinCount, std::vector<std::string>& outSkipped)
	{
		outPinCount = 0;
		std::map<std::string, int> skippedCounts;

		const double originX = component.symbolOriginX;
		const double originY = component.symbolOriginY;
		// Y is negated: EasyEDA measures down the screen, a KiCad symbol measures up.
		const auto mapX = [originX](double x) { return (x - originX) * SchematicUnitMm; };
		const auto mapY = [originY](double y) { return -(y - originY) * SchematicUnitMm; };

		std::string graphics;
		std::string pins;

		for (const std::string& shape : component.symbolShapes)
		{
			if (shape.empty()) { continue; }
			const std::string kind = shape.substr(0, shape.find('~'));

			if (kind == "R")
			{
				// R~x~y~rx~ry~width~height~stroke~strokeWidth~style~fill~gId~locked
				const std::vector<std::string> f = split(shape, '~');
				const double x = toDouble(f, 1);
				const double y = toDouble(f, 2);
				const double width = toDouble(f, 5);
				const double height = toDouble(f, 6);
				const bool filled = !field(f, 10).empty() && field(f, 10) != "none";
				graphics += "\t\t\t(rectangle\n";
				graphics += "\t\t\t\t(start " + number(mapX(x), 2) + " " + number(mapY(y), 2) + ")\n";
				graphics += "\t\t\t\t(end " + number(mapX(x + width), 2) + " "
					+ number(mapY(y + height), 2) + ")\n";
				graphics += "\t\t\t\t(stroke\n\t\t\t\t\t(width 0.254)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n";
				graphics += std::string("\t\t\t\t(fill\n\t\t\t\t\t(type ")
					+ (filled ? "background" : "none") + ")\n\t\t\t\t)\n";
				graphics += "\t\t\t)\n";
			}
			else if (kind == "E")
			{
				// E~cx~cy~rx~ry~stroke~strokeWidth~style~fill~gId~locked. KiCad has no ellipse, so
				// the larger radius is used — every EasyEDA symbol ellipse seen so far is a circle.
				const std::vector<std::string> f = split(shape, '~');
				const double radius = std::max(toDouble(f, 3), toDouble(f, 4)) * SchematicUnitMm;
				const bool filled = !field(f, 8).empty() && field(f, 8) != "none";
				graphics += "\t\t\t(circle\n";
				graphics += "\t\t\t\t(center " + number(mapX(toDouble(f, 1)), 2) + " "
					+ number(mapY(toDouble(f, 2)), 2) + ")\n";
				graphics += "\t\t\t\t(radius " + number(radius, 2) + ")\n";
				graphics += "\t\t\t\t(stroke\n\t\t\t\t\t(width 0.254)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n";
				graphics += std::string("\t\t\t\t(fill\n\t\t\t\t\t(type ")
					+ (filled ? "background" : "none") + ")\n\t\t\t\t)\n";
				graphics += "\t\t\t)\n";
			}
			else if (kind == "PL" || kind == "PG")
			{
				// PL~"x y x y ..."~stroke~strokeWidth~style~fill~gId~locked. PG is the same, closed.
				const std::vector<std::string> f = split(shape, '~');
				const std::vector<double> points = numbersIn(field(f, 1));
				if (points.size() < 4) { continue; }
				graphics += "\t\t\t(polyline\n\t\t\t\t(pts\n";
				for (size_t i = 0; i + 1 < points.size(); i += 2)
				{
					graphics += "\t\t\t\t\t(xy " + number(mapX(points[i]), 2) + " "
						+ number(mapY(points[i + 1]), 2) + ")\n";
				}
				if (kind == "PG" && points.size() >= 4)
				{
					graphics += "\t\t\t\t\t(xy " + number(mapX(points[0]), 2) + " "
						+ number(mapY(points[1]), 2) + ")\n";
				}
				graphics += "\t\t\t\t)\n";
				graphics += "\t\t\t\t(stroke\n\t\t\t\t\t(width 0.254)\n\t\t\t\t\t(type default)\n\t\t\t\t)\n";
				graphics += "\t\t\t\t(fill\n\t\t\t\t\t(type none)\n\t\t\t\t)\n";
				graphics += "\t\t\t)\n";
			}
			else if (kind == "P")
			{
				// Sub-parts separated by "^^": [0] settings, [1] dot, [2] path, [3] name, [4] number.
				const std::vector<std::string> segments = splitOnCaret(shape);
				if (segments.size() < 5) { note(outSkipped, skippedCounts, "pin (unreadable)"); continue; }
				const std::vector<std::string> settings = split(segments[0], '~');
				const double x = toDouble(settings, 4);
				const double y = toDouble(settings, 5);
				const int easyRotation = static_cast<int>(toDouble(settings, 6));

				// The pin path is "M <x> <y> h <len>" or "... v <len>"; its length is what KiCad
				// calls the pin length, and its sign is already carried by the rotation.
				const std::vector<std::string> pathFields = split(segments[2], '~');
				const std::vector<double> pathNumbers = numbersIn(field(pathFields, 0));
				double length = 2.54;
				if (pathNumbers.size() >= 3)
				{
					length = std::fabs(pathNumbers.back()) * SchematicUnitMm;
				}
				if (length < 0.01) { length = 2.54; }

				const std::vector<std::string> nameFields = split(segments[3], '~');
				const std::vector<std::string> numberFields = split(segments[4], '~');
				std::string pinName = field(nameFields, 4);
				std::string pinNumber = field(numberFields, 4);
				if (pinName.empty()) { pinName = "~"; }
				if (pinNumber.empty()) { pinNumber = std::to_string(outPinCount + 1); }

				// EasyEDA's rotation points *away* from the body; KiCad's points into it, and the
				// Y flip mirrors the angle on top of that. Both together are (180 - r).
				int angle = (180 - easyRotation) % 360;
				if (angle < 0) { angle += 360; }

				pins += "\t\t\t(pin passive line\n";
				pins += "\t\t\t\t(at " + number(mapX(x), 2) + " " + number(mapY(y), 2) + " "
					+ std::to_string(angle) + ")\n";
				pins += "\t\t\t\t(length " + number(length, 2) + ")\n";
				pins += "\t\t\t\t(name \"" + KicadSymbolWriter::escape(pinName) + "\""
					+ FontEffects + ")\n";
				pins += "\t\t\t\t(number \"" + KicadSymbolWriter::escape(pinNumber) + "\""
					+ FontEffects + ")\n";
				pins += "\t\t\t)\n";
				++outPinCount;
			}
			else if (kind == "T") { note(outSkipped, skippedCounts, "symbol text"); }
			else if (kind == "A") { note(outSkipped, skippedCounts, "symbol arc"); }
			else if (kind == "PT") { note(outSkipped, skippedCounts, "symbol path"); }
			else if (!kind.empty()) { note(outSkipped, skippedCounts, "symbol " + kind); }
		}

		flushSkipped(outSkipped, skippedCounts);

		if (graphics.empty() && pins.empty())
		{
			return std::string();
		}

		std::string reference = component.referencePrefix.empty() ? "U" : component.referencePrefix;
		const std::string value = component.mpn.empty() ? component.title : component.mpn;

		std::string out;
		out += "\t(symbol \"" + KicadSymbolWriter::escape(name) + "\"\n";
		out += "\t\t(pin_names\n\t\t\t(offset 0.254)\n\t\t)\n";
		out += "\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n";
		const auto property = [&out](const char* key, const std::string& text, bool hidden)
			{
				out += std::string("\t\t(property \"") + key + "\" \""
					+ KicadSymbolWriter::escape(text) + "\"\n";
				out += "\t\t\t(at 0 0 0)\n";
				out += "\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1.27 1.27)\n\t\t\t\t)\n";
				if (hidden) { out += "\t\t\t\t(hide yes)\n"; }
				out += "\t\t\t)\n\t\t)\n";
			};
		property("Reference", reference, false);
		property("Value", value, false);
		property("Footprint", component.packageName.empty()
			? std::string() : sanitize(component.packageName), true);
		property("Datasheet", std::string(), true);
		property("Description", component.title, true);
		// The trail back to where this came from. Without it a symbol that turns out wrong gives
		// the user nothing to check it against.
		property("LCSC", component.lcscCode, true);

		// KiCad ties a symbol's bodies to their parent by the "<Parent>_<unit>_<style>" naming
		// convention: _0_1 holds the graphics, _1_1 the pins. Renaming one without the other
		// produces a symbol that draws nothing (KicadSymbolWriter::renamedSymbol knows this too).
		if (!graphics.empty())
		{
			out += "\t\t(symbol \"" + KicadSymbolWriter::escape(name) + "_0_1\"\n";
			out += graphics;
			out += "\t\t)\n";
		}
		if (!pins.empty())
		{
			out += "\t\t(symbol \"" + KicadSymbolWriter::escape(name) + "_1_1\"\n";
			out += pins;
			out += "\t\t)\n";
		}
		out += "\t)\n";
		return out;
	}

	namespace
	{
		// SVG "A rx ry rot largeArc sweep x y" -> the point halfway along the drawn sweep, which
		// is the third point a KiCad fp_arc wants. Endpoint-to-centre conversion, W3C SVG
		// implementation notes F.6.5, restricted to the circular case EasyEDA emits (rx == ry,
		// no rotation). Returns false when the parameters describe no arc at all.
		bool arcMidPoint(double x1, double y1, double radius, bool largeArc, bool sweep,
			double x2, double y2, double& outX, double& outY)
		{
			const double dx = (x1 - x2) * 0.5;
			const double dy = (y1 - y2) * 0.5;
			double r = std::fabs(radius);
			const double chordHalf = std::sqrt(dx * dx + dy * dy);
			if (chordHalf < 1e-9) { return false; }
			// A radius too small for the chord is not an error in SVG; it is scaled up to fit.
			if (r < chordHalf) { r = chordHalf; }

			const double height = std::sqrt(std::max(0.0, r * r - chordHalf * chordHalf));
			const double midX = (x1 + x2) * 0.5;
			const double midY = (y1 + y2) * 0.5;
			// Unit normal to the chord. F.6.5.2 gives the centre offset as
			// ±(height/chordHalf)·(dy, -dx), and this normal is the negative of that vector — so
			// the spec's "+ when largeArc != sweep" becomes "+ when they are equal" here.
			// Getting it backwards is not a crash, it is an arc that bulges the wrong way: the
			// SOIC pin-1 notch ends up sticking out of the body outline instead of cut into it.
			const double normalX = -dy / chordHalf;
			const double normalY = dx / chordHalf;
			const double sign = (largeArc == sweep) ? 1.0 : -1.0;
			const double centreX = midX + sign * height * normalX;
			const double centreY = midY + sign * height * normalY;

			// The mid point is where the perpendicular bisector leaves the circle, on the side the
			// sweep actually travels.
			double toMidX = midX - centreX;
			double toMidY = midY - centreY;
			const double toMidLength = std::sqrt(toMidX * toMidX + toMidY * toMidY);
			if (toMidLength < 1e-9)
			{
				// Half-circle: the bisector passes through the centre, so take the normal instead.
				toMidX = normalX;
				toMidY = normalY;
			}
			else
			{
				toMidX /= toMidLength;
				toMidY /= toMidLength;
			}
			const double outward = largeArc ? -1.0 : 1.0;
			outX = centreX + outward * r * toMidX;
			outY = centreY + outward * r * toMidY;
			return true;
		}
	}

	std::string EasyEdaConverter::footprintFile(const EasyEdaComponent& component,
		const std::string& name, int& outPadCount, std::vector<std::string>& outSkipped)
	{
		outPadCount = 0;
		std::map<std::string, int> skippedCounts;

		const double originX = component.footprintOriginX;
		const double originY = component.footprintOriginY;
		// No Y negation here: a KiCad footprint measures down, exactly as EasyEDA does.
		const auto mapX = [originX](double x) { return (x - originX) * PcbUnitMm; };
		const auto mapY = [originY](double y) { return (y - originY) * PcbUnitMm; };

		std::string body;

		for (const std::string& shape : component.footprintShapes)
		{
			if (shape.empty()) { continue; }
			const std::string kind = shape.substr(0, shape.find('~'));
			const std::vector<std::string> f = split(shape, '~');

			if (kind == "PAD")
			{
				// PAD~shape~x~y~width~height~layer~net~number~holeRadius~points~rotation~gId~...
				const std::string padShape = field(f, 1);
				const double x = toDouble(f, 2);
				const double y = toDouble(f, 3);
				const double width = toDouble(f, 4) * PcbUnitMm;
				const double height = toDouble(f, 5) * PcbUnitMm;
				const int layer = static_cast<int>(toDouble(f, 6));
				std::string padNumber = field(f, 8);
				const double holeRadius = toDouble(f, 9) * PcbUnitMm;
				const double rotation = toDouble(f, 11);
				if (padNumber.empty()) { padNumber = std::to_string(outPadCount + 1); }

				// EasyEDA's OVAL is an obround, ELLIPSE a circle, RECT a rectangle. POLYGON is a
				// custom outline KiCad can express but not in three lines, so it degrades to its
				// bounding rectangle — noted, because that is a real approximation on copper.
				std::string kicadShape = "rect";
				if (padShape == "ELLIPSE") { kicadShape = "circle"; }
				else if (padShape == "OVAL") { kicadShape = "oval"; }
				else if (padShape == "POLYGON")
				{
					note(outSkipped, skippedCounts, "polygon pad outline (used its bounding box)");
				}

				const bool throughHole = holeRadius > 1e-9;
				const char* padType = throughHole ? "thru_hole" : "smd";
				std::string layers;
				if (throughHole)
				{
					layers = "\"*.Cu\" \"*.Mask\"";
				}
				else if (layer == 2)
				{
					layers = "\"B.Cu\" \"B.Paste\" \"B.Mask\"";
				}
				else
				{
					layers = "\"F.Cu\" \"F.Paste\" \"F.Mask\"";
				}

				body += "\t(pad \"" + padNumber + "\" " + padType + " " + kicadShape + "\n";
				body += "\t\t(at " + number(mapX(x)) + " " + number(mapY(y));
				if (std::fabs(rotation) > 1e-9) { body += " " + number(rotation, 1); }
				body += ")\n";
				body += "\t\t(size " + number(width) + " " + number(height) + ")\n";
				if (throughHole)
				{
					body += "\t\t(drill " + number(holeRadius * 2.0) + ")\n";
				}
				body += "\t\t(layers " + layers + ")\n";
				body += "\t)\n";
				++outPadCount;
			}
			else if (kind == "TRACK")
			{
				// TRACK~strokeWidth~layer~net~"x y x y ..."~gId~locked
				const double width = toDouble(f, 1) * PcbUnitMm;
				const std::string layer = kicadLayer(static_cast<int>(toDouble(f, 2)));
				if (layer.empty()) { note(outSkipped, skippedCounts, "track on an unmapped layer"); continue; }
				const std::vector<double> points = numbersIn(field(f, 4));
				for (size_t i = 0; i + 3 < points.size(); i += 2)
				{
					body += "\t(fp_line\n";
					body += "\t\t(start " + number(mapX(points[i])) + " " + number(mapY(points[i + 1])) + ")\n";
					body += "\t\t(end " + number(mapX(points[i + 2])) + " " + number(mapY(points[i + 3])) + ")\n";
					body += "\t\t(stroke\n\t\t\t(width " + number(width > 1e-9 ? width : 0.12)
						+ ")\n\t\t\t(type solid)\n\t\t)\n";
					body += "\t\t(layer \"" + layer + "\")\n";
					body += "\t)\n";
				}
			}
			else if (kind == "CIRCLE")
			{
				// CIRCLE~cx~cy~radius~strokeWidth~layer~gId~locked
				const double centreX = toDouble(f, 1);
				const double centreY = toDouble(f, 2);
				const double radius = toDouble(f, 3) * PcbUnitMm;
				const double width = toDouble(f, 4) * PcbUnitMm;
				const std::string layer = kicadLayer(static_cast<int>(toDouble(f, 5)));
				if (layer.empty()) { note(outSkipped, skippedCounts, "circle on an unmapped layer"); continue; }
				body += "\t(fp_circle\n";
				body += "\t\t(center " + number(mapX(centreX)) + " " + number(mapY(centreY)) + ")\n";
				// KiCad takes a rim point, not a radius — the same trap KicadGeometry documents.
				body += "\t\t(end " + number(mapX(centreX) + radius) + " " + number(mapY(centreY)) + ")\n";
				body += "\t\t(stroke\n\t\t\t(width " + number(width > 1e-9 ? width : 0.12)
					+ ")\n\t\t\t(type solid)\n\t\t)\n";
				body += "\t\t(fill no)\n";
				body += "\t\t(layer \"" + layer + "\")\n";
				body += "\t)\n";
			}
			else if (kind == "ARC")
			{
				// ARC~strokeWidth~layer~net~"M x,y A rx,ry rot large sweep x2,y2"~~gId~locked
				const double width = toDouble(f, 1) * PcbUnitMm;
				const std::string layer = kicadLayer(static_cast<int>(toDouble(f, 2)));
				const std::string path = field(f, 4);
				const std::vector<double> v = numbersIn(path);
				// M x1 y1 A rx ry rotation largeArc sweep x2 y2 — eleven numbers, in that order.
				if (layer.empty() || v.size() < 9)
				{
					note(outSkipped, skippedCounts, "arc"); continue;
				}
				double midX = 0.0;
				double midY = 0.0;
				if (!arcMidPoint(v[0], v[1], v[2], v[5] != 0.0, v[6] != 0.0, v[7], v[8], midX, midY))
				{
					note(outSkipped, skippedCounts, "arc"); continue;
				}
				body += "\t(fp_arc\n";
				body += "\t\t(start " + number(mapX(v[0])) + " " + number(mapY(v[1])) + ")\n";
				body += "\t\t(mid " + number(mapX(midX)) + " " + number(mapY(midY)) + ")\n";
				body += "\t\t(end " + number(mapX(v[7])) + " " + number(mapY(v[8])) + ")\n";
				body += "\t\t(stroke\n\t\t\t(width " + number(width > 1e-9 ? width : 0.12)
					+ ")\n\t\t\t(type solid)\n\t\t)\n";
				body += "\t\t(layer \"" + layer + "\")\n";
				body += "\t)\n";
			}
			else if (kind == "SOLIDREGION") { note(outSkipped, skippedCounts, "copper/assembly region"); }
			else if (kind == "SVGNODE") { note(outSkipped, skippedCounts, "3D model outline"); }
			else if (kind == "TEXT") { note(outSkipped, skippedCounts, "footprint text"); }
			else if (kind == "HOLE") { note(outSkipped, skippedCounts, "mounting hole"); }
			else if (kind == "VIA") { note(outSkipped, skippedCounts, "via"); }
			else if (!kind.empty()) { note(outSkipped, skippedCounts, "footprint " + kind); }
		}

		flushSkipped(outSkipped, skippedCounts);

		if (body.empty())
		{
			return std::string();
		}

		std::string out;
		out += "(footprint \"" + KicadSymbolWriter::escape(name) + "\"\n";
		out += "\t(version 20241229)\n";
		out += "\t(generator \"PartManager\")\n";
		out += "\t(generator_version \"9.0\")\n";
		out += "\t(layer \"F.Cu\")\n";
		out += "\t(descr \"" + KicadSymbolWriter::escape(component.title) + "\")\n";
		out += "\t(attr smd)\n";
		// Reference and Value are mandatory; a footprint without them loads with a warning.
		out += "\t(property \"Reference\" \"REF**\"\n\t\t(at 0 -3 0)\n\t\t(layer \"F.SilkS\")\n"
			"\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n";
		out += "\t(property \"Value\" \"" + KicadSymbolWriter::escape(name) + "\"\n\t\t(at 0 3 0)\n"
			"\t\t(layer \"F.Fab\")\n"
			"\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n";
		out += body;
		out += ")\n";
		return out;
	}

	EasyEdaConversion EasyEdaConverter::convert(const EasyEdaComponent& component)
	{
		EasyEdaConversion conversion;
		if (!component.ok)
		{
			conversion.errorMessage = component.errorMessage.empty()
				? std::string("No EasyEDA component to convert.") : component.errorMessage;
			return conversion;
		}

		const std::string base = component.mpn.empty() ? component.title : component.mpn;
		conversion.symbolName = sanitize(base);
		conversion.footprintName = sanitize(component.packageName.empty()
			? base : component.packageName);

		const std::string block = symbolBlock(component, conversion.symbolName,
			conversion.pinCount, conversion.skipped);
		if (!block.empty())
		{
			// Through library() so the result is a file KiCad opens on its own, not a fragment.
			// It embeds the base symbols too; harmless here — this symbol extends none of them.
			conversion.symbolLibraryText = KicadSymbolWriter::library({ block });
		}

		conversion.footprintText = footprintFile(component, conversion.footprintName,
			conversion.padCount, conversion.skipped);

		if (conversion.symbolLibraryText.empty() && conversion.footprintText.empty())
		{
			conversion.errorMessage = "EasyEDA's data for this part held no shape PartManager "
				"could convert.";
			return conversion;
		}

		conversion.ok = true;
		return conversion;
	}

}
