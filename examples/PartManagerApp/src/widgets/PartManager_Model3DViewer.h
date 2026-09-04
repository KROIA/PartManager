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
// cannot tessellate itself, and the panel says exactly that instead of showing
// an empty scene the user reads as a broken file.
//
// **The 3D page is raised the moment a load starts, not when it finishes.** Qt3D
// drives its job graph — including the one that reads the mesh off disk — from
// the render loop, and the render loop only runs while its window is exposed.
// Keeping the container on an unselected QStackedWidget page until the mesh
// reports itself Ready is therefore a deadlock: it can never become Ready. What
// the user sees instead is a progress strip over the scene, and a watchdog that
// says so if the mesh still has not arrived.
//
// STEP files are converted by the shared MeshCacheBuilder rather than here: one
// converter for the whole app means two viewers cannot race to write the same
// cache entry, and a model the background sweep already did opens instantly.
//
// **The scene is KiCad's board frame.** Millimetres, Z up, the part sitting on
// z = 0 — measured from a converted model, not assumed. So the board goes under
// the part at z = 0 with the footprint's pads on it, which is what makes the
// panel answer "is the model the right way up and the right size for this
// footprint" rather than only "is there a model".
//
// **Orbit and zoom, no pan.** The camera always looks at the model's own centre
// and the mouse can only turn around it; there is nothing off-centre to travel
// to, and a part dragged out of frame with no way to say "put it back" is the
// one failure mode a preview this small cannot afford. Zoom starts fitted to the
// model, computed from the mesh file (MeshBounds) rather than from Qt3D's
// extents, which are not populated until a frame or two after the mesh loads.
// @see docs/design/ARCHITECTURE.md §13, §5a, §12a
// @see PartManager_Model3DFormat.h, PartManager_MeshCacheBuilder.h, PartManager_MeshBounds.h
#pragma once

#include "kicad/PartManager_KicadGeometry.h"
#include "model3d/PartManager_MeshBounds.h"
#include "model3d/PartManager_Model3DFormat.h"
#include "model3d/PartManager_StepColors.h"
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <vector>

namespace Qt3DCore { class QEntity; class QTransform; }
namespace Qt3DRender { class QMesh; class QCamera; class QMaterial; }
namespace Qt3DExtras { class Qt3DWindow; class QPhongMaterial; }

class QLabel;
class QProgressBar;
class QStackedWidget;
class QTimer;

namespace PartManager
{

	class MeshCacheBuilder;

	class Model3DViewer : public QWidget
	{
		Q_OBJECT
	public:
		explicit Model3DViewer(QWidget* parent = nullptr);

		// The app's one STEP converter and mesh cache. Set it before showModel() for STEP files
		// to be tessellated; without it a STEP file is reported rather than converted, which is
		// the correct behaviour for a viewer with no database behind it.
		void setCacheBuilder(MeshCacheBuilder* builder);

		// Shows `absolutePath`. Returns false when the file cannot be drawn — a format this
		// build has no loader for, or a path that is not there — with the reason in the panel
		// and in outMessage. An empty path clears the viewer, which is not a failure.
		//
		// A `.step`/`.stp` file is **converted** rather than refused when a cache builder is set
		// and a converter is installed, so this returns true having started the work, not having
		// finished it. Already-converted files come straight from the cache.
		bool showModel(const QString& absolutePath, QString* outMessage = nullptr);

		// The footprint the part is placed on, drawn as a board under the model. An empty
		// drawing gives a plain board sized to the model alone, which is still the right answer:
		// it is the surface the part stands on and the thing that gives its size a scale.
		//
		// Call it before showModel() when both change — the board is rebuilt from whichever
		// arrives last, so the order only affects how many times it is built.
		void setFootprint(const KicadDrawing& footprint);

		// Puts the camera back where showModel() left it, for a "Reset view" button.
		void resetView();

	protected:
		// The camera's aspect ratio is the widget's, so a resized panel has to re-project or the
		// model comes out stretched.
		void resizeEvent(QResizeEvent* event) override;
		// Mouse handling for the embedded QWindow. Qt3D's own QOrbitCameraController is not used:
		// it pans on the right button and translates on the arrow keys, both of which move the
		// model off-centre with no way back.
		bool eventFilter(QObject* watched, QEvent* event) override;

	private:
		// Builds the scene once; showModel() only ever swaps the mesh's source afterwards, so
		// there is one Qt3D window for the widget's lifetime rather than one per model.
		void buildScene();
		// Aims the camera at the model's centre and backs it off far enough to fit the whole
		// thing, from m_bounds. Falls back to a fixed standoff for a mesh that could not be
		// measured, which is the old behaviour and is only ever right by luck.
		void frameModel();
		// Re-applies the projection for the current widget size, without moving the camera.
		void updateProjection();
		// Keeps the headlight on the camera. A light fixed in world space leaves half of every
		// orbit in shadow, which reads as a black model rather than as the far side of one.
		void syncHeadlight();
		// Puts the loaded mesh where the footprint says the model goes, and recomputes
		// m_placedBounds from that. Everything that measures the scene uses the placed box: the
		// raw one is where the file happens to have been authored, which for a good half of the
		// KiCad library is not where the part sits.
		void applyPlacement();
		// Builds the board slab and the footprint's copper and silkscreen under the model. Cheap
		// enough to redo whenever either the model or the footprint changes, and much simpler
		// than patching the entities that are already there.
		void rebuildBoard();
		// One pad: copper on the top face, and on the bottom face too with a drilled hole
		// through both when the pad is through-hole. Both materials are shared across the whole
		// footprint and belong to the board entity.
		void addPad(const KicadShape& shape, Qt3DRender::QMaterial* copper,
			Qt3DRender::QMaterial* holeMaterial);
		// One silkscreen shape, walked into segments and drawn as thin boxes on the board's top
		// face. Curves are stepped: at 0.12 mm wide the chords are shorter than the line.
		void addSilkscreen(const KicadShape& shape, Qt3DRender::QMaterial* material);
		// Raises the 3D page, after putting the window container where the layout wants it. See
		// the definition: a stacked page keeps a stale geometry while it is hidden, and a native
		// window shows that stale size for a frame when it is raised.
		void showScenePage();
		// Raises the text page. Leaves the progress strip alone — a STEP file being converted
		// shows both, an explanation and a bar.
		void showMessage(const QString& message);
		// The indeterminate strip along the bottom. Neither FreeCAD nor QMesh reports a
		// percentage, so this says *what* is happening rather than pretending to know how far.
		void showProgress(const QString& text);
		void hideProgress();
		// Points the viewer at one plain mesh file — an attached STL or OBJ, drawn in the default
		// grey because those formats carry no colour of their own.
		void loadMesh(const QString& absolutePath);
		// Points it at a converted mesh *set*: one mesh per solid, each with the colour the STEP
		// gave that solid. Falls back to loadMesh() on the first mesh if the manifest is unusable.
		void loadMeshSet(const QString& manifestPath);
		// Builds the entities for `m_meshFiles`/`m_meshColors` and starts them loading. Shared by
		// both paths above, which differ only in how many meshes there are and what colour.
		void buildModelEntities();
		// Nothing loaded in time. Says what is usually wrong rather than leaving the strip
		// spinning, which is indistinguishable from a hang.
		void onLoadTimeout();
		// The message shown when a STEP file cannot be converted, naming where PartManager looked.
		QString noConverterMessage() const;

		QStackedWidget* m_stack = nullptr;
		// The QWindow container holding the Qt3D surface — the stack's second page.
		QWidget* m_sceneContainer = nullptr;
		QLabel* m_messageLabel = nullptr;
		QWidget* m_progressStrip = nullptr;
		QLabel* m_progressLabel = nullptr;
		QProgressBar* m_progressBar = nullptr;
		Qt3DExtras::Qt3DWindow* m_window = nullptr;
		Qt3DCore::QEntity* m_root = nullptr;
		// The parent of every mesh entity. Rebuilt whole when the model changes: the solid count
		// varies per part, so reconciling would be more code than starting again.
		Qt3DCore::QEntity* m_modelEntity = nullptr;
		// One per solid. m_meshes is what the Ready/Error watch is on — the scene is shown once
		// they have all reported, or the watchdog fires.
		std::vector<Qt3DRender::QMesh*> m_meshes;
		std::vector<QString> m_meshFiles;
		std::vector<StepColor> m_meshColors;
		// Everything under the part: the slab and one entity per pad. Deleted and rebuilt as a
		// whole, so this is the only handle needed.
		Qt3DCore::QEntity* m_boardEntity = nullptr;
		// A point light that follows the camera, so the side being looked at is always the lit one.
		Qt3DCore::QEntity* m_headlightEntity = nullptr;
		Qt3DCore::QTransform* m_headlightTransform = nullptr;

		Qt3DCore::QTransform* m_modelTransform = nullptr;

		// The mesh as the file has it, and the same box after the footprint's placement has been
		// applied. Everything that measures the scene uses the placed one; the raw one is only
		// the input to that.
		MeshBounds m_bounds;
		MeshBounds m_placedBounds;
		KicadDrawing m_footprint;
		// Where the last drag was, for turning mouse travel into an orbit.
		QPoint m_lastDragPos;
		bool m_dragging = false;

		// Not owned, and outlives no viewer reliably — a QPointer so a closed database taking the
		// builder with it cannot leave a dangling one behind.
		QPointer<MeshCacheBuilder> m_builder;
		// The STEP file this viewer is waiting on a conversion for, so the builder's signals —
		// which fire for background work too — can be told apart from this viewer's own request.
		QString m_pendingStep;
		// The mesh handed to QMesh, for the watchdog's message.
		QString m_loadingMesh;
		// The model path currently drawn, so showModel() can tell "the user picked another part"
		// from "the panel was rebuilt for the same one" and skip the second. Empty whenever the
		// scene does not hold a loaded model, which includes every message state.
		QString m_shownPath;
		QTimer* m_loadWatchdog = nullptr;
	};

}
