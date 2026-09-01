#include "widgets/PartManager_TagChipDelegate.h"

#include <QPainter>
#include <QStringList>

namespace PartManager
{
	namespace
	{
		constexpr int ChipPaddingX = 5;   // horizontal text inset inside a chip
		constexpr int ChipSpacing = 4;    // gap between the text and a chip, and between chips
		constexpr int ChipRadius = 3;
		constexpr int ChipInsetY = 2;     // vertical inset of the chip inside the row

		QVariantList tagsOf(const QModelIndex& index)
		{
			return index.data(TagChipDelegate::TagsRole).toList();
		}

		// Black on light chips, white on dark ones — a fixed text colour is unreadable
		// on half of the palette a user can pick in Manage Tags.
		QColor readableTextColor(const QColor& background)
		{
			double luminance = 0.299 * background.redF() + 0.587 * background.greenF() + 0.114 * background.blueF();
			return luminance > 0.6 ? QColor(Qt::black) : QColor(Qt::white);
		}
	}

	TagChipDelegate::TagChipDelegate(QObject* parent)
		: QStyledItemDelegate(parent)
	{
	}

	int TagChipDelegate::chipsWidth(const QStyleOptionViewItem& option, const QModelIndex& index)
	{
		int width = 0;
		for (const QVariant& entry : tagsOf(index))
		{
			QStringList tag = entry.toStringList();
			if (tag.isEmpty())
			{
				continue;
			}
			width += ChipSpacing + option.fontMetrics.horizontalAdvance(tag.first()) + 2 * ChipPaddingX;
		}
		return width;
	}

	void TagChipDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
		const QModelIndex& index) const
	{
		QStyledItemDelegate::paint(painter, option, index);

		QVariantList tags = tagsOf(index);
		if (tags.isEmpty())
		{
			return;
		}

		QStyleOptionViewItem opt(option);
		initStyleOption(&opt, index);

		int x = opt.rect.left() + 4 + opt.fontMetrics.horizontalAdvance(index.data(Qt::DisplayRole).toString());
		int height = opt.rect.height() - 2 * ChipInsetY;

		painter->save();
		painter->setPen(Qt::NoPen);
		painter->setRenderHint(QPainter::Antialiasing, true);
		for (const QVariant& entry : tags)
		{
			QStringList tag = entry.toStringList();
			if (tag.isEmpty())
			{
				continue;
			}
			int width = opt.fontMetrics.horizontalAdvance(tag.first()) + 2 * ChipPaddingX;
			x += ChipSpacing;
			if (x + width > opt.rect.right())
			{
				break; // out of room — the rest stay hidden rather than bleeding into the next column
			}

			QRect chip(x, opt.rect.top() + ChipInsetY, width, height);
			QColor background(tag.size() > 1 ? tag.at(1) : QString());
			if (!background.isValid())
			{
				background = opt.palette.color(QPalette::Mid);
			}
			painter->setBrush(background);
			painter->drawRoundedRect(chip, ChipRadius, ChipRadius);
			painter->setPen(readableTextColor(background));
			painter->drawText(chip, Qt::AlignCenter, tag.first()); // user data — never tr()'d
			painter->setPen(Qt::NoPen);
			x += width;
		}
		painter->restore();
	}

	QSize TagChipDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
	{
		QSize size = QStyledItemDelegate::sizeHint(option, index);
		size.setWidth(size.width() + chipsWidth(option, index));
		return size;
	}

}
