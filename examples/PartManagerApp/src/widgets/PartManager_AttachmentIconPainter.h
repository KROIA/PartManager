// @file PartManager_AttachmentIconPainter.h
// @brief The four-glyph "what is attached" strip drawn in the part list's Files column (§3).
//
// Four fixed slots — datasheet, KiCad symbol, KiCad footprint, 3D model — in that
// order, always all four. **Missing slots are drawn faintly rather than left
// out**, because the useful question is "what is this part still missing?" and a
// strip that only showed what was present would put a part's one attachment in
// the same place as another part's different one.
//
// Drawn in code rather than shipped as .svg icons: the set is four shapes, they
// have to work at 18 px, and they need two states each. A resource file would be
// eight files to keep in step with a palette they already read from.
//
// Cached in QPixmapCache, keyed by the flag set, height and device pixel ratio —
// a table of 400 rows draws at most sixteen distinct strips.
// @see docs/design/ARCHITECTURE.md §3, §7b
// @see PartManager_MainWindowController.h (PartAttachment), PartManager_TypeIconPainter.h
#pragma once

#include <QPixmap>
#include <QString>

namespace PartManager
{

	class AttachmentIconPainter
	{
		AttachmentIconPainter() = delete;
	public:
		// `flags` is an OR of PartAttachment. `height` is the glyph height; the returned pixmap is
		// four glyphs wide plus the gaps between them.
		static QPixmap strip(int flags, int height, qreal devicePixelRatio = 1.0);

		// One glyph on its own, for the Mouser result table where only some slots are knowable.
		// `present` false draws the faint outline.
		static QPixmap single(int flag, bool present, int height, qreal devicePixelRatio = 1.0);

		// "Datasheet, KiCad symbol" — what the strip says, for the cell's tooltip. Names the
		// missing ones too, since that is the half a glyph cannot spell out.
		static QString describe(int flags);
	};

}
