#include "widgets/PartManager_AttachmentIconPainter.h"
#include "controllers/PartManager_MainWindowController.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QStringList>
#include <algorithm>

namespace PartManager
{
	namespace
	{
		// Colours picked to be distinguishable at 14 px and from each other when desaturated:
		// paper white for the datasheet, KiCad's own symbol red and silkscreen yellow for the two
		// library files, and a neutral blue for the 3D model.
		const QColor DatasheetColour(0xC0, 0x39, 0x2B);
		const QColor SymbolColour(0x84, 0x00, 0x00);
		const QColor FootprintColour(0xB8, 0x86, 0x0B);
		const QColor ModelColour(0x2E, 0x6D, 0xA4);
		// What an empty slot looks like: present but plainly unfilled, not invisible.
		const QColor AbsentColour(0x00, 0x00, 0x00, 0x30);

		constexpr int SlotCount = 4;

		int flagAt(int index)
		{
			switch (index)
			{
			case 0:  return AttachmentDatasheet;
			case 1:  return AttachmentKicadSymbol;
			case 2:  return AttachmentKicadFootprint;
			default: return Attachment3DModel;
			}
		}

		QColor colourAt(int index)
		{
			switch (index)
			{
			case 0:  return DatasheetColour;
			case 1:  return SymbolColour;
			case 2:  return FootprintColour;
			default: return ModelColour;
			}
		}

		// Every glyph is drawn inside the unit box and scaled by the caller, so the shapes are
		// readable as ratios instead of as pixel arithmetic.
		void drawGlyph(QPainter& painter, int index, bool present, const QColor& colour)
		{
			QPen pen(colour);
			pen.setWidthF(present ? 0.11 : 0.09);
			pen.setJoinStyle(Qt::MiterJoin);
			painter.setPen(pen);
			painter.setBrush(present ? QBrush(colour.lighter(180)) : Qt::NoBrush);

			switch (index)
			{
			case 0:
			{
				// Datasheet: a page with the corner folded down.
				QPainterPath page;
				page.moveTo(0.20, 0.06);
				page.lineTo(0.62, 0.06);
				page.lineTo(0.80, 0.26);
				page.lineTo(0.80, 0.94);
				page.lineTo(0.20, 0.94);
				page.closeSubpath();
				painter.drawPath(page);
				// The fold, always an outline — filling it would hide it against the page.
				painter.setBrush(Qt::NoBrush);
				painter.drawPolyline(QPolygonF({ QPointF(0.62, 0.06), QPointF(0.62, 0.26),
					QPointF(0.80, 0.26) }));
				if (present)
				{
					painter.drawLine(QPointF(0.31, 0.46), QPointF(0.69, 0.46));
					painter.drawLine(QPointF(0.31, 0.62), QPointF(0.69, 0.62));
					painter.drawLine(QPointF(0.31, 0.78), QPointF(0.56, 0.78));
				}
				break;
			}
			case 1:
			{
				// KiCad symbol: the schematic box with a pin out of each side.
				painter.drawRect(QRectF(0.28, 0.22, 0.44, 0.56));
				painter.setBrush(Qt::NoBrush);
				painter.drawLine(QPointF(0.06, 0.38), QPointF(0.28, 0.38));
				painter.drawLine(QPointF(0.06, 0.62), QPointF(0.28, 0.62));
				painter.drawLine(QPointF(0.72, 0.38), QPointF(0.94, 0.38));
				painter.drawLine(QPointF(0.72, 0.62), QPointF(0.94, 0.62));
				break;
			}
			case 2:
			{
				// KiCad footprint: a land pattern — two rows of pads with the body between them.
				for (int pad = 0; pad < 3; ++pad)
				{
					const double x = 0.16 + pad * 0.28;
					painter.drawRect(QRectF(x, 0.08, 0.16, 0.22));
					painter.drawRect(QRectF(x, 0.70, 0.16, 0.22));
				}
				painter.setBrush(Qt::NoBrush);
				painter.drawRect(QRectF(0.14, 0.38, 0.72, 0.24));
				break;
			}
			default:
			{
				// 3D model: an isometric cube.
				QPainterPath cube;
				cube.moveTo(0.50, 0.06);
				cube.lineTo(0.92, 0.28);
				cube.lineTo(0.92, 0.72);
				cube.lineTo(0.50, 0.94);
				cube.lineTo(0.08, 0.72);
				cube.lineTo(0.08, 0.28);
				cube.closeSubpath();
				painter.drawPath(cube);
				painter.setBrush(Qt::NoBrush);
				// The three edges meeting at the front corner are what make it read as a solid
				// rather than as a hexagon.
				painter.drawLine(QPointF(0.08, 0.28), QPointF(0.50, 0.50));
				painter.drawLine(QPointF(0.92, 0.28), QPointF(0.50, 0.50));
				painter.drawLine(QPointF(0.50, 0.50), QPointF(0.50, 0.94));
				break;
			}
			}
		}

		QPixmap render(int flags, int slotFrom, int slotTo, int height, qreal devicePixelRatio,
			const QString& cacheKey)
		{
			QPixmap cached;
			if (QPixmapCache::find(cacheKey, &cached))
			{
				return cached;
			}

			// Not `slots`: Qt #defines it as a keyword, and the resulting error blames this line
			// rather than the macro.
			const int glyphCount = slotTo - slotFrom;
			const int gap = std::max(2, height / 6);
			const int width = glyphCount * height + (glyphCount - 1) * gap;

			QPixmap pixmap(static_cast<int>(width * devicePixelRatio),
				static_cast<int>(height * devicePixelRatio));
			pixmap.setDevicePixelRatio(devicePixelRatio);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			for (int slot = slotFrom; slot < slotTo; ++slot)
			{
				const bool present = (flags & flagAt(slot)) != 0;
				painter.save();
				painter.translate((slot - slotFrom) * (height + gap), 0);
				painter.scale(height, height);
				drawGlyph(painter, slot, present, present ? colourAt(slot) : AbsentColour);
				painter.restore();
			}
			painter.end();

			QPixmapCache::insert(cacheKey, pixmap);
			return pixmap;
		}
	}

	QPixmap AttachmentIconPainter::strip(int flags, int height, qreal devicePixelRatio)
	{
		if (height <= 0)
		{
			return QPixmap();
		}
		return render(flags, 0, SlotCount, height, devicePixelRatio,
			QStringLiteral("pm-attach-%1-%2-%3").arg(flags).arg(height).arg(devicePixelRatio));
	}

	QPixmap AttachmentIconPainter::single(int flag, bool present, int height, qreal devicePixelRatio)
	{
		int index = 0;
		for (; index < SlotCount && flagAt(index) != flag; ++index) {}
		if (index == SlotCount || height <= 0)
		{
			return QPixmap();
		}
		return render(present ? flag : 0, index, index + 1, height, devicePixelRatio,
			QStringLiteral("pm-attach1-%1-%2-%3-%4").arg(flag).arg(present)
				.arg(height).arg(devicePixelRatio));
	}

	QString AttachmentIconPainter::describe(int flags)
	{
		const QString names[SlotCount] = {
			QCoreApplication::translate("PartManager::AttachmentIconPainter", "Datasheet"),
			QCoreApplication::translate("PartManager::AttachmentIconPainter", "KiCad symbol"),
			QCoreApplication::translate("PartManager::AttachmentIconPainter", "KiCad footprint"),
			QCoreApplication::translate("PartManager::AttachmentIconPainter", "3D model"),
		};

		QStringList present;
		QStringList missing;
		for (int slot = 0; slot < SlotCount; ++slot)
		{
			((flags & flagAt(slot)) ? present : missing).append(names[slot]);
		}

		const QString separator = QCoreApplication::translate(
			"PartManager::AttachmentIconPainter", ", ");
		if (present.isEmpty())
		{
			return QCoreApplication::translate("PartManager::AttachmentIconPainter",
				"Nothing attached");
		}
		if (missing.isEmpty())
		{
			return QCoreApplication::translate("PartManager::AttachmentIconPainter",
				"Attached: %1").arg(present.join(separator));
		}
		return QCoreApplication::translate("PartManager::AttachmentIconPainter",
			"Attached: %1\nMissing: %2").arg(present.join(separator), missing.join(separator));
	}

}
