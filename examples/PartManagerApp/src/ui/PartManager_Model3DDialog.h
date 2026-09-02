// @file PartManager_Model3DDialog.h
// @brief The 3D model screen for one part (§13) — attach, view, replace, remove.
//
// The ribbon's "3D Viewer" button opens this for the selected part. There is no
// separate "add a 3D model" flow anywhere else: attaching and looking are the
// same screen, because the first thing anyone does after attaching a model is
// check that it is the right one.
//
// **Attaching is not gated on being able to draw it.** A `.step` file is what
// §5a's KiCad export needs and what this viewer cannot tessellate; refusing it
// to keep the viewer tidy would break the more important half. The panel says
// what the format is and why it is not on screen.
// @see docs/design/ARCHITECTURE.md §13, §5a
// @see PartManager_Model3DViewer.h, PartManager_Model3DFormat.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>
#include <QString>

class QLabel;
class QPushButton;

namespace PartManager
{

	class MeshCacheBuilder;
	class Model3DViewer;

	class Model3DDialog : public QDialog
	{
		Q_OBJECT
	public:
		// `builder` is the app's one STEP converter, borrowed rather than owned — a dialog that
		// started its own would race the background sweep for the same cache entry. May be null,
		// in which case a STEP file is reported instead of converted.
		Model3DDialog(DatabaseHandle* handle, MeshCacheBuilder* builder, int partId,
			const QString& partName, QWidget* parent = nullptr);

	signals:
		// A model was attached or removed, so the owner can refresh what it shows and put the
		// new STEP file in front of the converter.
		void modelChanged();

	private slots:
		void attach();
		void detach();
		void openInSystemViewer();

	private:
		// Re-reads the part's model row and repaints the viewer, the label and the buttons.
		void reload();

		PartEditorController m_controller;
		int m_partId;
		Model3DViewer* m_viewer;
		QLabel* m_fileLabel;
		QPushButton* m_attachButton;
		QPushButton* m_detachButton;
		QPushButton* m_openButton;
	};

}
