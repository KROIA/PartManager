#include "widgets/PartManager_KicadVariantView.h"
#include "widgets/PartManager_KicadShapePainter.h"

#include <QFont>
#include <QFontMetrics>
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
		// `Blend::Peers`. The top entry is the *lower* of the two on purpose: it composites
		// straight onto the board, while the one behind only ever reaches the eye through
		// `1 - PeerTopOpacity` of itself, so equal numbers would not look equal.
		//
		// Chosen by working the arithmetic out on the two colours this actually runs on and
		// then looking at the result on the user's 0603 resistors. Amber #FFB34D over blue
		// #4FC3F7 over the #00101C board gives three readable colours: amber alone #A67A3C,
		// blue alone #3B96C0, and where they overlap #BBA975 — a pale yellow that is neither
		// of its parents, so a shared pad reads as shared at a glance. Raising the top much
		// past 0.7 collapses the overlap back onto the amber and the change stops being one.
		constexpr double PeerTopOpacity = 0.65;
		constexpr double PeerBackOpacity = 0.75;
		// The overlay gets the larger share in Both mode: it is the mode that answers the
		// question, the strip underneath is the legend.
		constexpr double OverlayShareOfHeight = 0.62;

		// The legend and the hint sit on the same plate: the board colour, nearly opaque. Nearly,
		// rather than fully, because a pad passing under it should still read as "there is
		// something there" — and fully transparent is what made the old placeholder unreadable.
		const QColor PlateBackground(0x00, 0x10, 0x1C, 0xD8);
		const QColor PlateText(0xE8, 0xEC, 0xF0);
		// The empty panel has no drawing to protect, so it gets a solid, brighter background
		// instead of a plate — see `paintEvent` for what that replaced.
		const QColor PlaceholderBackground(0x2C, 0x38, 0x46);
		// Reported unreadable on the user's own library: near-black text on the #00101C board at
		// the default point size. The panel is brighter, the text bright, and a third again as
		// large — enough to read across the room, still a placeholder rather than a banner.
		constexpr double PlaceholderFontScale = 1.35;
		constexpr double HintFontScale = 1.1;
		constexpr int LegendSwatch = 11;
		constexpr int LegendGap = 6;
		constexpr int LegendPadding = 7;
		constexpr double PlateRadius = 4.0;

		QRectF insetBy(const QRectF& area, int margin)
		{
			return area.adjusted(margin, margin, -margin, -margin);
		}

		// A font `factor` times the size of `base`. Both units have to be handled: a font set in
		// pixels answers -1 to pointSizeF(), and scaling that gives a font Qt then ignores.
		QFont enlarged(const QFont& base, double factor)
		{
			QFont scaled(base);
			if (scaled.pointSizeF() > 0.0)
			{
				scaled.setPointSizeF(scaled.pointSizeF() * factor);
			}
			else
			{
				scaled.setPixelSize(qRound(scaled.pixelSize() * factor));
			}
			return scaled;
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

	void KicadVariantView::setOverlayBlend(Blend blend)
	{
		if (m_blend == blend) { return; }
		m_blend = blend;
		update();
	}

	void KicadVariantView::setHighlighted(const QString& key)
	{
		if (m_highlighted == key) { return; }
		m_highlighted = key;
		update();
	}

	void KicadVariantView::setHintText(const QString& text)
	{
		if (m_hint == text) { return; }
		m_hint = text;
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
		// view that has failed. Under `Blend::Peers` the order still decides which colour wins a
		// shared pad — source-over is not commutative — so it is kept either way, and the
		// highlighted entry is still the one the caller wants to read the others against.
		const bool peers = m_blend == Blend::Peers;
		const double topOpacity = peers ? PeerTopOpacity : 1.0;
		const double backOpacity = peers ? PeerBackOpacity : BackgroundOpacity;
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
			painter.setOpacity(isTop ? topOpacity : backOpacity);
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

	void KicadVariantView::paintLegend(QPainter& painter, const QRectF& area)
	{
		// Only the entries that were given wording. The variant browser gives none, so its view
		// is exactly as it was — the key exists for the suggestion dialog, where the two colours
		// mean "what you have" and "what you are looking at" and nothing on screen said so.
		std::vector<const Entry*> keyed;
		for (const Entry& entry : m_entries)
		{
			if (!entry.legend.isEmpty()) { keyed.push_back(&entry); }
		}
		if (keyed.empty()) { return; }

		const QFontMetrics metrics(font());
		const int lineHeight = std::max(metrics.height(), LegendSwatch);
		int textWidth = 0;
		for (const Entry* entry : keyed)
		{
			textWidth = std::max(textWidth, metrics.horizontalAdvance(entry->legend));
		}
		const double plateWidth = 2.0 * LegendPadding + LegendSwatch + LegendGap + textWidth;
		const double plateHeight = 2.0 * LegendPadding
			+ lineHeight * static_cast<double>(keyed.size());
		// Clamped to the panel: the text is elided below rather than allowed to carry the plate
		// off the right edge, which is where a long line would take it on a narrow splitter.
		const QRectF plate(area.left() + Margin, area.top() + Margin,
			std::min(plateWidth, area.width() - 2.0 * Margin), plateHeight);
		if (plate.width() < MinimumUsable) { return; }

		painter.save();
		painter.setPen(Qt::NoPen);
		painter.setBrush(PlateBackground);
		painter.drawRoundedRect(plate, PlateRadius, PlateRadius);
		double top = plate.top() + LegendPadding;
		for (const Entry* entry : keyed)
		{
			const QRectF swatch(plate.left() + LegendPadding,
				top + (lineHeight - LegendSwatch) / 2.0, LegendSwatch, LegendSwatch);
			painter.setPen(Qt::NoPen);
			painter.setBrush(entry->colour);
			painter.drawRect(swatch);

			const QRectF line(swatch.right() + LegendGap, top,
				plate.right() - LegendPadding - swatch.right() - LegendGap, lineHeight);
			painter.setPen(PlateText);
			painter.drawText(line, Qt::AlignLeft | Qt::AlignVCenter,
				metrics.elidedText(entry->legend, Qt::ElideRight,
					static_cast<int>(line.width())));
			top += lineHeight;
		}
		painter.restore();
	}

	void KicadVariantView::paintHint(QPainter& painter, const QRectF& area)
	{
		const QFont hintFont = enlarged(font(), HintFontScale);
		const QFontMetrics metrics(hintFont);
		const double maxWidth = area.width() - 4.0 * Margin;
		if (maxWidth < MinimumUsable) { return; }

		const QRect measured = metrics.boundingRect(
			QRect(0, 0, static_cast<int>(maxWidth), static_cast<int>(area.height())),
			Qt::AlignHCenter | Qt::TextWordWrap, m_hint);
		// Along the bottom rather than across the middle: the footprint under it is the thing the
		// sentence is talking about, and a plate over its pads would hide what the user is being
		// asked to look at.
		const QRectF plate(area.center().x() - measured.width() / 2.0 - LegendPadding,
			area.bottom() - Margin - measured.height() - 2.0 * LegendPadding,
			measured.width() + 2.0 * LegendPadding, measured.height() + 2.0 * LegendPadding);

		painter.save();
		painter.setPen(Qt::NoPen);
		painter.setBrush(PlateBackground);
		painter.drawRoundedRect(plate, PlateRadius, PlateRadius);
		painter.setPen(PlateText);
		painter.setFont(hintFont);
		painter.drawText(plate, Qt::AlignCenter | Qt::TextWordWrap, m_hint);
		painter.restore();
	}

	void KicadVariantView::paintEvent(QPaintEvent* event)
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing, true);

		if (m_entries.empty())
		{
			// **Both halves of this were unreadable** on the user's library: the disabled
			// window-text colour on the #00101C board is near-black on near-black, at the default
			// point size. Lighter panel, bright text, a third again as large — and still nothing
			// but a line of text, because there is genuinely nothing else to show.
			painter.fillRect(event->rect(), PlaceholderBackground);
			painter.setPen(PlateText);
			painter.setFont(enlarged(font(), PlaceholderFontScale));
			painter.drawText(insetBy(QRectF(rect()), Margin).toRect(),
				Qt::AlignCenter | Qt::TextWordWrap,
				tr("Pick a package in the tree to compare its footprints."));
			return;
		}

		painter.fillRect(event->rect(), KicadShapePainter::boardBackground());

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

		// Both last, and both over the whole widget rather than over one of its panels: they are
		// chrome about what is on screen, and a swatch half-hidden under a pad is a swatch that
		// has to be worked out instead of read.
		paintLegend(painter, full);
		if (!m_hint.isEmpty())
		{
			paintHint(painter, full);
		}
	}

}
