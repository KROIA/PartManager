#include "kicad/PartManager_KicadGeometry.h"
#include "kicad/PartManager_KicadSymbolWriter.h"

#include <algorithm>
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
			const double padX = (shape.kind == KicadShapeKind::Circle) ? shape.radius : shape.sizeX / 2.0;
			const double padY = (shape.kind == KicadShapeKind::Circle) ? shape.radius : shape.sizeY / 2.0;
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
				shape.label = node.text(1);
				// The shape is the fourth atom: "(pad "1" smd roundrect ...)". Only round and
				// oval need distinguishing; every other shape is close enough to a rectangle
				// at preview size.
				const std::string& padShape = node.text(3);
				shape.roundPad = (padShape == "circle" || padShape == "oval");
				shape.filled = true;
				if (const Node* layers = node.find("layers")) { shape.layer = layers->text(1); }
				drawing.shapes.push_back(std::move(shape));
			}
		}
		return drawing;
	}

}
