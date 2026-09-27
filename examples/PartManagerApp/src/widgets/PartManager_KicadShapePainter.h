// @file PartManager_KicadShapePainter.h
// @brief Draws a KicadDrawing's shapes with QPainter — the half two views share.
//
// `KicadPreviewWidget` shows one drawing fitted to itself; `KicadVariantView`
// shows several under one transform. The transform is the only thing they
// disagree about, so the shape loop lives here and neither owns a copy of it —
// a second copy is how a pad that rotates correctly in one preview starts
// rotating the wrong way in the other.
//
// The caller sets the transform and hands over the `scale` it used, which is
// needed for the one thing a scaled painter cannot work out for itself: a pen
// width of "0" in the file means *hairline*, and a hairline is a property of the
// screen, not of the millimetres being drawn.
// @see docs/design/ARCHITECTURE.md §5a, §12a
// @see PartManager_KicadPreviewWidget.h, PartManager_KicadVariantView.h
#pragma once

#include "kicad/PartManager_KicadGeometry.h"
#include <QColor>

class QPainter;

namespace PartManager
{

	namespace KicadShapePainter
	{
		// KiCad's own palette, so a part is recognisable as the same thing here and there.
		QColor sheetBackground();
		QColor boardBackground();
		QColor symbolOutline();
		QColor copperColour();
		// The layer colours a footprint is drawn in: copper, silkscreen, courtyard, fab.
		QColor layerColour(const QString& layer);

		// Paints `drawing` in its own coordinates — the caller has already translated, scaled
		// and flipped. `scale` is that transform's scale factor, for hairline pens.
		//
		// `tint`, when valid, replaces every layer and copper colour: an overlay needs each
		// variant in one identifiable colour, and KiCad's per-layer palette would paint all of
		// them the same red. Invalid (the default) keeps the real palette.
		void paint(QPainter& painter, const KicadDrawing& drawing, double scale,
			const QColor& tint = QColor());
	}

}
