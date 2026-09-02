#include "kicad/PartManager_KicadGeometry.h"
#include "kicad/PartManager_KicadSymbolWriter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace PartManager
{

	namespace
	{
		// A parsed s-expression: either an atom (`node.atom`) or a list (`node.children`).
		// KiCad's format is small enough that this is the whole grammar.
		struct Node
		{
			bool isList = false;
			std::string atom;
			std::vector<Node> children;

			// The first child list whose head atom is `head`, or null. This is how every lookup
			// in the format is phrased — "(start x y)", "(layer ...)", "(stroke ...)".
			const Node* find(const std::string& head) const
			{
				for (const Node& child : children)
				{
					if (child.isList && !child.children.empty()
						&& !child.children.front().isList
						&& child.children.front().atom == head)
					{
						return &child;
					}
				}
				return nullptr;
			}

			// The head atom of a list, empty for an atom or an empty list.
			const std::string& head() const
			{
				static const std::string none;
				if (!isList || children.empty() || children.front().isList) { return none; }
				return children.front().atom;
			}

			// Child atom `index` counting from the head, as a number. KiCad writes positions as
			// bare atoms after the head — "(at 1.27 -2.54 90)" — so this is the common read.
			double number(std::size_t index, double fallback = 0.0) const
			{
				if (index >= children.size() || children[index].isList) { return fallback; }
				const std::string& text = children[index].atom;
				if (text.empty()) { return fallback; }
				char* end = nullptr;
				const double value = std::strtod(text.c_str(), &end);
				return (end == text.c_str()) ? fallback : value;
			}

			const std::string& text(std::size_t index) const
			{
				static const std::string none;
				if (index >= children.size() || children[index].isList) { return none; }
				return children[index].atom;
			}
		};

		bool isSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		// Recursive-descent over the whole text. `position` is left just past what was read.
		// Quoted strings keep their contents unescaped; nothing else in the format needs
		// unescaping for a preview.
		bool parseNode(const std::string& text, std::size_t& position, Node& out)
		{
			while (position < text.size() && isSpace(text[position])) { ++position; }
			if (position >= text.size()) { return false; }

			if (text[position] == '(')
			{
				++position;
				out.isList = true;
				for (;;)
				{
					while (position < text.size() && isSpace(text[position])) { ++position; }
					if (position >= text.size()) { return false; }
					if (text[position] == ')') { ++position; return true; }
					Node child;
					if (!parseNode(text, position, child)) { return false; }
					out.children.push_back(std::move(child));
				}
			}

			if (text[position] == '"')
			{
				++position;
				out.isList = false;
				while (position < text.size() && text[position] != '"')
				{
					if (text[position] == '\\' && position + 1 < text.size()) { ++position; }
					out.atom.push_back(text[position]);
					++position;
				}
				if (position < text.size()) { ++position; }   // the closing quote
				return true;
			}

			out.isList = false;
			while (position < text.size() && !isSpace(text[position])
				&& text[position] != '(' && text[position] != ')')
			{
				out.atom.push_back(text[position]);
				++position;
			}
			return !out.atom.empty();
		}

		bool parseAll(const std::string& text, Node& out)
		{
			std::size_t position = 0;
			return parseNode(text, position, out) && out.isList;
		}

		KicadPoint pointOf(const Node& node, std::size_t index = 1)
		{
			KicadPoint point;
			point.x = node.number(index);
			point.y = node.number(index + 1);
			return point;
		}

		// Stroke width, from either the current `(stroke (width w))` or the pre-KiCad-7
		// `(width w)` that vendor files still ship. Zero when absent, which the painter reads
		// as "hairline" rather than "invisible".
		double strokeWidthOf(const Node& node)
		{
			if (const Node* stroke = node.find("stroke"))
			{
				if (const Node* width = stroke->find("width")) { return width->number(1); }
			}
			if (const Node* width = node.find("width")) { return width->number(1); }
			return 0.0;
		}

		bool isFilled(const Node& node)
		{
			const Node* fill = node.find("fill");
			if (!fill) { return false; }
			const Node* type = fill->find("type");
			if (!type) { return false; }
			const std::string& how = type->text(1);
			// "background" is KiCad's pale body fill; "none" is not a fill. Anything else
			// (outline, colour) counts as filled.
			return how != "none";
		}

		void appendPoints(const Node& parent, KicadShape& shape)
		{
			const Node* pts = parent.find("pts");
			if (!pts) { return; }
			for (const Node& child : pts->children)
			{
				if (child.isList && child.head() == "xy")
				{
					shape.points.push_back(pointOf(child));
				}
			}
		}

		// The graphic and pin primitives shared by symbol unit bodies. Called for every unit of
		// a symbol, since KiCad splits body graphics ("_0_1") from pins ("_1_1") and both have
		// to be drawn for the symbol to look like anything.
		void collectSymbolShapes(const Node& unit, std::vector<KicadShape>& out)
		{
			for (const Node& node : unit.children)
			{
				if (!node.isList) { continue; }
				const std::string& kind = node.head();

				if (kind == "rectangle")
				{
					const Node* start = node.find("start");
					const Node* end = node.find("end");
					if (!start || !end) { continue; }
					KicadShape shape;
					shape.kind = KicadShapeKind::Rectangle;
					shape.points = { pointOf(*start), pointOf(*end) };
					shape.strokeWidth = strokeWidthOf(node);
					shape.filled = isFilled(node);
					out.push_back(std::move(shape));
				}
				else if (kind == "polyline" || kind == "bezier")
				{
					// A bezier is drawn through its control points rather than curved: four
					// points of a preview-sized curve are within a pixel of the real thing.
					KicadShape shape;
					shape.kind = KicadShapeKind::Polyline;
					appendPoints(node, shape);
					if (shape.points.size() < 2) { continue; }
					shape.strokeWidth = strokeWidthOf(node);
					shape.filled = isFilled(node);
					shape.closed = shape.filled;
					out.push_back(std::move(shape));
				}
				else if (kind == "circle")
				{
					const Node* center = node.find("center");
					const Node* radius = node.find("radius");
					if (!center || !radius) { continue; }
					KicadShape shape;
					shape.kind = KicadShapeKind::Circle;
					shape.points = { pointOf(*center) };
					shape.radius = radius->number(1);
					shape.strokeWidth = strokeWidthOf(node);
					shape.filled = isFilled(node);
					out.push_back(std::move(shape));
				}
				else if (kind == "arc")
				{
					const Node* start = node.find("start");
					const Node* mid = node.find("mid");
					const Node* end = node.find("end");
					if (!start || !mid || !end) { continue; }
					KicadShape shape;
					shape.kind = KicadShapeKind::Arc;
					shape.points = { pointOf(*start), pointOf(*mid), pointOf(*end) };
					shape.strokeWidth = strokeWidthOf(node);
					out.push_back(std::move(shape));
				}
				else if (kind == "pin")
				{
					const Node* at = node.find("at");
					const Node* length = node.find("length");
					if (!at) { continue; }
					// `at` is the *connection* point and the angle points from there back into
					// the body, so the far end is where the symbol outline is. Drawing it the
					// other way round puts every pin inside the box.
					const KicadPoint anchor = pointOf(*at);
					const double angle = at->number(3) * 3.14159265358979323846 / 180.0;
					const double run = length ? length->number(1) : 2.54;
					KicadShape shape;
					shape.kind = KicadShapeKind::Pin;
					shape.points = { anchor,
						KicadPoint{ anchor.x + run * std::cos(angle), anchor.y + run * std::sin(angle) } };
					if (const Node* number = node.find("number")) { shape.label = number->text(1); }
					out.push_back(std::move(shape));
				}
			}
		}

		// KiCad names a symbol's unit bodies "<Parent>_<unit>_<style>", so a nested symbol
		// belongs to `parent` when it is prefixed that way.
		bool isUnitOf(const std::string& unitName, const std::string& parent)
		{
			return unitName.size() > parent.size() + 1
				&& unitName.compare(0, parent.size(), parent) == 0
				&& unitName[parent.size()] == '_';
		}
	}

	bool KicadDrawing::bounds(KicadPoint& outMin, KicadPoint& outMax) const
	{
		bool any = false;
		for (const KicadShape& shape : shapes)
		{
			// A circle's extent is its radius, and a pad's its size — using the centre alone
			// would clip exactly the parts a footprint is mostly made of.
			double padX = (shape.kind == KicadShapeKind::Circle) ? shape.radius : shape.sizeX / 2.0;
			double padY = (shape.kind == KicadShapeKind::Circle) ? shape.radius : shape.sizeY / 2.0;
			if (shape.rotationDegrees != 0.0)
			{
				// The upright box of a turned pad. At 90 degrees this is the swap; in between it
				// is genuinely wider than either side, which is what a footprint measured on the
				// unrotated size would clip off.
				constexpr double DegreesToRadians = 3.14159265358979323846 / 180.0;
				const double c = std::abs(std::cos(shape.rotationDegrees * DegreesToRadians));
				const double s = std::abs(std::sin(shape.rotationDegrees * DegreesToRadians));
				const double halfX = padX, halfY = padY;
				padX = halfX * c + halfY * s;
				padY = halfX * s + halfY * c;
			}
			for (const KicadPoint& point : shape.points)
			{
				if (!any)
				{
					outMin = { point.x - padX, point.y - padY };
					outMax = { point.x + padX, point.y + padY };
					any = true;
					continue;
				}
				outMin.x = std::min(outMin.x, point.x - padX);
				outMin.y = std::min(outMin.y, point.y - padY);
				outMax.x = std::max(outMax.x, point.x + padX);
				outMax.y = std::max(outMax.y, point.y + padY);
			}
		}
		return any;
	}

	KicadDrawing KicadGeometry::symbol(const std::string& libraryText, const std::string& symbolName)
	{
		KicadDrawing drawing;
		drawing.yAxisPointsUp = true;

		Node root;
		if (!parseAll(libraryText, root) || root.head() != "kicad_symbol_lib")
		{
			// A bare `(symbol ...)` block, which is how the generator passes one around.
			Node single;
			if (!parseAll(libraryText, single) || single.head() != "symbol") { return drawing; }
			root = Node{};
			root.isList = true;
			Node head; head.atom = "kicad_symbol_lib";
			root.children.push_back(head);
			root.children.push_back(single);
		}

		// Index the top-level symbols by name, so `extends` can be followed without rescanning.
		std::vector<const Node*> symbols;
		for (const Node& child : root.children)
		{
			if (child.isList && child.head() == "symbol") { symbols.push_back(&child); }
		}
		const auto byName = [&symbols](const std::string& name) -> const Node*
			{
				for (const Node* candidate : symbols)
				{
					if (candidate->text(1) == name) { return candidate; }
				}
				return nullptr;
			};

		const Node* target = symbolName.empty() ? nullptr : byName(symbolName);
		if (!target)
		{
			// No name asked for, or the library does not carry it. Prefer a symbol that is not
			// one of the embedded bases — those are present in every generated library, so
			// taking the first symbol outright would draw a resistor for everything.
			for (const Node* candidate : symbols)
			{
				const std::string& name = candidate->text(1);
				if (name.rfind("PM_", 0) != 0) { target = candidate; break; }
			}
			if (!target && !symbols.empty()) { target = symbols.front(); }
		}
		if (!target) { return drawing; }

		drawing.name = target->text(1);

		// A generated part carries no body of its own — it is `(extends "<base>")` and the
		// base is embedded in the same file. Follow the chain, with a hop limit so a file that
		// extends itself cannot hang the UI.
		const Node* body = target;
		for (int hop = 0; hop < 8; ++hop)
		{
			const Node* extends = body->find("extends");
			if (!extends) { break; }
			const Node* base = byName(extends->text(1));
			if (!base || base == body) { break; }
			body = base;
		}

		const std::string bodyName = body->text(1);
		for (const Node& child : body->children)
		{
			if (!child.isList || child.head() != "symbol") { continue; }
			if (!isUnitOf(child.text(1), bodyName)) { continue; }
			collectSymbolShapes(child, drawing.shapes);
		}
		// A vendor symbol sometimes puts its graphics straight in the outer block instead of
		// in a unit, so take those too rather than drawing an empty box.
		collectSymbolShapes(*body, drawing.shapes);
		return drawing;
	}

	KicadDrawing KicadGeometry::genericSymbolForType(const std::string& typeName)
	{
		std::string base = KicadSymbolWriter::baseSymbolForType(typeName);
		if (base.empty()) { base = KicadSymbolWriter::GenericBaseSymbol; }
		return symbol(KicadSymbolWriter::library(KicadSymbolWriter::baseSymbolBlocks()), base);
	}

	std::string KicadGeometry::withModelPath(const std::string& footprintText,
		const std::string& newPath)
	{
		if (newPath.empty())
		{
			return footprintText;
		}

		// Text surgery rather than parse-and-rewrite on purpose: the S-expression parser above
		// keeps only what a drawing needs, so round-tripping through it would throw away every
		// pad, property and comment in the file.
		for (size_t at = footprintText.find("(model"); at != std::string::npos;
			at = footprintText.find("(model", at + 1))
		{
			// "(models" and "(model_thing" are not this token. A footprint has no such key today,
			// but a cheap boundary check is better than a rewrite that silently corrupts one.
			size_t cursor = at + 6;
			if (cursor >= footprintText.size()
				|| !std::isspace(static_cast<unsigned char>(footprintText[cursor])))
			{
				continue;
			}
			while (cursor < footprintText.size()
				&& std::isspace(static_cast<unsigned char>(footprintText[cursor])))
			{
				++cursor;
			}
			if (cursor >= footprintText.size())
			{
				break;
			}

			// The path is quoted in KiCad 6+ and bare in files written by older tools; both end
			// where the next whitespace or the entry's own children begin.
			size_t end = cursor;
			if (footprintText[cursor] == '"')
			{
				end = footprintText.find('"', cursor + 1);
				if (end == std::string::npos) { break; }
				++end;
			}
			else
			{
				while (end < footprintText.size()
					&& !std::isspace(static_cast<unsigned char>(footprintText[end]))
					&& footprintText[end] != '(' && footprintText[end] != ')')
				{
					++end;
				}
			}

			return footprintText.substr(0, cursor) + "\"" + newPath + "\""
				+ footprintText.substr(end);
		}

		// No model entry at all — a hand-made or stripped-down footprint. Appending one is what
		// makes the copied model reachable; without it the file is correct and useless.
		const size_t close = footprintText.find_last_of(')');
		if (close == std::string::npos)
		{
			return footprintText;
		}
		const std::string entry =
			"  (model \"" + newPath + "\"\n"
			"    (offset (xyz 0 0 0))\n"
			"    (scale (xyz 1 1 1))\n"
			"    (rotate (xyz 0 0 0))\n"
			"  )\n";
		return footprintText.substr(0, close) + entry + footprintText.substr(close);
	}

	KicadDrawing KicadGeometry::footprint(const std::string& footprintText)
	{
		KicadDrawing drawing;
		// Board coordinates, unlike symbols.
		drawing.yAxisPointsUp = false;

		Node root;
		if (!parseAll(footprintText, root)) { return drawing; }
		if (root.head() != "footprint" && root.head() != "module") { return drawing; }
		drawing.name = root.text(1);

		for (const Node& node : root.children)
		{
			if (!node.isList) { continue; }
			const std::string& kind = node.head();
			std::string layer;
			if (const Node* layerNode = node.find("layer")) { layer = layerNode->text(1); }

			if (kind == "fp_line")
			{
				const Node* start = node.find("start");
				const Node* end = node.find("end");
				if (!start || !end) { continue; }
				KicadShape shape;
				shape.kind = KicadShapeKind::Polyline;
				shape.points = { pointOf(*start), pointOf(*end) };
				shape.strokeWidth = strokeWidthOf(node);
				shape.layer = layer;
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "fp_rect")
			{
				const Node* start = node.find("start");
				const Node* end = node.find("end");
				if (!start || !end) { continue; }
				KicadShape shape;
				shape.kind = KicadShapeKind::Rectangle;
				shape.points = { pointOf(*start), pointOf(*end) };
				shape.strokeWidth = strokeWidthOf(node);
				shape.filled = isFilled(node);
				shape.layer = layer;
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "fp_circle")
			{
				const Node* center = node.find("center");
				const Node* end = node.find("end");
				if (!center || !end) { continue; }
				// A footprint circle gives a point on the rim rather than a radius.
				const KicadPoint middle = pointOf(*center);
				const KicadPoint rim = pointOf(*end);
				KicadShape shape;
				shape.kind = KicadShapeKind::Circle;
				shape.points = { middle };
				shape.radius = std::hypot(rim.x - middle.x, rim.y - middle.y);
				shape.strokeWidth = strokeWidthOf(node);
				shape.filled = isFilled(node);
				shape.layer = layer;
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "fp_arc")
			{
				const Node* start = node.find("start");
				const Node* mid = node.find("mid");
				const Node* end = node.find("end");
				if (!start || !mid || !end) { continue; }
				KicadShape shape;
				shape.kind = KicadShapeKind::Arc;
				shape.points = { pointOf(*start), pointOf(*mid), pointOf(*end) };
				shape.strokeWidth = strokeWidthOf(node);
				shape.layer = layer;
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "fp_poly")
			{
				KicadShape shape;
				shape.kind = KicadShapeKind::Polyline;
				appendPoints(node, shape);
				if (shape.points.size() < 2) { continue; }
				shape.strokeWidth = strokeWidthOf(node);
				shape.filled = isFilled(node);
				shape.closed = true;
				shape.layer = layer;
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "pad")
			{
				const Node* at = node.find("at");
				const Node* size = node.find("size");
				if (!at || !size) { continue; }
				KicadShape shape;
				shape.kind = KicadShapeKind::Pad;
				shape.points = { pointOf(*at) };
				shape.sizeX = size->number(1);
				shape.sizeY = size->number(2);
				// "(at -1.05 -0.96 90)" — the optional third number turns the pad. Every
				// side-entry package in the KiCad library uses it.
				shape.rotationDegrees = at->number(3);
				shape.label = node.text(1);
				// The shape is the fourth atom: "(pad "1" smd roundrect ...)". Only round and
				// oval need distinguishing; every other shape is close enough to a rectangle
				// at preview size.
				const std::string& padShape = node.text(3);
				shape.roundPad = (padShape == "circle" || padShape == "oval");
				shape.filled = true;
				if (const Node* layers = node.find("layers")) { shape.layer = layers->text(1); }

				// The third atom is the pad type: "(pad "1" thru_hole circle ...)". Authoritative,
				// unlike the layer list — a through-hole pad is usually on "*.Cu" but a file is
				// free to spell its layers out one by one.
				const std::string& padType = node.text(2);
				shape.throughHole = (padType == "thru_hole" || padType == "np_thru_hole");
				if (const Node* drill = node.find("drill"))
				{
					// Two forms: "(drill 0.9)" and "(drill oval 0.9 1.6)". Taking atom 1 blindly
					// reads the oval one as a diameter of zero, i.e. a hole that is not there.
					shape.drillDiameter = (drill->text(1) == "oval")
						? drill->number(2)
						: drill->number(1);
				}
				drawing.shapes.push_back(std::move(shape));
			}
			else if (kind == "model")
			{
				// Where the part's 3D model sits relative to this footprint. A model file's own
				// origin is not where the part goes: vendor libraries commonly author around the
				// top of the body and put the correction here.
				KicadModelPlacement placement;
				placement.present = true;

				// Two spellings, two units. "(offset (xyz ...))" is the current one and is in
				// millimetres; "(at (xyz ...))" is the legacy one and is in *inches*. Reading
				// the legacy form as millimetres divides the correction by 25.4, which lands
				// near enough to zero to pass for "no offset" while the part sinks into the board.
				if (const Node* offset = node.find("offset"))
				{
					if (const Node* xyz = offset->find("xyz"))
					{
						placement.offsetX = xyz->number(1);
						placement.offsetY = xyz->number(2);
						placement.offsetZ = xyz->number(3);
					}
				}
				else if (const Node* at = node.find("at"))
				{
					if (const Node* xyz = at->find("xyz"))
					{
						constexpr double MmPerInch = 25.4;
						placement.offsetX = xyz->number(1) * MmPerInch;
						placement.offsetY = xyz->number(2) * MmPerInch;
						placement.offsetZ = xyz->number(3) * MmPerInch;
					}
				}

				if (const Node* scale = node.find("scale"))
				{
					if (const Node* xyz = scale->find("xyz"))
					{
						// A zero scale is a model that cannot be seen; treat a missing or absurd
						// value as "unscaled" rather than collapsing the part to a point.
						placement.scaleX = xyz->number(1) != 0.0 ? xyz->number(1) : 1.0;
						placement.scaleY = xyz->number(2) != 0.0 ? xyz->number(2) : 1.0;
						placement.scaleZ = xyz->number(3) != 0.0 ? xyz->number(3) : 1.0;
					}
				}

				if (const Node* rotate = node.find("rotate"))
				{
					if (const Node* xyz = rotate->find("xyz"))
					{
						// Negated here, once, so no renderer has to remember the convention.
						placement.rotateX = -xyz->number(1);
						placement.rotateY = -xyz->number(2);
						placement.rotateZ = -xyz->number(3);
					}
				}

				// A footprint may name several models. The first is the part; the rest are
				// alternates a viewer with no way to choose between them should leave alone.
				if (!drawing.model3D.present)
				{
					drawing.model3D = placement;
				}
			}
		}
		return drawing;
	}

	bool KicadGeometry::arcCircle(const KicadPoint& start, const KicadPoint& mid,
		const KicadPoint& end, KicadPoint& outCentre, double& outRadius,
		double& outStartAngle, double& outSpanAngle)
	{
		const double ax = start.x, ay = start.y;
		const double bx = mid.x, by = mid.y;
		const double cx = end.x, cy = end.y;
		const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
		if (std::abs(d) < 1e-12) { return false; }

		const double aSq = ax * ax + ay * ay;
		const double bSq = bx * bx + by * by;
		const double cSq = cx * cx + cy * cy;
		outCentre.x = (aSq * (by - cy) + bSq * (cy - ay) + cSq * (ay - by)) / d;
		outCentre.y = (aSq * (cx - bx) + bSq * (ax - cx) + cSq * (bx - ax)) / d;
		outRadius = std::hypot(ax - outCentre.x, ay - outCentre.y);

		const double startAngle = std::atan2(ay - outCentre.y, ax - outCentre.x);
		const double midAngle = std::atan2(by - outCentre.y, bx - outCentre.x);
		const double endAngle = std::atan2(cy - outCentre.y, cx - outCentre.x);

		// Sweep from start to end the way that actually passes through the middle point — the
		// short way round is wrong for exactly the arcs that need drawing.
		constexpr double Pi = 3.14159265358979323846;
		double span = endAngle - startAngle;
		while (span <= -Pi) { span += 2.0 * Pi; }
		while (span > Pi) { span -= 2.0 * Pi; }
		double toMid = midAngle - startAngle;
		while (toMid <= -Pi) { toMid += 2.0 * Pi; }
		while (toMid > Pi) { toMid -= 2.0 * Pi; }
		if ((span >= 0.0) != (toMid >= 0.0) || std::abs(toMid) > std::abs(span))
		{
			span += (span >= 0.0) ? -2.0 * Pi : 2.0 * Pi;
		}

		outStartAngle = startAngle;
		outSpanAngle = span;
		return true;
	}

}
