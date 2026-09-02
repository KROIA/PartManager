// @file PartManager_Model3DViewer.h
// @brief Qt3D mesh viewer for a part's 3D model (§13) — orbit, zoom, reset.
//
// A `QWidget` wrapper around a `Qt3DExtras::Qt3DWindow`, so it drops into an
// ordinary layout next to the rest of the app. Qt3D lives here in the app and
// never in `core/` — the library stays widget-free (§12a) and the *decision*
// about what can be drawn lives in `core/model3d` where it is testable without a
// GPU.
//
// **Only mesh formats draw.** `showModel()` reports what happened rather than
// failing silently: a STEP file is a valid, correctly-stored model this build
// cannot tessellate, and the panel says exactly that instead of showing an empty
// scene the user reads as a broken file.
//
// The camera frames the mesh from its bounding sphere once the geometry has
// actually loaded — QMesh loads asynchronously, so framing at showModel() time
// would aim at an empty scene every time.
// @see docs/design/ARCHITECTURE.md §13, §5a, §12a
// @see PartManager_Model3DFormat.h
#pragma once

#include "model3d/PartManager_Model3DFormat.h"
#include <QWidget>
#include <QString>

namespace Qt3DCore { class QEntity; class QTransform; }
namespace Qt3DRender { class QMesh; class QCamera; }
namespace Qt3DExtras { class Qt3DWindow; class QOrbitCameraController; class QPhongMaterial; }

class QLabel;
class QStackedWidget;

namespace PartManager
{

	class Model3DViewer : public QWidget
	{
		Q_OBJECT
	public:
		explicit Model3DViewer(QWidget* parent = nullptr);

		// Shows `absolutePath`. Returns false when the file cannot be drawn — a format this
		// build has no loader for, or a path that is not there — with the reason in the panel
		// and in outMessage. An empty path clears the viewer, which is not a failure.
		bool showModel(const QString& absolutePath, QString* outMessage = nullptr);

		// Puts the camera back where showModel() left it, for a "Reset view" button.
		void resetView();

	private:
		// Builds the scene once; showModel() only ever swaps the mesh's source afterwards, so
		// there is one Qt3D window for the widget's lifetime rather than one per model.
		void buildScene();
		// Frames whatever the mesh currently holds. Called after the geometry reports itself
		// loaded, because QMesh is asynchronous and the bounding volume is empty until then.
		void frameModel();
		void showMessage(const QString& message);

		QStackedWidget* m_stack = nullptr;
		QLabel* m_messageLabel = nullptr;
		Qt3DExtras::Qt3DWindow* m_window = nullptr;
		Qt3DCore::QEntity* m_root = nullptr;
		Qt3DCore::QEntity* m_modelEntity = nullptr;
		Qt3DRender::QMesh* m_mesh = nullptr;
		Qt3DExtras::QOrbitCameraController* m_cameraController = nullptr;
	};

}
