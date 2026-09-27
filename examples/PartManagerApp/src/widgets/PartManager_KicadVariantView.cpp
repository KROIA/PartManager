#include "widgets/PartManager_KicadVariantView.h"
#include "widgets/PartManager_KicadShapePainter.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace PartManager
{

	namespace
	{
		constexpr int Margin = 10;
		constexpr int MinimumUsable = 24;
		// Faint enough that the opaque one reads as "on top", strong enough that a pad which
		// only exists on one variant is still visible. Measured by eye against the user's 0603
		// resistors, where the difference is a fraction of a millimetre of pad width.
		constexpr double BackgroundOpacity = 0.35;
		// The overlay gets the larger share in Both mode: it is the mode that answers the
		// question, the strip underneath is the legend.
		constexpr double OverlayShareOfHeight = 0.62;

		QRectF insetBy(const QRectF& area, int margin)
		{
			return area.adjusted(margin, margin, -margin, -margin);
		}
	}

	KicadVariantView::KicadVariantView(QWidget* parent)
		: QWidget(parent)
	{
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	QSize KicadVariantView::minimumSizeHint() const
	{
		return QSize(240, 200);
	}

	void KicadVariantView::setEntries(const std::vector<Entry>& entries)
	{
		m_entries = entries;
		m_pads.clear();
		m_pads.reserve(entries.size());
		for (const Entry& entry : entries)
		{
			m_pads.push_back(KicadGeometry::padsOnly(entry.drawing));
		}
		// The highlight is deliberately not cleared: the same variant is usually still in the
		// new list when the tree moves within one package, and dropping it would blink the view.
		update();
	}

	void KicadVariantView::clear()
	{
		m_entries.clear();
		m_pads.clear();
		m_highlighted.clear();
		update();
	}

	void KicadVariantView::setMode(Mode mode)
	{
		if (m_mode == mode) { return; }
		m_mode = mode;
		update();
	}

	void KicadVariantView::setHighlighted(const QString& key)
	{
		if (m_highlighted == key) { return; }
		m_highlighted = key;
		update();
	}

	bool KicadVariantView::sharedBounds(bool padsOnly, KicadPoint& outMin, KicadPoint& outMax) const
	{
		if (padsOnly)
		{
			return KicadGeometry::unionBounds(m_pads, outMin, outMax);
		}
		std::vector<KicadDrawing> drawings;
		drawings.reserve(m_entries.size());
		for (const Entry& entry : m_entries)
		{
			drawings.push_back(entry.drawing);
		}
		return KicadGeometry::unionBounds(drawings, outMin, outMax);
	}

	void KicadVariantView::paintOverlay(QPainter& painter, const QRectF& area)
	{
		KicadPoint min;
		KicadPoint max;
		// Measured over the pads, because the pads are what is drawn. Using the full geometry
		// would frame the overlay by silkscreen that is no longer on screen and leave the copper
		// small in the middle of its panel.
		if (!sharedBounds(true, min, max)) { return; }

		const double spanX = std::max(max.x - min.x, 0.001);
		const double spanY = std::max(max.y - min.y, 0.001);
		const QRectF usable = insetBy(area, Margin);
		if (usable.width() < MinimumUsable || usable.height() < MinimumUsable) { return; }

		// **The shared transform.** One scale for every variant, so a 0.1 mm pad difference is
		// 0.1 mm of difference on screen instead of being normalised away.
		const double scale = std::min(usable.width() / spanX, usable.height() / spanY);
		const double centreX = (min.x + max.x) / 2.0;
		const double centreY = (min.y + max.y) / 2.0;

		// Highlighted last so it lands on top of the others; when nothing is highlighted every
		// variant is drawn at the background opacity, which reads as "pick one" rather than as a
		// view that has failed.
		std::vector<size_t> order;
		bool haveTop = false;
		size_t topIndex = 0;
		for (size_t i = 0; i < m_entries.size(); ++i)
		{
			if (!m_highlighted.isEmpty() && m_entries[i].key == m_highlighted)
			{
				haveTop = true;
				topIndex = i;
			}
			else
			{
				order.push_back(i);
			}
		}
		if (haveTop) { order.push_back(topIndex); }

		for (size_t index : order)
		{
			const bool isTop = haveTop && index == topIndex;
			painter.save();
			painter.setOpacity(isTop ? 1.0 : BackgroundOpacity);
			painter.translate(usable.center().x(), usable.center().y());
			painter.scale(scale, scale * (m_pads[index].yAxisPointsUp ? -1.0 : 1.0));
			painter.translate(-centreX, -centreY);
			KicadShapePainter::paint(painter, m_pads[index], scale, m_entries[index].colour);
			painter.restore();
		}
	}

	void KicadVariantView::paintTile(QPainter& painter, const QRectF& area, const Entry& entry)
	{
		KicadPoint min;
		KicadPoint max;
		// The full geometry this time, and still the *shared* box: the tiles are side by side to
		// be compared, so one of them being physically larger has to show as larger here too.
		if (!sharedBounds(false, min, max)) { return; }

		const double spanX = std::max(max.x - min.x, 0.001);
		const double spanY = std::max(max.y - min.y, 0.001);
		const QRectF usable = insetBy(area, Margin / 2);
		if (usable.width() < MinimumUsable || usable.height() < MinimumUsable) { return; }

		const double scale = std::min(usable.width() / spanX, usable.height() / spanY);
		const double centreX = (min.x + max.x) / 2.0;
		const double centreY = (min.y + max.y) / 2.0;

		painter.save();
		painter.translate(usable.center().x(), usable.center().y());
		painter.scale(scale, scale * (entry.drawing.yAxisPointsUp ? -1.0 : 1.0));
		painter.translate(-centreX, -centreY);
		// No tint: this is the footprint as KiCad draws it, silkscreen yellow and copper red.
		// The variant's colour is on the frame the caller draws around this tile.
		KicadShapePainter::paint(painter, entry.drawing, scale);
		painter.restore();
	}

	void KicadVariantView::paintEvent(QPaintEvent* event)
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.fillRect(event->rect(), KicadShapePainter::boardBackground());

		if (m_entries.empty())
		{
			painter.setPen(palette().color(QPalette::Disabled, QPalette::WindowText));
			painter.drawText(insetBy(QRectF(rect()), Margin).toRect(),
				Qt::AlignCenter | Qt::TextWordWrap,
				tr("Pick a package in the tree to compare its footprints."));
			return;
		}

		const QRectF full(rect());
		QRectF overlayArea = full;
		QRectF stripArea;
		if (m_mode == Mode::Both)
		{
			overlayArea = QRectF(full.left(), full.top(), full.width(),
				full.height() * OverlayShareOfHeight);
			stripArea = QRectF(full.left(), overlayArea.bottom(), full.width(),
				full.height() - overlayArea.height());
		}
		else if (m_mode == Mode::SideBySide)
		{
			stripArea = full;
			overlayArea = QRectF();
		}

		if (!overlayArea.isEmpty())
		{
			paintOverlay(painter, overlayArea);
		}

		if (!stripArea.isEmpty())
		{
			// One tile per variant, equal width: the comparison is between the drawings, not
			// between how many parts use them.
			const int count = static_cast<int>(m_entries.size());
			const double columnWidth = stripArea.width() / std::max(count, 1);
			for (int i = 0; i < count; ++i)
			{
				const QRectF column(stripArea.left() + i * columnWidth, stripArea.top(),
					columnWidth, stripArea.height());
				const Entry& entry = m_entries[static_cast<size_t>(i)];
				const bool isTop = !m_highlighted.isEmpty() && entry.key == m_highlighted;

				// The frame is where the variant's colour lives on this side — it ties the tile
				// to its tree row and to its silhouette in the overlay without recolouring the
				// footprint itself. Thicker when this is the one in front.
				painter.setPen(QPen(entry.colour, isTop ? 2.5 : 1.0));
				painter.setBrush(Qt::NoBrush);
				const QRectF frame = column.adjusted(1.5, 1.5, -1.5, -1.5);
				painter.drawRect(frame);

				const QString caption = fontMetrics().elidedText(entry.label, Qt::ElideMiddle,
					static_cast<int>(columnWidth) - Margin);
				const QRectF captionStrip(column.left(), column.bottom() - fontMetrics().height() - 3,
					column.width(), static_cast<double>(fontMetrics().height()));
				painter.setOpacity(isTop ? 1.0 : 0.75);
				painter.drawText(captionStrip, Qt::AlignHCenter | Qt::AlignBottom, caption);
				painter.setOpacity(1.0);

				QRectF drawingArea = frame;
				drawingArea.setBottom(captionStrip.top());
				paintTile(painter, drawingArea, entry);
			}
		}
	}

}
