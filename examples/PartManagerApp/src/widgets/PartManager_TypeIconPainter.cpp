#include "widgets/PartManager_TypeIconPainter.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPolygonF>

namespace PartManager
{

	namespace
	{
		// Every glyph is drawn in a 0..1 box and scaled at the end, so one routine serves the
		// 28-pixel table thumbnail and the 100-pixel preview without a second set of numbers.
		constexpr qreal Unit = 1.0;

		QColor bodyColour(std::uint32_t rgb)
		{
			return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
		}

		// Leads/pins are drawn in a darkened version of the body so a glyph reads as one object
		// rather than two, and still has contrast on both light and dark row backgrounds.
		QColor leadColour(const QColor& body)
		{
			return body.darker(190);
		}

		void drawLeads(QPainter& painter, const QColor& lead, qreal penWidth, bool horizontal)
		{
			QPen pen(lead, penWidth, Qt::SolidLine, Qt::RoundCap);
			painter.setPen(pen);
			painter.setBrush(Qt::NoBrush);
			if (horizontal)
			{
				painter.drawLine(QPointF(0.02, 0.5), QPointF(0.24, 0.5));
				painter.drawLine(QPointF(0.76, 0.5), QPointF(0.98, 0.5));
			}
			else
			{
				painter.drawLine(QPointF(0.5, 0.02), QPointF(0.5, 0.24));
				painter.drawLine(QPointF(0.5, 0.76), QPointF(0.5, 0.98));
			}
		}

		void paintGlyph(QPainter& painter, const TypeIcon& style, int pixelSize)
		{
			const QColor body = bodyColour(style.colour);
			const QColor lead = leadColour(body);
			// Scaled to the box, so a hairline stays a hairline at 28 px and does not turn into
			// a slab at 128.
			const qreal stroke = 0.075;

			painter.setRenderHint(QPainter::Antialiasing, true);

			switch (style.glyph)
			{
			case TypeGlyph::Resistor:
			{
				drawLeads(painter, lead, stroke, true);
				painter.setPen(QPen(lead, stroke * 0.7));
				painter.setBrush(body);
				// Deliberately tall: a two-terminal glyph confined to the middle band reads as
				// smaller than the chip and connector glyphs sitting beside it in the column.
				painter.drawRect(QRectF(0.24, 0.26, 0.52, 0.48));
				break;
			}
			case TypeGlyph::Capacitor:
			{
				// Two plates with a gap — the one shape everybody reads as a capacitor.
				drawLeads(painter, lead, stroke, true);
				painter.setPen(QPen(body, stroke * 1.9, Qt::SolidLine, Qt::FlatCap));
				painter.drawLine(QPointF(0.40, 0.18), QPointF(0.40, 0.82));
				painter.drawLine(QPointF(0.60, 0.18), QPointF(0.60, 0.82));
				break;
			}
			case TypeGlyph::Inductor:
			{
				drawLeads(painter, lead, stroke, true);
				painter.setPen(QPen(body, stroke * 1.6, Qt::SolidLine, Qt::RoundCap));
				painter.setBrush(Qt::NoBrush);
				// Three bumps, not four: at 28 px a fourth turns the coil into a smudge, and
				// three is still unmistakably a coil rather than a spring.
				for (int bump = 0; bump < 3; ++bump)
				{
					const qreal left = 0.24 + bump * 0.173;
					painter.drawArc(QRectF(left, 0.28, 0.173, 0.44), 0, 180 * 16);
				}
				break;
			}
			case TypeGlyph::Diode:
			case TypeGlyph::Led:
			{
				drawLeads(painter, lead, stroke, true);
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				QPolygonF triangle;
				triangle << QPointF(0.28, 0.22) << QPointF(0.28, 0.78) << QPointF(0.66, 0.50);
				painter.drawPolygon(triangle);
				painter.setPen(QPen(body, stroke * 1.5, Qt::SolidLine, Qt::FlatCap));
				painter.drawLine(QPointF(0.66, 0.22), QPointF(0.66, 0.78));
				if (style.glyph == TypeGlyph::Led)
				{
					// The two emission arrows are the whole difference between a diode and an
					// LED, so they are drawn even at thumbnail size.
					painter.setPen(QPen(lead, stroke * 0.8, Qt::SolidLine, Qt::RoundCap));
					painter.drawLine(QPointF(0.50, 0.20), QPointF(0.64, 0.04));
					painter.drawLine(QPointF(0.66, 0.20), QPointF(0.80, 0.04));
				}
				break;
			}
			case TypeGlyph::Transistor:
			{
				painter.setPen(QPen(lead, stroke * 0.9, Qt::SolidLine, Qt::RoundCap));
				painter.setBrush(body);
				painter.drawEllipse(QPointF(0.52, 0.5), 0.36, 0.36);
				painter.setPen(QPen(lead, stroke, Qt::SolidLine, Qt::RoundCap));
				painter.drawLine(QPointF(0.30, 0.5), QPointF(0.02, 0.5));
				painter.drawLine(QPointF(0.42, 0.30), QPointF(0.42, 0.70));
				painter.drawLine(QPointF(0.42, 0.42), QPointF(0.74, 0.16));
				painter.drawLine(QPointF(0.42, 0.58), QPointF(0.74, 0.84));
				break;
			}
			case TypeGlyph::Ic:
			{
				// A chip body with legs down both sides, plus the pin-1 dot.
				painter.setPen(QPen(lead, stroke * 0.8, Qt::SolidLine, Qt::RoundCap));
				for (int pin = 0; pin < 3; ++pin)
				{
					const qreal y = 0.30 + pin * 0.20;
					painter.drawLine(QPointF(0.06, y), QPointF(0.24, y));
					painter.drawLine(QPointF(0.76, y), QPointF(0.94, y));
				}
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.24, 0.18, 0.52, 0.64), 0.06, 0.06);
				painter.setBrush(body.lighter(210));
				painter.drawEllipse(QPointF(0.34, 0.30), 0.055, 0.055);
				break;
			}
			case TypeGlyph::Connector:
			{
				// A header shell with three pins sticking out of it.
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.14, 0.22, 0.42, 0.56), 0.05, 0.05);
				painter.setPen(QPen(lead, stroke * 1.1, Qt::SolidLine, Qt::RoundCap));
				for (int pin = 0; pin < 3; ++pin)
				{
					const qreal y = 0.32 + pin * 0.18;
					painter.drawLine(QPointF(0.56, y), QPointF(0.92, y));
				}
				break;
			}
			case TypeGlyph::Crystal:
			{
				drawLeads(painter, lead, stroke, true);
				painter.setPen(QPen(body, stroke * 1.7, Qt::SolidLine, Qt::FlatCap));
				painter.drawLine(QPointF(0.32, 0.20), QPointF(0.32, 0.80));
				painter.drawLine(QPointF(0.68, 0.20), QPointF(0.68, 0.80));
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRect(QRectF(0.40, 0.24, 0.20, 0.52));
				break;
			}
			case TypeGlyph::Switch:
			{
				painter.setPen(QPen(lead, stroke, Qt::SolidLine, Qt::RoundCap));
				painter.drawLine(QPointF(0.02, 0.68), QPointF(0.26, 0.68));
				painter.drawLine(QPointF(0.74, 0.68), QPointF(0.98, 0.68));
				// The open lever is what says "switch" rather than "wire".
				painter.setPen(QPen(body, stroke * 1.4, Qt::SolidLine, Qt::RoundCap));
				painter.drawLine(QPointF(0.26, 0.68), QPointF(0.72, 0.28));
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawEllipse(QPointF(0.26, 0.68), 0.075, 0.075);
				painter.drawEllipse(QPointF(0.74, 0.68), 0.075, 0.075);
				break;
			}
			case TypeGlyph::Relay:
			{
				// The coil that drives it, and the contact it closes. An earlier version drew a
				// small body, a thin lever and a dashed link between them: at 28 px that came out
				// as a coloured blob beside a stray diagonal, so the body carries the coil
				// windings and the lever is heavy enough to survive the size.
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.06, 0.22, 0.44, 0.56), 0.06, 0.06);
				painter.setPen(QPen(body.lighter(190), stroke * 0.9, Qt::SolidLine, Qt::RoundCap));
				for (int winding = 0; winding < 3; ++winding)
				{
					const qreal y = 0.36 + winding * 0.14;
					painter.drawLine(QPointF(0.14, y), QPointF(0.42, y));
				}
				painter.setPen(QPen(lead, stroke * 1.3, Qt::SolidLine, Qt::RoundCap));
				painter.drawLine(QPointF(0.60, 0.72), QPointF(0.94, 0.34));
				painter.setPen(Qt::NoPen);
				painter.setBrush(lead);
				painter.drawEllipse(QPointF(0.60, 0.72), 0.075, 0.075);
				painter.drawEllipse(QPointF(0.94, 0.72), 0.075, 0.075);
				break;
			}
			case TypeGlyph::Fuse:
			{
				drawLeads(painter, lead, stroke, true);
				painter.setPen(QPen(body, stroke * 1.1));
				painter.setBrush(Qt::NoBrush);
				painter.drawRoundedRect(QRectF(0.22, 0.26, 0.56, 0.48), 0.08, 0.08);
				// The filament, which is what distinguishes a fuse from a plain body.
				QPainterPath filament;
				filament.moveTo(0.22, 0.50);
				filament.cubicTo(0.40, 0.24, 0.60, 0.76, 0.78, 0.50);
				painter.setPen(QPen(body, stroke * 0.8));
				painter.drawPath(filament);
				break;
			}
			case TypeGlyph::Sensor:
			{
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.16, 0.34, 0.44, 0.44), 0.06, 0.06);
				// Waves coming off it — the usual shorthand for "this measures something".
				painter.setPen(QPen(lead, stroke * 0.8, Qt::SolidLine, Qt::RoundCap));
				painter.setBrush(Qt::NoBrush);
				for (int wave = 0; wave < 3; ++wave)
				{
					const qreal radius = 0.16 + wave * 0.13;
					painter.drawArc(QRectF(0.46 - radius, 0.44 - radius, radius * 2, radius * 2),
						-45 * 16, 90 * 16);
				}
				break;
			}
			case TypeGlyph::Mechanical:
			{
				// A screw seen from the side: head, shank, thread.
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.24, 0.10, 0.52, 0.18), 0.05, 0.05);
				painter.drawRect(QRectF(0.40, 0.28, 0.20, 0.44));
				QPolygonF tip;
				tip << QPointF(0.40, 0.72) << QPointF(0.60, 0.72) << QPointF(0.50, 0.94);
				painter.drawPolygon(tip);
				painter.setPen(QPen(bodyColour(style.colour).lighter(190), stroke * 0.7));
				for (int thread = 0; thread < 3; ++thread)
				{
					const qreal y = 0.36 + thread * 0.12;
					painter.drawLine(QPointF(0.40, y), QPointF(0.60, y));
				}
				break;
			}
			case TypeGlyph::Generic:
			{
				painter.setPen(Qt::NoPen);
				painter.setBrush(body);
				painter.drawRoundedRect(QRectF(0.10, 0.10, 0.80, 0.80), 0.12, 0.12);
				if (!style.initials.empty())
				{
					// Drawn unscaled: text inside a 0..1 transform picks a sub-pixel font and
					// comes out as a smear. Below this the letters are illegible anyway, so
					// there is no point drawing them at all.
					painter.save();
					painter.resetTransform();
					if (pixelSize >= 20)
					{
						QFont font = painter.font();
						font.setPixelSize(std::max(8, int(pixelSize * 0.44)));
						font.setBold(true);
						painter.setFont(font);
						painter.setPen(Qt::white);
						painter.drawText(QRect(0, 0, pixelSize, pixelSize), Qt::AlignCenter,
							QString::fromStdString(style.initials));
					}
					painter.restore();
				}
				break;
			}
			}
		}
	}

	QPixmap TypeIconPainter::icon(const QString& typeName, int size, qreal devicePixelRatio)
	{
		if (size <= 0) { return QPixmap(); }

		const QString key = QStringLiteral("pm-typeicon-%1-%2-%3")
			.arg(typeName).arg(size).arg(devicePixelRatio);
		QPixmap cached;
		if (QPixmapCache::find(key, &cached)) { return cached; }

		const qreal ratio = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
		QPixmap pixmap(int(size * ratio), int(size * ratio));
		pixmap.setDevicePixelRatio(ratio);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		const TypeIcon style = TypeIconStyle::forType(typeName.toStdString());
		painter.scale(size / Unit, size / Unit);
		paintGlyph(painter, style, size);
		painter.end();

		QPixmapCache::insert(key, pixmap);
		return pixmap;
	}

}
