#include "widgets/PartManager_KicadPreviewWidget.h"

#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace PartManager
{

	namespace
	{
		// KiCad's own palette, so a part is recognisable as the same thing here and there.
		const QColor SheetBackground(0xFF, 0xFF, 0xFF);
		const QColor SymbolOutline(0x84, 0x00, 0x00);
		const QColor SymbolFill(0xFF, 0xFF, 0xC2);
		const QColor PinColour(0x84, 0x00, 0x00);

		const QColor BoardBackground(0x00, 0x10, 0x1C);
		const QColor CopperColour(0xC8, 0x34, 0x34);
		const QColor SilkColour(0xF2, 0xED, 0xA1);
		const QColor CourtyardColour(0xE0, 0x60, 0xE0);
		const QColor FabColour(0xC2, 0xB2, 0x80);

		constexpr int Margin = 8;
		// Below this the drawing is a smudge; a caption and nothing else is more honest.
		constexpr int MinimumUsable = 24;

		QColor colourForLayer(const QString& layer)
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

		// KiCad stores an arc as three points on it. Qt wants a bounding box plus two angles, so
		// the centre has to be recovered — it is the circumcentre of the three.
		//
		// Returns false for collinear points, where there is no circle; the caller draws the two
		// segments instead, which is what a zero-curvature arc looks like anyway.
		bool arcGeometry(const KicadPoint& start, const KicadPoint& mid, const KicadPoint& end,
			QPointF& outCentre, double& outRadius, double& outStartAngle, double& outSpanAngle)
		{
			const double ax = start.x, ay = start.y;
			const double bx = mid.x, by = mid.y;
			const double cx = end.x, cy = end.y;
			const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
			if (std::abs(d) < 1e-12) { return false; }

			const double aSq = ax * ax + ay * ay;
			const double bSq = bx * bx + by * by;
			const double cSq = cx * cx + cy * cy;
			const double ux = (aSq * (by - cy) + bSq * (cy - ay) + cSq * (ay - by)) / d;
			const double uy = (aSq * (cx - bx) + bSq * (ax - cx) + cSq * (bx - ax)) / d;

			outCentre = QPointF(ux, uy);
			outRadius = std::hypot(ax - ux, ay - uy);

			const double startAngle = std::atan2(ay - uy, ax - ux);
			const double midAngle = std::atan2(by - uy, bx - ux);
			const double endAngle = std::atan2(cy - uy, cx - ux);

			// Sweep from start to end the way that actually passes through the middle point —
			// the short way round is wrong for exactly the arcs that need drawing.
			double span = endAngle - startAngle;
			while (span <= -M_PI) { span += 2.0 * M_PI; }
			while (span > M_PI) { span -= 2.0 * M_PI; }
			double toMid = midAngle - startAngle;
			while (toMid <= -M_PI) { toMid += 2.0 * M_PI; }
			while (toMid > M_PI) { toMid -= 2.0 * M_PI; }
			if ((span >= 0.0) != (toMid >= 0.0) || std::abs(toMid) > std::abs(span))
			{
				span += (span >= 0.0) ? -2.0 * M_PI : 2.0 * M_PI;
			}

			outStartAngle = startAngle;
			outSpanAngle = span;
			return true;
		}
	}

	KicadPreviewWidget::KicadPreviewWidget(QWidget* parent)
		: QWidget(parent)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	QSize KicadPreviewWidget::minimumSizeHint() const
	{
		return QSize(120, 90);
	}

	void KicadPreviewWidget::showDrawing(const KicadDrawing& drawing, const QString& emptyMessage)
	{
		m_drawing = drawing;
		m_message = drawing.empty() ? emptyMessage : QString();
		update();
	}

	void KicadPreviewWidget::showMessage(const QString& message)
	{
		m_drawing = KicadDrawing();
		m_message = message;
		update();
	}

	void KicadPreviewWidget::setCaption(const QString& caption)
	{
		m_caption = caption;
		update();
	}

	void KicadPreviewWidget::paintEvent(QPaintEvent* event)
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);

		const bool isFootprint = !m_drawing.yAxisPointsUp && !m_drawing.empty();
		const QColor background = m_drawing.empty()
			? palette().color(QPalette::Base)
			: (isFootprint ? BoardBackground : SheetBackground);
		painter.fillRect(event->rect(), background);

		if (m_drawing.empty())
		{
			painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
			painter.drawText(rect().adjusted(Margin, Margin, -Margin, -Margin),
				Qt::AlignCenter | Qt::TextWordWrap, m_message);
			return;
		}

		KicadPoint min, max;
		if (!m_drawing.bounds(min, max)) { return; }

		const double spanX = std::max(max.x - min.x, 0.001);
		const double spanY = std::max(max.y - min.y, 0.001);
		// The caption gets a strip of its own at the bottom. Drawn over the top of the drawing
		// it lands on whatever happens to be there — a resistor's lower pin, a footprint's
		// silkscreen edge — and is unreadable on exactly the parts it is meant to identify.
		const int captionHeight = m_caption.isEmpty() ? 0 : fontMetrics().height() + 2;
		const double usableWidth = width() - 2.0 * Margin;
		const double usableHeight = height() - 2.0 * Margin - captionHeight;
		if (usableWidth < MinimumUsable || usableHeight < MinimumUsable) { return; }

		const double scale = std::min(usableWidth / spanX, usableHeight / spanY);
		const double centreX = (min.x + max.x) / 2.0;
		const double centreY = (min.y + max.y) / 2.0;
		// A symbol's Y axis points up and the screen's points down, so symbols are flipped and
		// footprints are not. Flipping both, or neither, mirrors half the previews.
		const double flip = m_drawing.yAxisPointsUp ? -1.0 : 1.0;

		painter.translate(width() / 2.0, (height() - captionHeight) / 2.0);
		painter.scale(scale, scale * flip);
		painter.translate(-centreX, -centreY);

		// Widths are in millimetres and the painter is scaled, so a cosmetic pen would come out
		// hairline-thin regardless of the file. A zero width in the file means "default".
		const auto penFor = [scale](const QColor& colour, double width)
			{
				QPen pen(colour);
				pen.setWidthF(width > 0.0 ? width : std::max(0.12, 1.0 / scale));
				pen.setCapStyle(Qt::RoundCap);
				pen.setJoinStyle(Qt::RoundJoin);
				return pen;
			};

		for (const KicadShape& shape : m_drawing.shapes)
		{
			const QString layer = QString::fromStdString(shape.layer);
			const QColor stroke = isFootprint ? colourForLayer(layer) : SymbolOutline;

			switch (shape.kind)
			{
			case KicadShapeKind::Rectangle:
			{
				if (shape.points.size() < 2) { break; }
				const QRectF box = QRectF(QPointF(shape.points[0].x, shape.points[0].y),
					QPointF(shape.points[1].x, shape.points[1].y)).normalized();
				painter.setPen(penFor(stroke, shape.strokeWidth));
				painter.setBrush(shape.filled ? QBrush(isFootprint ? stroke : SymbolFill) : Qt::NoBrush);
				painter.drawRect(box);
				break;
			}
			case KicadShapeKind::Circle:
			{
				if (shape.points.empty()) { break; }
				painter.setPen(penFor(stroke, shape.strokeWidth));
				painter.setBrush(shape.filled ? QBrush(isFootprint ? stroke : SymbolFill) : Qt::NoBrush);
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
					painter.setBrush(QBrush(isFootprint ? stroke : SymbolFill));
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
				QPointF centre;
				double radius = 0.0, startAngle = 0.0, spanAngle = 0.0;
				painter.setPen(penFor(stroke, shape.strokeWidth));
				painter.setBrush(Qt::NoBrush);
				if (arcGeometry(shape.points[0], shape.points[1], shape.points[2],
					centre, radius, startAngle, spanAngle))
				{
					const QRectF box(centre.x() - radius, centre.y() - radius, radius * 2.0, radius * 2.0);
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
				painter.setPen(penFor(PinColour, 0.15));
				painter.setBrush(Qt::NoBrush);
				painter.drawLine(QPointF(shape.points[0].x, shape.points[0].y),
					QPointF(shape.points[1].x, shape.points[1].y));
				break;
			}
			case KicadShapeKind::Pad:
			{
				if (shape.points.empty()) { break; }
				const QRectF box(shape.points[0].x - shape.sizeX / 2.0,
					shape.points[0].y - shape.sizeY / 2.0, shape.sizeX, shape.sizeY);
				painter.setPen(Qt::NoPen);
				painter.setBrush(QBrush(CopperColour));
				if (shape.roundPad) { painter.drawEllipse(box); }
				else { painter.drawRect(box); }
				break;
			}
			}
		}

		if (!m_caption.isEmpty())
		{
			painter.resetTransform();
			painter.setPen(isFootprint ? SilkColour : SymbolOutline);
			const QRect strip(Margin, height() - captionHeight - 1,
				width() - 2 * Margin, captionHeight);
			painter.drawText(strip, Qt::AlignBottom | Qt::AlignHCenter,
				fontMetrics().elidedText(m_caption, Qt::ElideMiddle, strip.width()));
		}
	}

}
