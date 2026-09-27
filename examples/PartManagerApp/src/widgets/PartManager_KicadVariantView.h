// @file PartManager_KicadVariantView.h
// @brief Several footprints drawn together, so a human can see how they differ (§5a).
//
// The screen this serves exists because naming footprints after the package
// found **13 package names claimed by different pad layouts** in the user's real
// data, with nothing shared. Telling those apart is a visual job: the numbers are
// a hash each, and "which of these is which" is only answerable by looking.
//
// **One transform over all of them, never one each.** `KicadPreviewWidget` fits
// its single drawing to itself, which is right for a preview and wrong here: fit
// each variant separately and a 0402 and an 0805 come out the same size on
// screen, normalising away the difference the view is for. Every mode below
// measures `KicadGeometry::unionBounds()` once and draws everything through it,
// so a 0.1 mm pad difference is 0.1 mm of difference to look at.
//
// **The highlight is the interaction.** One entry is drawn opaque and last; the
// rest sit behind it at reduced opacity in their own colour. The owning screen
// drives `setHighlighted()` from the *hover* over its tree, so swiping down the
// list flips through the variants in place — which is how the user asked to read
// them, and it is much faster than clicking each in turn.
// @see docs/design/ARCHITECTURE.md §5a, §12a
// @see PartManager_KicadShapePainter.h, PartManager_FootprintVariantsDialog.h
#pragma once

#include "kicad/PartManager_KicadGeometry.h"
#include <QColor>
#include <QString>
#include <QWidget>
#include <vector>

namespace PartManager
{

	class KicadVariantView : public QWidget
	{
		Q_OBJECT
	public:
		// Side by side answers "what are they"; overlaid answers "where do they differ";
		// both at once is what the user asked for and is the default, because the two
		// questions are usually asked in that order and a mode switch between them loses
		// the place.
		enum class Mode { SideBySide, Overlay, Both };

		// One footprint in the comparison.
		struct Entry
		{
			QString key;             // the variant's content hash — what the tree addresses it by
			QString label;
			QColor colour;
			KicadDrawing drawing;
		};

		// **The two halves render differently on purpose.** The overlay is an analytical view
		// and abstracts: pads only (`KicadGeometry::padsOnly` — physical interchangeability is
		// decided by the copper, and silkscreen is the busiest layer in most files, so drawing
		// it makes two compatible footprints look unalike), each variant in its own flat
		// colour. A tile is a *picture of the real footprint* and must not abstract: every
		// layer, KiCad's own colours, no tint. The variant's colour reaches a tile through the
		// frame around it, which is enough to map tile → tree row → overlay silhouette without
		// the drawing itself lying about what colour a layer is.

		explicit KicadVariantView(QWidget* parent = nullptr);

		void setEntries(const std::vector<Entry>& entries);
		void clear();
		void setMode(Mode mode);
		Mode mode() const { return m_mode; }

		// The entry drawn opaque, on top. An unknown or empty key highlights nothing, which is
		// a legitimate state — the pointer is outside the tree and nothing is selected.
		void setHighlighted(const QString& key);

		QSize minimumSizeHint() const override;

	protected:
		void paintEvent(QPaintEvent* event) override;

	private:
		// Every variant's pads, one shared transform, the highlighted one opaque and last.
		void paintOverlay(QPainter& painter, const QRectF& area);
		// One variant, drawn whole and in KiCad's colours — the picture, not the analysis.
		void paintTile(QPainter& painter, const QRectF& area, const Entry& entry);
		// The union box of everything currently loaded, over the pads (`m_pads`) or over the
		// full drawings. Two boxes because the two views draw two different sets of shapes: an
		// overlay framed by geometry it is no longer drawing would sit in a corner of its panel.
		// Computed per paint rather than cached — one pass over a handful of rectangles, and a
		// cache here is a cache that can disagree with what is on screen.
		bool sharedBounds(bool padsOnly, KicadPoint& outMin, KicadPoint& outMax) const;

		std::vector<Entry> m_entries;
		// `m_entries` filtered to copper, in the same order. Kept beside them rather than
		// recomputed per paint: it is the same answer every time and paint runs on every hover.
		std::vector<KicadDrawing> m_pads;
		QString m_highlighted;
		Mode m_mode = Mode::Both;
	};

}
