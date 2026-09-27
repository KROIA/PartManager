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
//
// **Two footprints are not N footprints, and `Blend` is where that shows.** With a
// list to sweep, one opaque entry on top is the right answer. With exactly two —
// the suggestion dialog's reference against one candidate — the opaque one hides
// the pads that overlap, which are the only pads worth looking at, so both go
// translucent and an overlap becomes a third colour. The default is the former;
// the latter is opt-in, because the caller is the only thing that knows which of
// the two screens it is.
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

		// How the overlay shares the panel between the footprints on it.
		enum class Blend
		{
			// One entry opaque and on top, the rest faint behind it. The variant browser's
			// scheme and the default: that screen shows *N* variants, where making every one
			// of them translucent turns the overlap into a colour nothing can be read out of,
			// and the highlight is the interaction rather than a decoration on it.
			Highlight,
			// Every entry translucent, so where two of them cover the same copper the result
			// is a third colour and both outlines survive. Two footprints only — the
			// suggestion dialog, where an opaque top entry hides precisely the pads that
			// answer the question being asked.
			Peers
		};

		// One footprint in the comparison.
		struct Entry
		{
			QString key;             // the variant's content hash — what the tree addresses it by
			QString label;
			// What the corner legend says this colour means, and deliberately **not** `label`:
			// that one captions the side-by-side tile ("a1b2c3d4 · 3 parts"), which answers
			// "which variant is this" and says nothing about *why* it is on screen. Empty on
			// every entry — the variant browser's case, where the tree already carries the
			// colours — draws no legend at all, so the key is opt-in by having something to say.
			QString legend;
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

		// Opt-in, and deliberately not decided from the entry count: two entries in the
		// variant browser are still two of *N* variants read by sweeping the tree, which is
		// the interaction `Highlight` exists for. Only a screen that knows its two footprints
		// are peers may say so.
		void setOverlayBlend(Blend blend);
		Blend overlayBlend() const { return m_blend; }

		// The entry drawn opaque, on top. An unknown or empty key highlights nothing, which is
		// a legitimate state — the pointer is outside the tree and nothing is selected.
		void setHighlighted(const QString& key);

		// A sentence drawn **over** the footprints, on a plate opaque enough to survive landing
		// on a pad. Empty (the default) draws nothing, which is what the variant browser wants.
		//
		// Not the same state as the placeholder below it in `paintEvent`: the placeholder means
		// "there is nothing to show", this one means "one of the two is on screen and the other
		// is waiting for you to pick it". A screen that has something to draw must draw it.
		void setHintText(const QString& text);

		QSize minimumSizeHint() const override;

	protected:
		void paintEvent(QPaintEvent* event) override;

	private:
		// Every variant's pads, one shared transform, the highlighted one opaque and last.
		void paintOverlay(QPainter& painter, const QRectF& area);
		// One variant, drawn whole and in KiCad's colours — the picture, not the analysis.
		void paintTile(QPainter& painter, const QRectF& area, const Entry& entry);
		// The corner key: a swatch and a line of text for every entry that carries legend text.
		void paintLegend(QPainter& painter, const QRectF& area);
		// `m_hint`, on its plate, along the bottom edge — below the drawing rather than across it.
		void paintHint(QPainter& painter, const QRectF& area);
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
		QString m_hint;
		Mode m_mode = Mode::Both;
		Blend m_blend = Blend::Highlight;
	};

}
