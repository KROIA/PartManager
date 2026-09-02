// @file PartManager_KicadPreviewWidget.h
// @brief Paints a KicadDrawing — the schematic symbol or PCB footprint of a part.
//
// The parsing lives in `core/kicad` where it is testable without a screen; this
// is only the painter, so the two can be changed independently and the geometry
// stays widget-free (§12a).
//
// Scales the drawing to fit and centres it, rather than offering pan and zoom: at
// the size these panels get, a whole symbol always fits, and a preview the user
// has to navigate is a worse preview. Anything that needs manipulating is opened
// in KiCad.
//
// Colours follow KiCad's own — a dark board canvas for footprints, a light sheet
// for symbols — because the point of the preview is recognising the part as the
// same thing you will see when you place it.
// @see docs/design/ARCHITECTURE.md §5a, §12a
// @see PartManager_KicadGeometry.h
#pragma once

#include "kicad/PartManager_KicadGeometry.h"
#include <QString>
#include <QWidget>

namespace PartManager
{

	class KicadPreviewWidget : public QWidget
	{
		Q_OBJECT
	public:
		explicit KicadPreviewWidget(QWidget* parent = nullptr);

		// Shows `drawing`. An empty one shows `emptyMessage` instead of a blank panel, so
		// "this part has no footprint" never reads as "the preview is broken".
		void showDrawing(const KicadDrawing& drawing, const QString& emptyMessage = QString());

		// Replaces whatever is shown with a line of text — for "not attached yet" and for the
		// reason a file could not be read.
		void showMessage(const QString& message);

		// A caption drawn in the corner: the symbol or footprint name, so the user can tell
		// which of several similar packages they are actually looking at.
		void setCaption(const QString& caption);

		QSize minimumSizeHint() const override;

	protected:
		void paintEvent(QPaintEvent* event) override;

	private:
		KicadDrawing m_drawing;
		QString m_message;
		QString m_caption;
	};

}
