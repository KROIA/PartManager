#include "widgets/PartManager_KicadPreviewWidget.h"
#include "widgets/PartManager_KicadShapePainter.h"

#include <QPaintEvent>
#include <QPainter>

#include <algorithm>

namespace PartManager
{

	namespace
	{
		constexpr int Margin = 8;
		// Below this the drawing is a smudge; a caption and nothing else is more honest.
		constexpr int MinimumUsable = 24;
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
			: (isFootprint ? KicadShapePainter::boardBackground()
				: KicadShapePainter::sheetBackground());
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

		// The shape loop is shared with the overlay view, which sets a different transform over
		// the same shapes — see KicadShapePainter.
		KicadShapePainter::paint(painter, m_drawing, scale);

		if (!m_caption.isEmpty())
		{
			painter.resetTransform();
			painter.setPen(isFootprint ? KicadShapePainter::layerColour(QStringLiteral("F.SilkS"))
				: KicadShapePainter::symbolOutline());
			const QRect strip(Margin, height() - captionHeight - 1,
				width() - 2 * Margin, captionHeight);
			painter.drawText(strip, Qt::AlignBottom | Qt::AlignHCenter,
				fontMetrics().elidedText(m_caption, Qt::ElideMiddle, strip.width()));
		}
	}

}
