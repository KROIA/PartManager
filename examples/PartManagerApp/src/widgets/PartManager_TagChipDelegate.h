// @file PartManager_TagChipDelegate.h
// @brief Draws a part's §2d tags as small coloured chips after the cell's text.
//
// Used on the part table's name column (§7b): the cell keeps its normal text and
// selection painting, and every tag in TagsRole is drawn after it as a rounded
// rect filled with `tag.color`. Chip text colour is picked from the background's
// luminance so a dark tag colour still reads.
//
// The role's value is a QVariantList of QStringList{name, color} pairs — a plain
// list rather than a registered metatype, so nothing has to be declared to Qt's
// meta system just to hand two strings to a painter.
// @see docs/design/ARCHITECTURE.md §2d, §7b, §12b
// @see PartManager_MainWindowController.h
#pragma once

#include <QStyledItemDelegate>

namespace PartManager
{

	class TagChipDelegate : public QStyledItemDelegate
	{
		Q_OBJECT
	public:
		// Item data role holding the chips: QVariantList of QStringList{name, color}.
		static constexpr int TagsRole = Qt::UserRole + 1;

		explicit TagChipDelegate(QObject* parent = nullptr);

		void paint(QPainter* painter, const QStyleOptionViewItem& option,
			const QModelIndex& index) const override;
		QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;

	private:
		// Total width the chips of `index` need, 0 when it carries none.
		static int chipsWidth(const QStyleOptionViewItem& option, const QModelIndex& index);
	};

}
