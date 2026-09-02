// @file PartManager_TypeIconPainter.h
// @brief Draws the placeholder icon a part gets when it has no photo.
//
// Which glyph and colour a type maps to is decided in `core/domain/TypeIcon`,
// where it is testable without a screen; this only draws the shapes.
//
// Rendered rather than shipped as image files: the same glyph is needed at table
// size (28 px) and at preview size (100+ px), and one vector routine beats two
// sets of PNGs that go soft on a high-DPI display. Results are cached in
// QPixmapCache under the type and size, so scrolling a long table does not
// re-draw the same resistor a thousand times.
// @see docs/design/ARCHITECTURE.md §7b, §7c
// @see PartManager_TypeIcon.h
#pragma once

#include "domain/PartManager_TypeIcon.h"
#include <QPixmap>
#include <QString>

namespace PartManager
{

	class TypeIconPainter
	{
		TypeIconPainter() = delete;
	public:
		// A square icon of `size` pixels for `typeName`. Never null for a non-zero size, which
		// is the point: the caller uses it precisely when there is no real image to show.
		//
		// `devicePixelRatio` is taken from the target widget so the glyph is crisp on a scaled
		// display; the returned pixmap carries it, so callers draw it at logical size as usual.
		static QPixmap icon(const QString& typeName, int size, qreal devicePixelRatio = 1.0);
	};

}
