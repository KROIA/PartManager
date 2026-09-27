#include "widgets/PartManager_KicadShapePainter.h"

#include <QPainter>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace PartManager
{

	namespace
	{
		const QColor SheetBackground(0xFF, 0xFF, 0xFF);
		const QColor SymbolOutline(0x84, 0x00, 0x00);
		const QColor SymbolFill(0xFF, 0xFF, 0xC2);
		const QColor PinColour(0x84, 0x00, 0x00);

		const QColor BoardBackground(0x00, 0x10, 0x1C);
		const QColor CopperColour(0xC8, 0x34, 0x34);
		const QColor SilkColour(0xF2, 0xED, 0xA1);
		const QColor CourtyardColour(0xE0, 0x60, 0xE0);
		const QColor FabColour(0xC2, 0xB2, 0x80);
	}

	namespace KicadShapePainter
	{

		QColor sheetBackground() { return SheetBackground; }
		QColor boardBackground() { return BoardBackground; }
		QColor symbolOutline()   { return SymbolOutline; }
		QColor copperColour()    { return CopperColour; }

		QColor layerColour(const QString& layer)
		{
			if (layer.startsWith(QLatin1String("F.Cu")) || layer.startsWith(QLatin1String("B.Cu"))
				|| layer.startsWith(QLatin1String("*.Cu")))
			{
				return CopperColour;
			}
			if (layer.contains(QLatin1String("SilkS"))) { return SilkColour; }
			if (layer.contains(QLatin1String("CrtYd"))) { return CourtyardColour; }
			if (layer.contains(QLatin1String("Fab"))) { return FabColour; }
			return SilkColour;
		}

		void paint(QPainter& painter, const KicadDrawing& drawing, double scale, const QColor& tint)
		{
			const bool isFootprint = !drawing.yAxisPointsUp;
			const bool tinted = tint.isValid();

			// Widths are in millimetres and the painter is scaled, so a cosmetic pen would come
			// out hairline-thin regardless of the file. A zero width in the file means "default".
			const auto penFor = [scale](const QColor& colour, double width)
				{
					QPen pen(colour);
					pen.setWidthF(width > 0.0 ? width : std::max(0.12, 1.0 / scale));
					pen.setCapStyle(Qt::RoundCap);
					pen.setJoinStyle(Qt::RoundJoin);
					return pen;
				};

			for (const KicadShape& shape : drawing.shapes)
			{
				const QString layer = QString::fromStdString(shape.layer);
				const QColor stroke = tinted
					? tint
					: (isFootprint ? layerColour(layer) : SymbolOutline);
				const QColor fill = tinted ? tint : (isFootprint ? stroke : SymbolFill);

				switch (shape.kind)
				{
				case KicadShapeKind::Rectangle:
				{
					if (shape.points.size() < 2) { break; }
					const QRectF box = QRectF(QPointF(shape.points[0].x, shape.points[0].y),
						QPointF(shape.points[1].x, shape.points[1].y)).normalized();
					painter.setPen(penFor(stroke, shape.strokeWidth));
					painter.setBrush(shape.filled ? QBrush(fill) : Qt::NoBrush);
					painter.drawRect(box);
					break;
				}
				case KicadShapeKind::Circle:
				{
					if (shape.points.empty()) { break; }
					painter.setPen(penFor(stroke, shape.strokeWidth));
					painter.setBrush(shape.filled ? QBrush(fill) : Qt::NoBrush);
					painter.drawEllipse(QPointF(shape.points[0].x, shape.points[0].y),
						shape.radius, shape.radius);
					break;
				}
				case KicadShapeKind::Polyline:
				{
					if (shape.points.size() < 2) { break; }
					QPolygonF polygon;
					for (const KicadPoint& point : shape.points)
					{
						polygon << QPointF(point.x, point.y);
					}
					painter.setPen(penFor(stroke, shape.strokeWidth));
					if (shape.filled)
					{
						painter.setBrush(QBrush(fill));
						painter.drawPolygon(polygon);
					}
					else
					{
						painter.setBrush(Qt::NoBrush);
						if (shape.closed) { painter.drawPolygon(polygon); }
						else { painter.drawPolyline(polygon); }
					}
					break;
				}
				case KicadShapeKind::Arc:
				{
					if (shape.points.size() < 3) { break; }
					KicadPoint centre;
					double radius = 0.0, startAngle = 0.0, spanAngle = 0.0;
					painter.setPen(penFor(stroke, shape.strokeWidth));
					painter.setBrush(Qt::NoBrush);
					// Shared with the 3D board, which walks the same arc in steps rather than
					// handing it to drawArc — one circumcentre, two renderers.
					if (KicadGeometry::arcCircle(shape.points[0], shape.points[1], shape.points[2],
						centre, radius, startAngle, spanAngle))
					{
						const QRectF box(centre.x - radius, centre.y - radius,
							radius * 2.0, radius * 2.0);
						// Qt counts sixteenths of a degree, anticlockwise.
						painter.drawArc(box, int(-startAngle * 180.0 / M_PI * 16.0),
							int(-spanAngle * 180.0 / M_PI * 16.0));
					}
					else
					{
						// Collinear: the "arc" is a straight run through the middle point.
						painter.drawLine(QPointF(shape.points[0].x, shape.points[0].y),
							QPointF(shape.points[2].x, shape.points[2].y));
					}
					break;
				}
				case KicadShapeKind::Pin:
				{
					if (shape.points.size() < 2) { break; }
					painter.setPen(penFor(tinted ? tint : PinColour, 0.15));
					painter.setBrush(Qt::NoBrush);
					painter.drawLine(QPointF(shape.points[0].x, shape.points[0].y),
						QPointF(shape.points[1].x, shape.points[1].y));
					break;
				}
				case KicadShapeKind::Pad:
				{
					if (shape.points.empty()) { break; }
					const QRectF box(-shape.sizeX / 2.0, -shape.sizeY / 2.0,
						shape.sizeX, shape.sizeY);
					painter.setPen(Qt::NoPen);
					painter.setBrush(QBrush(tinted ? tint : CopperColour));
					painter.save();
					painter.translate(shape.points[0].x, shape.points[0].y);
					// KiCad's angle is anticlockwise with Y up; these coordinates have Y down,
					// which turns it into a clockwise one — and clockwise is what
					// QPainter::rotate does.
					painter.rotate(shape.rotationDegrees);
					if (shape.roundPad) { painter.drawEllipse(box); }
					else { painter.drawRect(box); }
					painter.restore();
					break;
				}
				}
			}
		}

	}

}
