#include "widgets/PartManager_Model3DViewer.h"

#include <Qt3DCore/QEntity>
#include <Qt3DCore/QTransform>
#include <Qt3DExtras/QCuboidMesh>
#include <Qt3DExtras/QCylinderMesh>
#include <Qt3DExtras/QForwardRenderer>
#include <Qt3DExtras/QPhongMaterial>
#include <Qt3DExtras/Qt3DWindow>
#include <Qt3DRender/QCamera>
#include <Qt3DRender/QDirectionalLight>
#include <Qt3DRender/QGeometryRenderer>
#include <Qt3DRender/QMaterial>
#include <Qt3DRender/QMesh>
#include <Qt3DRender/QPointLight>

#include "model3d/PartManager_MeshBounds.h"
#include "model3d/PartManager_StepColors.h"
#include "model3d/PartManager_StepConverter.h"
#include "services/PartManager_MeshCacheBuilder.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QQuaternion>
#include <QVector3D>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace PartManager
{
	namespace
	{
		enum Page
		{
			PageMessage = 0,
			PageScene
		};

		// A light studio background. The old near-black one was half the reason a loaded model
		// looked like nothing at all: with no light in the scene a QPhongMaterial renders close
		// to its ambient, and dark grey on dark grey is invisible.
		const QColor SceneClearColor(0xE4, 0xE8, 0xED);
		// A component body is grey plastic; the ambient term is what keeps the unlit side
		// readable against a light background rather than going to black.
		const QColor MeshColor(0x9A, 0xA2, 0xAA);
		const QColor MeshAmbient(0x4E, 0x54, 0x5A);
		const QColor MeshSpecular(0x30, 0x30, 0x30);

		// KiCad's own board colours, so the preview reads as the same board the user will see.
		const QColor BoardColor(0x1F, 0x5B, 0x2E);
		const QColor BoardAmbient(0x0C, 0x24, 0x12);
		const QColor PadColor(0xC8, 0x8A, 0x33);
		const QColor PadAmbient(0x50, 0x36, 0x14);
		const QColor SilkColor(0xF0, 0xF0, 0xEA);
		const QColor SilkAmbient(0x60, 0x60, 0x5C);
		// A drilled hole seen from outside is a dark cylinder, not a black one — the barrel is
		// plated and catches some light.
		const QColor HoleColor(0x1A, 0x18, 0x16);

		// 1.6 mm is the industry-standard board and every fab's default, so this is the real
		// thickness rather than one picked to look right. It matters more than it looks: a
		// through-hole lead has to visibly pass through *this* distance.
		constexpr float BoardThicknessMm = 1.6f;
		// Copper plus finish is about 35 um; silkscreen ink is thinner still. Both are rounded up
		// to something that will not z-fight with the slab at any zoom this panel offers.
		constexpr float PadThicknessMm = 0.06f;
		constexpr float SilkThicknessMm = 0.03f;
		// How much board to show around everything on it. Enough to read as a board rather than
		// as a tile cut to the part.
		constexpr double BoardMarginMm = 1.0;

		// How long a mesh may take to appear before the panel says something is wrong. Qt3D reads
		// an STL of a few hundred kilobytes in well under a second; ten is far past "slow disk"
		// and into "this is never going to finish", which is worth naming rather than spinning at.
		constexpr int LoadTimeoutMs = 10000;

		// The vertical field of view the projection uses. Also what the fit distance is derived
		// from, so the two cannot drift apart.
		constexpr float FieldOfViewDegrees = 45.0f;

		// Where the camera sits relative to the part before the user turns it: above and to one
		// side, so the board is visibly a board and the part visibly stands on it. Normalised on
		// use, so these are a direction and not a distance.
		const QVector3D DefaultEyeDirection(0.62f, -0.62f, 0.48f);
		// Z is up: KiCad's board plane is XY and its models stand in +Z.
		const QVector3D WorldUp(0.0f, 0.0f, 1.0f);

		// Degrees of orbit per pixel dragged, and the zoom step per wheel notch.
		constexpr float OrbitDegreesPerPixel = 0.35f;
		constexpr float ZoomPerNotch = 0.88f;
		// A dolly camera can be pushed inside the model or out to the horizon; both leave the
		// user with an empty panel and no obvious way back. Bounds are in model radii.
		constexpr float MinDistanceInRadii = 1.2f;
		constexpr float MaxDistanceInRadii = 40.0f;

		// What is actually printed on the board. Courtyard and fab layers are drafting aids that
		// exist only in the editor, so they have no business on a render of the physical thing.
		bool isSilkscreen(const std::string& layer)
		{
			return layer.find("SilkS") != std::string::npos;
		}

		// The half-angle actually available, which is the vertical one on a wide panel and the
		// horizontal one on a tall, narrow one. Fitting to the wrong one crops the model on the
		// axis nobody checked.
		float fittingHalfAngle(float aspect)
		{
			const float vertical = qDegreesToRadians(FieldOfViewDegrees) / 2.0f;
			if (aspect >= 1.0f)
			{
				return vertical;
			}
			return std::atan(std::tan(vertical) * std::max(aspect, 0.05f));
		}
	}

	Model3DViewer::Model3DViewer(QWidget* parent)
		: QWidget(parent)
	{
		QVBoxLayout* layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(2);

		m_stack = new QStackedWidget(this);
		m_messageLabel = new QLabel(this);
		m_messageLabel->setAlignment(Qt::AlignCenter);
		m_messageLabel->setWordWrap(true);
		m_stack->addWidget(m_messageLabel);
		layout->addWidget(m_stack, 1);

		// The strip sits under the stack rather than over the scene, so it is equally visible
		// whether the 3D page or the text page is up — conversion shows both.
		m_progressStrip = new QWidget(this);
		QHBoxLayout* progressLayout = new QHBoxLayout(m_progressStrip);
		progressLayout->setContentsMargins(4, 0, 4, 2);
		m_progressLabel = new QLabel(m_progressStrip);
		m_progressBar = new QProgressBar(m_progressStrip);
		// 0..0 is Qt's busy indicator. Neither FreeCAD's tessellation nor QMesh's loader reports
		// a percentage, and a fake one that jumps to 100% and waits is worse than none.
		m_progressBar->setRange(0, 0);
		m_progressBar->setTextVisible(false);
		m_progressBar->setMaximumHeight(10);
		m_progressBar->setMaximumWidth(140);
		progressLayout->addWidget(m_progressLabel, 1);
		progressLayout->addWidget(m_progressBar, 0);
		m_progressStrip->hide();
		layout->addWidget(m_progressStrip, 0);

		m_loadWatchdog = new QTimer(this);
		m_loadWatchdog->setSingleShot(true);
		connect(m_loadWatchdog, &QTimer::timeout, this, &Model3DViewer::onLoadTimeout);

		buildScene();
		showMessage(tr("Select a part with a 3D model."));
	}

	void Model3DViewer::buildScene()
	{
		m_window = new Qt3DExtras::Qt3DWindow();
		m_window->defaultFrameGraph()->setClearColor(SceneClearColor);

		m_root = new Qt3DCore::QEntity();

		// Only the placement transform lives here for the widget's lifetime; the meshes
		// themselves are children rebuilt per model, because a STEP is one entity per solid and
		// the solid count changes with the part.
		m_modelEntity = new Qt3DCore::QEntity(m_root);
		m_modelTransform = new Qt3DCore::QTransform(m_modelEntity);
		m_modelEntity->addComponent(m_modelTransform);

		// Qt3DWindow builds a camera and a frame graph but no light at all, so a QPhongMaterial
		// in a fresh scene renders at its ambient term and nothing else. Two lights: one fixed
		// from above, which is what makes the board read as flat and the part as standing on it,
		// and one on the camera so the side being looked at is never the dark one.
		Qt3DCore::QEntity* skyEntity = new Qt3DCore::QEntity(m_root);
		Qt3DRender::QDirectionalLight* sky = new Qt3DRender::QDirectionalLight(skyEntity);
		sky->setWorldDirection(QVector3D(-0.3f, 0.4f, -1.0f));
		sky->setColor(QColor(0xFF, 0xFF, 0xF6));
		sky->setIntensity(0.55f);
		skyEntity->addComponent(sky);

		m_headlightEntity = new Qt3DCore::QEntity(m_root);
		Qt3DRender::QPointLight* headlight = new Qt3DRender::QPointLight(m_headlightEntity);
		headlight->setColor(QColor(0xFF, 0xFF, 0xFF));
		headlight->setIntensity(0.75f);
		// No falloff: the camera distance changes with every zoom, and a light that dimmed with
		// it would make zooming out look like turning the lights off.
		headlight->setConstantAttenuation(1.0f);
		headlight->setLinearAttenuation(0.0f);
		headlight->setQuadraticAttenuation(0.0f);
		m_headlightTransform = new Qt3DCore::QTransform(m_headlightEntity);
		m_headlightEntity->addComponent(headlight);
		m_headlightEntity->addComponent(m_headlightTransform);

		m_window->setRootEntity(m_root);
		// Rotation only. QOrbitCameraController would also translate on the right button and on
		// the arrow keys, which walks the part out of frame with nothing to bring it back — on a
		// panel this size that is a preview the user has to repair rather than read. Driving the
		// camera from the QWindow's own mouse events is both less code and exactly the two
		// gestures wanted.
		m_window->installEventFilter(this);

		QWidget* container = QWidget::createWindowContainer(m_window, m_stack);
		container->setMinimumSize(120, 100);
		m_stack->addWidget(container);
	}

	void Model3DViewer::setCacheBuilder(MeshCacheBuilder* builder)
	{
		m_builder = builder;
		if (builder == nullptr)
		{
			return;
		}
		// The builder converts in the background as well, so both signals fire for models this
		// viewer never asked about. m_pendingStep is what tells them apart.
		connect(builder, &MeshCacheBuilder::meshReady, this,
			[this](const QString& stepPath, const QString& meshPath)
			{
				if (!m_pendingStep.isEmpty() && stepPath == m_pendingStep)
				{
					// What the builder produces is a mesh set, not a single mesh.
					loadMeshSet(meshPath);
				}
			});
		connect(builder, &MeshCacheBuilder::meshFailed, this,
			[this](const QString& stepPath, const QString& reason)
			{
				if (m_pendingStep.isEmpty() || stepPath != m_pendingStep)
				{
					return;
				}
				m_pendingStep.clear();
				hideProgress();
				showMessage(tr("The STEP model could not be converted.\n\n%1").arg(reason));
			});
	}

	void Model3DViewer::showMessage(const QString& message)
	{
		m_messageLabel->setText(message);
		m_stack->setCurrentIndex(PageMessage);
	}

	void Model3DViewer::showProgress(const QString& text)
	{
		m_progressLabel->setText(text);
		m_progressStrip->show();
	}

	void Model3DViewer::hideProgress()
	{
		m_progressStrip->hide();
	}

	bool Model3DViewer::showModel(const QString& absolutePath, QString* outMessage)
	{
		auto report = [outMessage](const QString& text)
		{
			if (outMessage != nullptr)
			{
				*outMessage = text;
			}
		};

		m_pendingStep.clear();
		m_loadWatchdog->stop();
		hideProgress();
		// Whatever comes next, the board under the *previous* part is no longer the right one.
		m_bounds = MeshBounds();
		rebuildBoard();

		if (absolutePath.isEmpty())
		{
			// Clearing is not a failure — it is what selecting a part without a model means.
			showMessage(tr("This part has no 3D model attached."));
			report(QString());
			return false;
		}

		const QFileInfo info(absolutePath);
		if (!info.isFile())
		{
			const QString message = tr("The model file is no longer on disk:\n%1").arg(absolutePath);
			showMessage(message);
			report(message);
			return false;
		}

		const Model3DFormat format = model3DFormatOf(absolutePath.toStdString());
		if (isRenderableModel3D(format))
		{
			loadMesh(absolutePath);
			report(QString());
			return true;
		}

		if (format == Model3DFormat::Step && !m_builder.isNull())
		{
			// Converted once, drawn every time after — and usually converted already, by the
			// background sweep, before the user ever selects the part.
			const QString cached = m_builder->meshFor(absolutePath);
			if (!cached.isEmpty())
			{
				// A converted STEP is a *set* — one mesh per solid, each in the colour the STEP
				// gave that solid.
				loadMeshSet(cached);
				report(QString());
				return true;
			}
			if (MeshCacheBuilder::converterAvailable())
			{
				m_pendingStep = absolutePath;
				showMessage(tr("Converting %1 to a mesh…\nThis happens once per model.")
					.arg(info.fileName()));
				showProgress(tr("Converting…"));
				m_builder->prioritise(absolutePath);
				report(QString());
				return true;
			}
			const QString message = noConverterMessage();
			showMessage(message);
			report(message);
			return false;
		}

		// Recognised, stored, exported — just not something this build can turn into triangles.
		// Naming the format and why is the difference between "broken" and "not yet".
		const QString name = QString::fromStdString(model3DFormatName(format));
		const QString message = isModel3D(format)
			? tr("%1 files are stored and used for KiCad export, but cannot be shown here — "
				 "%1 describes surfaces rather than triangles and needs a CAD kernel to draw.\n\n"
				 "Attach an OBJ, STL, PLY or glTF version to see it in 3D.").arg(name)
			: tr("That file is not a 3D model this build recognises.");
		showMessage(message);
		report(message);
		return false;
	}

	void Model3DViewer::loadMesh(const QString& absolutePath)
	{
		// STL, OBJ and the rest carry no colour, so there is one mesh and it is the default grey.
		m_meshFiles = { absolutePath };
		m_meshColors = { StepColor() };
		buildModelEntities();
	}

	void Model3DViewer::loadMeshSet(const QString& manifestPath)
	{
		QFile file(manifestPath);
		std::vector<StepConverter::MeshSetEntry> entries;
		if (file.open(QIODevice::ReadOnly))
		{
			entries = StepConverter::parseMeshSet(
				QString::fromUtf8(file.readAll()).toStdString());
			file.close();
		}
		if (entries.empty())
		{
			// A manifest that will not parse is a cache entry from a build that wrote a different
			// format. Nothing to draw from it, and the converter will rewrite it on the next
			// sweep; saying so beats an empty scene.
			showMessage(tr("The converted mesh for this model could not be read."));
			return;
		}

		const QString folder = QFileInfo(manifestPath).path();
		m_meshFiles.clear();
		m_meshColors.clear();
		for (const StepConverter::MeshSetEntry& entry : entries)
		{
			// Filenames are relative to the manifest, so the cache folder can be moved or copied.
			m_meshFiles.push_back(folder + QChar('/') + QString::fromStdString(entry.file));
			m_meshColors.push_back(entry.color);
		}
		buildModelEntities();
	}

	void Model3DViewer::buildModelEntities()
	{
		m_pendingStep.clear();
		if (m_meshFiles.empty())
		{
			return;
		}

		// Measured from the files, before Qt3D is handed them. Qt3D's own extents are not
		// populated until its bounding-volume job has run, a frame or two after Ready, so framing
		// from those means framing an empty box on the one frame the user is looking at.
		m_bounds = MeshBounds();
		for (const QString& file : m_meshFiles)
		{
			const MeshBounds part = meshBoundsOf(file.toStdString());
			if (!part.ok) { continue; }
			if (!m_bounds.ok)
			{
				m_bounds = part;
				continue;
			}
			m_bounds.minX = std::min(m_bounds.minX, part.minX);
			m_bounds.maxX = std::max(m_bounds.maxX, part.maxX);
			m_bounds.minY = std::min(m_bounds.minY, part.minY);
			m_bounds.maxY = std::max(m_bounds.maxY, part.maxY);
			m_bounds.minZ = std::min(m_bounds.minZ, part.minZ);
			m_bounds.maxZ = std::max(m_bounds.maxZ, part.maxZ);
		}
		rebuildBoard();

		// Every mesh entity goes and is built again. The solid count changes with the part, so
		// reconciling two lists would be more code than starting from nothing.
		m_meshes.clear();
		// A copy of the list, not the list itself: children() hands back the live one, and
		// deleting through it while iterating walks off the end of a container being modified.
		// The placement QTransform is a child object too but not an entity, which is what the
		// cast is guarding — it has to survive.
		const QObjectList children = m_modelEntity->children();
		for (QObject* child : children)
		{
			if (qobject_cast<Qt3DCore::QEntity*>(child) != nullptr)
			{
				delete child;
			}
		}

		m_loadingMesh = m_meshFiles.front();
		std::shared_ptr<int> outstanding = std::make_shared<int>(
			static_cast<int>(m_meshFiles.size()));
		for (size_t i = 0; i < m_meshFiles.size(); ++i)
		{
			Qt3DCore::QEntity* entity = new Qt3DCore::QEntity(m_modelEntity);
			Qt3DRender::QMesh* mesh = new Qt3DRender::QMesh(entity);
			mesh->setSource(QUrl::fromLocalFile(m_meshFiles[i]));

			const StepColor& color = m_meshColors[i];
			const QColor diffuse = QColor::fromRgbF(color.r, color.g, color.b);
			Qt3DExtras::QPhongMaterial* material = new Qt3DExtras::QPhongMaterial(entity);
			material->setDiffuse(diffuse);
			// The ambient term is a darkened diffuse rather than a fixed grey, so the unlit side
			// of a black moulding stays black and the unlit side of a lead stays metal.
			material->setAmbient(diffuse.darker(260));
			// Metal is shiny, moulded plastic is not, and lightness is the only cue available —
			// a STEP gives a colour and nothing else about the material.
			const bool metallic = color.luminance() > 0.55;
			material->setSpecular(metallic ? QColor(0xE0, 0xE4, 0xE8) : MeshSpecular);
			material->setShininess(metallic ? 70.0f : 14.0f);

			entity->addComponent(mesh);
			entity->addComponent(material);
			m_meshes.push_back(mesh);

			// The scene is raised once every mesh has reported, so a part is never shown with
			// half its solids missing.
			connect(mesh, &Qt3DRender::QMesh::statusChanged, this,
				[this, outstanding](Qt3DRender::QMesh::Status status)
				{
					if (status != Qt3DRender::QMesh::Ready
						&& status != Qt3DRender::QMesh::Error)
					{
						return;
					}
					if (*outstanding <= 0) { return; }
					if (--*outstanding > 0) { return; }
					m_loadWatchdog->stop();
					hideProgress();
					if (m_bounds.ok || status == Qt3DRender::QMesh::Ready)
					{
						frameModel();
						m_stack->setCurrentIndex(PageScene);
					}
					else
					{
						// The extension was one we claim to load, but this file did not parse.
						showMessage(tr("This model could not be loaded — the file may be damaged "
							"or use a variant of the format Qt does not read."));
					}
				});
		}

		// The 3D page goes up *now*, before the meshes are ready. Qt3D runs its job graph from
		// the render loop and the render loop only runs while the window is exposed, so a
		// container left on an unselected page never loads anything and the panel waits for a
		// Ready that cannot arrive. This is the difference between a viewer and a hang.
		m_stack->setCurrentIndex(PageScene);
		showProgress(tr("Loading %1…").arg(QFileInfo(m_loadingMesh).fileName()));
		m_loadWatchdog->start(LoadTimeoutMs);
	}

	void Model3DViewer::onLoadTimeout()
	{
		// Any solid having arrived means the loader works and the rest are merely slow; the
		// message below would be wrong and alarming.
		for (Qt3DRender::QMesh* mesh : m_meshes)
		{
			if (mesh->status() == Qt3DRender::QMesh::Ready)
			{
				return;
			}
		}
		hideProgress();
		// Naming the plugin is the actionable part: an app deployed without Qt3D's geometry
		// loaders shows exactly this, and nothing else in the window would ever say why.
		showMessage(tr("“%1” did not load.\n\nThe file is on disk, so this is usually a missing "
			"Qt3D geometry loader — check that the “geometryloaders” folder sits next to the "
			"executable.").arg(QFileInfo(m_loadingMesh).fileName()));
	}

	QString Model3DViewer::noConverterMessage() const
	{
		QStringList locations;
		for (const std::string& location : StepConverter::searchedLocations())
		{
			locations.append(QString::fromStdString(location));
		}
		// Saying *where* it looked is the actionable part — "no converter found" alone leaves
		// the user with nowhere to start, and the env-var override is the escape hatch.
		return tr("This STEP model is stored and will be used for KiCad export, but drawing it "
			"needs a CAD kernel to tessellate it first.\n\n"
			"Install FreeCAD and reopen this window, or attach an STL/OBJ version instead.\n\n"
			"Looked for a converter in:\n%1").arg(locations.join(QStringLiteral("\n")));
	}

	void Model3DViewer::updateProjection()
	{
		if (m_window == nullptr)
		{
			return;   // a layout-driven resize can land before buildScene() has run
		}
		const float aspect = static_cast<float>(width())
			/ static_cast<float>(height() > 0 ? height() : 1);
		// The near plane scales with the model: 0.1 mm is fine for an SOT-23 and wastes most of
		// the depth buffer on a connector, and a fixed one large enough for the connector would
		// clip the SOT-23 away entirely.
		const float radius = m_placedBounds.ok ? static_cast<float>(m_placedBounds.radius()) : 10.0f;
		m_window->camera()->lens()->setPerspectiveProjection(FieldOfViewDegrees, aspect,
			radius * 0.01f, radius * 400.0f);
	}

	void Model3DViewer::frameModel()
	{
		Qt3DRender::QCamera* camera = m_window->camera();
		updateProjection();

		// The centre of the model itself, not the origin. A KiCad model's origin is its
		// footprint's origin, which for an asymmetric package is off to one side — orbiting
		// about it swings the part around the panel instead of turning it on the spot.
		const QVector3D centre = m_placedBounds.ok
			? QVector3D(static_cast<float>(m_placedBounds.centreX()),
				static_cast<float>(m_placedBounds.centreY()),
				static_cast<float>(m_placedBounds.centreZ()))
			: QVector3D(0.0f, 0.0f, 0.0f);

		// What has to fit is the whole scene, not just the model: the board reaches wider than a
		// small part, and a through-hole part's leads reach below it. Measured as the furthest
		// corner of that box from the view centre, which is exact and needs no cases.
		double radius = m_placedBounds.ok ? m_placedBounds.radius() : 10.0;
		if (m_placedBounds.ok)
		{
			double reachX = std::max(std::abs(m_placedBounds.minX), std::abs(m_placedBounds.maxX));
			double reachY = std::max(std::abs(m_placedBounds.minY), std::abs(m_placedBounds.maxY));
			KicadPoint footMin, footMax;
			if (m_footprint.bounds(footMin, footMax))
			{
				reachX = std::max(reachX, std::max(std::abs(footMin.x), std::abs(footMax.x)));
				reachY = std::max(reachY, std::max(std::abs(footMin.y), std::abs(footMax.y)));
			}
			reachX += BoardMarginMm;
			reachY += BoardMarginMm;
			// The slab hangs to -1.6; a THT model's leads may hang further.
			const double lowZ = std::min<double>(m_placedBounds.minZ, -BoardThicknessMm);
			const double highZ = std::max(0.0, m_placedBounds.maxZ);

			radius = 0.0;
			for (const double cornerX : { -reachX, reachX })
			{
				for (const double cornerY : { -reachY, reachY })
				{
					for (const double cornerZ : { lowZ, highZ })
					{
						radius = std::max(radius, std::sqrt(
							std::pow(cornerX - centre.x(), 2.0)
							+ std::pow(cornerY - centre.y(), 2.0)
							+ std::pow(cornerZ - centre.z(), 2.0)));
					}
				}
			}
		}

		// Trigonometry rather than a guessed standoff: a sphere of this radius exactly fills the
		// field of view at this distance, and the margin is what keeps it off the edges. The old
		// fixed 40 mm put the camera sixteen part-widths away from an SOT-23.
		const float distance = static_cast<float>(radius)
			/ std::sin(fittingHalfAngle(static_cast<float>(width())
				/ static_cast<float>(height() > 0 ? height() : 1))) * 1.25f;

		camera->setUpVector(WorldUp);
		camera->setViewCenter(centre);
		camera->setPosition(centre + DefaultEyeDirection.normalized() * distance);
		syncHeadlight();
	}

	void Model3DViewer::syncHeadlight()
	{
		if (m_headlightTransform != nullptr)
		{
			m_headlightTransform->setTranslation(m_window->camera()->position());
		}
	}

	void Model3DViewer::resizeEvent(QResizeEvent* event)
	{
		QWidget::resizeEvent(event);
		updateProjection();
	}

	bool Model3DViewer::eventFilter(QObject* watched, QEvent* event)
	{
		if (watched != m_window)
		{
			return QWidget::eventFilter(watched, event);
		}

		Qt3DRender::QCamera* camera = m_window->camera();
		switch (event->type())
		{
		case QEvent::MouseButtonPress:
		{
			QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
			if (mouse->button() == Qt::LeftButton)
			{
				m_dragging = true;
				m_lastDragPos = mouse->pos();
			}
			return false;
		}
		case QEvent::MouseButtonRelease:
			m_dragging = false;
			return false;
		case QEvent::MouseMove:
		{
			QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
			if (!m_dragging || (mouse->buttons() & Qt::LeftButton) == 0)
			{
				return false;
			}
			const QPoint delta = mouse->pos() - m_lastDragPos;
			m_lastDragPos = mouse->pos();

			// Yaw about the world's up axis rather than the camera's, so repeated orbits cannot
			// accumulate roll and leave the board tilted on its side.
			camera->panAboutViewCenter(-delta.x() * OrbitDegreesPerPixel, WorldUp);

			// Dragging down tips the board's top away from the viewer, as though the part were
			// being pushed away across the desk. Qt3D's tilt sign gives the opposite, which
			// reads as the vertical axis being inverted.
			const float tilt = delta.y() * OrbitDegreesPerPixel;
			if (!qFuzzyIsNull(tilt))
			{
				// Applied and then checked rather than predicted: whether a positive tilt raises
				// or lowers the camera is Qt3D's convention to define, and a clamp that assumes
				// the wrong one locks the orbit at one end instead of at both.
				camera->tiltAboutViewCenter(tilt);
				const QVector3D after =
					(camera->position() - camera->viewCenter()).normalized();
				if (std::abs(QVector3D::dotProduct(after, WorldUp)) > 0.985f)
				{
					// Straight over the pole flips the image left-to-right in one frame, which
					// the user reads as the model jumping. Put it back instead.
					camera->tiltAboutViewCenter(-tilt);
				}
			}
			syncHeadlight();
			return true;
		}
		case QEvent::Wheel:
		{
			QWheelEvent* wheel = static_cast<QWheelEvent*>(event);
			const float notches = wheel->angleDelta().y() / 120.0f;
			if (qFuzzyIsNull(notches))
			{
				return false;
			}
			QVector3D toCamera = camera->position() - camera->viewCenter();
			const float radius = m_placedBounds.ok ? static_cast<float>(m_placedBounds.radius()) : 10.0f;
			const float distance = std::clamp(
				toCamera.length() * std::pow(ZoomPerNotch, notches),
				radius * MinDistanceInRadii, radius * MaxDistanceInRadii);
			camera->setPosition(camera->viewCenter() + toCamera.normalized() * distance);
			syncHeadlight();
			return true;
		}
		default:
			break;
		}
		return QWidget::eventFilter(watched, event);
	}

	void Model3DViewer::setFootprint(const KicadDrawing& footprint)
	{
		m_footprint = footprint;
		rebuildBoard();
	}

	void Model3DViewer::applyPlacement()
	{
		const KicadModelPlacement& placement = m_footprint.model3D;

		// Scale, then rotate, then translate — Qt3D's order and KiCad's, so a footprint that
		// offsets a rotated model lands the same way in both.
		const QVector3D scale(static_cast<float>(placement.scaleX),
			static_cast<float>(placement.scaleY), static_cast<float>(placement.scaleZ));
		const QQuaternion rotation = QQuaternion::fromEulerAngles(
			static_cast<float>(placement.rotateX), static_cast<float>(placement.rotateY),
			static_cast<float>(placement.rotateZ));
		// Y flips for the same reason the pads' does: the offset is written in the footprint's
		// coordinates, which measure Y downward, and this scene is the board's, which does not.
		const QVector3D translation(static_cast<float>(placement.offsetX),
			static_cast<float>(-placement.offsetY), static_cast<float>(placement.offsetZ));

		if (m_modelTransform != nullptr)
		{
			m_modelTransform->setScale3D(scale);
			m_modelTransform->setRotation(rotation);
			m_modelTransform->setTranslation(translation);
		}

		m_placedBounds = MeshBounds();
		if (!m_bounds.ok)
		{
			return;
		}
		// The eight corners through the same transform, re-boxed. A rotated model's upright box
		// is not its old one turned, so anything simpler would size the board wrong the moment a
		// footprint uses a rotation.
		bool first = true;
		for (const double x : { m_bounds.minX, m_bounds.maxX })
		{
			for (const double y : { m_bounds.minY, m_bounds.maxY })
			{
				for (const double z : { m_bounds.minZ, m_bounds.maxZ })
				{
					const QVector3D corner = rotation.rotatedVector(QVector3D(
						static_cast<float>(x) * scale.x(),
						static_cast<float>(y) * scale.y(),
						static_cast<float>(z) * scale.z())) + translation;
					if (first)
					{
						m_placedBounds.ok = true;
						m_placedBounds.minX = m_placedBounds.maxX = corner.x();
						m_placedBounds.minY = m_placedBounds.maxY = corner.y();
						m_placedBounds.minZ = m_placedBounds.maxZ = corner.z();
						first = false;
						continue;
					}
					m_placedBounds.minX = std::min<double>(m_placedBounds.minX, corner.x());
					m_placedBounds.maxX = std::max<double>(m_placedBounds.maxX, corner.x());
					m_placedBounds.minY = std::min<double>(m_placedBounds.minY, corner.y());
					m_placedBounds.maxY = std::max<double>(m_placedBounds.maxY, corner.y());
					m_placedBounds.minZ = std::min<double>(m_placedBounds.minZ, corner.z());
					m_placedBounds.maxZ = std::max<double>(m_placedBounds.maxZ, corner.z());
				}
			}
		}
	}

	void Model3DViewer::rebuildBoard()
	{
		if (m_root == nullptr)
		{
			return;
		}
		// Before anything measures the model: the placement is what decides where it actually is.
		applyPlacement();
		// Rebuilt whole rather than patched: the pad count changes with every part, and deleting
		// one parent is less code than reconciling two lists.
		delete m_boardEntity;
		m_boardEntity = nullptr;

		// Nothing loaded means nothing to stand on. A board on its own would look like a part
		// that failed to draw.
		if (!m_placedBounds.ok)
		{
			return;
		}

		// How far the board has to reach: the part's own outline, the footprint's, and a margin.
		double reachX = std::max(std::abs(m_placedBounds.minX), std::abs(m_placedBounds.maxX));
		double reachY = std::max(std::abs(m_placedBounds.minY), std::abs(m_placedBounds.maxY));
		KicadPoint footMin, footMax;
		const bool hasFootprint = m_footprint.bounds(footMin, footMax);
		if (hasFootprint)
		{
			reachX = std::max(reachX, std::max(std::abs(footMin.x), std::abs(footMax.x)));
			reachY = std::max(reachY, std::max(std::abs(footMin.y), std::abs(footMax.y)));
		}
		const float boardX = static_cast<float>((reachX + BoardMarginMm) * 2.0);
		const float boardY = static_cast<float>((reachY + BoardMarginMm) * 2.0);

		// The board's top surface is z = 0. Not "the lowest point of the model" — that would be
		// right for a surface-mount part and exactly wrong for a through-hole one, whose leads
		// reach *below* the board by design and would drag the whole slab down to the lead tips.
		// z = 0 is KiCad's contract for where a footprint sits, and a model that does not honour
		// it should visibly intersect the board rather than have the board quietly moved to suit.
		constexpr float SurfaceZ = 0.0f;

		m_boardEntity = new Qt3DCore::QEntity(m_root);

		Qt3DCore::QEntity* slab = new Qt3DCore::QEntity(m_boardEntity);
		Qt3DExtras::QCuboidMesh* slabMesh = new Qt3DExtras::QCuboidMesh(slab);
		slabMesh->setXExtent(boardX);
		slabMesh->setYExtent(boardY);
		slabMesh->setZExtent(BoardThicknessMm);
		Qt3DExtras::QPhongMaterial* slabMaterial = new Qt3DExtras::QPhongMaterial(slab);
		slabMaterial->setDiffuse(BoardColor);
		slabMaterial->setAmbient(BoardAmbient);
		slabMaterial->setSpecular(QColor(0x20, 0x30, 0x24));
		slabMaterial->setShininess(8.0f);
		Qt3DCore::QTransform* slabTransform = new Qt3DCore::QTransform(slab);
		// A cuboid is centred on its origin, so the slab hangs below the surface.
		slabTransform->setTranslation(QVector3D(0.0f, 0.0f, SurfaceZ - BoardThicknessMm / 2.0f));
		slab->addComponent(slabMesh);
		slab->addComponent(slabMaterial);
		slab->addComponent(slabTransform);

		if (!hasFootprint)
		{
			return;
		}

		// One material each for copper, silkscreen and drilled holes — a 64-pin package is 64
		// entities either way, but only one set of uniforms per kind.
		Qt3DExtras::QPhongMaterial* padMaterial =
			new Qt3DExtras::QPhongMaterial(m_boardEntity);
		padMaterial->setDiffuse(PadColor);
		padMaterial->setAmbient(PadAmbient);
		padMaterial->setSpecular(QColor(0xFF, 0xE8, 0xB0));
		padMaterial->setShininess(48.0f);

		Qt3DExtras::QPhongMaterial* silkMaterial =
			new Qt3DExtras::QPhongMaterial(m_boardEntity);
		silkMaterial->setDiffuse(SilkColor);
		silkMaterial->setAmbient(SilkAmbient);
		silkMaterial->setSpecular(QColor(0x40, 0x40, 0x40));
		silkMaterial->setShininess(6.0f);

		Qt3DExtras::QPhongMaterial* holeMaterial =
			new Qt3DExtras::QPhongMaterial(m_boardEntity);
		holeMaterial->setDiffuse(HoleColor);
		holeMaterial->setAmbient(QColor(0x08, 0x08, 0x08));
		holeMaterial->setSpecular(QColor(0x00, 0x00, 0x00));

		for (const KicadShape& shape : m_footprint.shapes)
		{
			if (shape.kind == KicadShapeKind::Pad)
			{
				addPad(shape, padMaterial, holeMaterial);
			}
			else if (isSilkscreen(shape.layer))
			{
				addSilkscreen(shape, silkMaterial);
			}
			// Courtyard and fab lines are deliberately left out: they are not on the physical
			// board, and drawing them here would make the preview disagree with what arrives
			// from the fab about what is actually printed.
		}
	}

	void Model3DViewer::addPad(const KicadShape& shape, Qt3DRender::QMaterial* copper,
		Qt3DRender::QMaterial* holeMaterial)
	{
		if (shape.points.empty() || shape.sizeX <= 0.0 || shape.sizeY <= 0.0)
		{
			return;
		}

		// A footprint file measures Y downward and the board frame the model lives in measures
		// it upward, so the sign flips here. Without it the pads mirror and a non-symmetric
		// package sits on top of nothing.
		const float x = static_cast<float>(shape.points[0].x);
		const float y = static_cast<float>(-shape.points[0].y);

		// A through-hole pad has copper on *both* faces. Drawing only the top one puts a THT
		// part's leads on a board they are supposed to pass through, which is the single thing
		// that makes a DIP look like an SMD in a preview.
		const float faces[2] = { PadThicknessMm / 2.0f,
			-BoardThicknessMm - PadThicknessMm / 2.0f };
		const int faceCount = shape.throughHole ? 2 : 1;
		for (int face = 0; face < faceCount; ++face)
		{
			Qt3DCore::QEntity* pad = new Qt3DCore::QEntity(m_boardEntity);
			Qt3DExtras::QCuboidMesh* padMesh = new Qt3DExtras::QCuboidMesh(pad);
			padMesh->setXExtent(static_cast<float>(shape.sizeX));
			padMesh->setYExtent(static_cast<float>(shape.sizeY));
			padMesh->setZExtent(PadThicknessMm);
			Qt3DCore::QTransform* padTransform = new Qt3DCore::QTransform(pad);
			// This frame already has Y up, so KiCad's anticlockwise angle is applied as-is —
			// unlike the 2D preview, which draws in the file's Y-down coordinates.
			padTransform->setRotationZ(static_cast<float>(shape.rotationDegrees));
			padTransform->setTranslation(QVector3D(x, y, faces[face]));
			pad->addComponent(padMesh);
			pad->addComponent(copper);
			pad->addComponent(padTransform);
		}

		if (!shape.throughHole || shape.drillDiameter <= 0.0)
		{
			return;
		}

		// The hole, as a dark cylinder right through the board and a hair past both pads. Not a
		// real hole — that would need the slab cut, which is CSG this scene has no use for —
		// but it is what makes a THT pad read as drilled rather than as a copper blob.
		Qt3DCore::QEntity* hole = new Qt3DCore::QEntity(m_boardEntity);
		Qt3DExtras::QCylinderMesh* holeMesh = new Qt3DExtras::QCylinderMesh(hole);
		holeMesh->setRadius(static_cast<float>(shape.drillDiameter) / 2.0f);
		holeMesh->setLength(BoardThicknessMm + 3.0f * PadThicknessMm);
		holeMesh->setSlices(20);
		holeMesh->setRings(2);
		Qt3DCore::QTransform* holeTransform = new Qt3DCore::QTransform(hole);
		// QCylinderMesh runs along Y; a quarter turn about X puts it along Z, which is the way
		// through a board that lies in XY.
		holeTransform->setRotationX(90.0f);
		holeTransform->setTranslation(QVector3D(x, y, -BoardThicknessMm / 2.0f));
		hole->addComponent(holeMesh);
		hole->addComponent(holeMaterial);
		hole->addComponent(holeTransform);
	}

	void Model3DViewer::addSilkscreen(const KicadShape& shape, Qt3DRender::QMaterial* material)
	{
		// Every silkscreen shape becomes a run of points and then one thin box per segment. An
		// arc walked in steps rather than handed to a renderer that understands curves: at
		// 0.12 mm wide and a couple of millimetres long, the chords are shorter than the line
		// is thick.
		std::vector<KicadPoint> path;
		switch (shape.kind)
		{
		case KicadShapeKind::Polyline:
			path = shape.points;
			if (shape.closed && path.size() > 2) { path.push_back(path.front()); }
			break;
		case KicadShapeKind::Rectangle:
		{
			if (shape.points.size() < 2) { return; }
			const KicadPoint a = shape.points[0];
			const KicadPoint b = shape.points[1];
			path = { a, { b.x, a.y }, b, { a.x, b.y }, a };
			break;
		}
		case KicadShapeKind::Circle:
		{
			if (shape.points.empty() || shape.radius <= 0.0) { return; }
			const int steps = std::clamp(static_cast<int>(shape.radius * 12.0), 12, 48);
			for (int i = 0; i <= steps; ++i)
			{
				const double angle = 2.0 * M_PI * i / steps;
				path.push_back({ shape.points[0].x + shape.radius * std::cos(angle),
					shape.points[0].y + shape.radius * std::sin(angle) });
			}
			break;
		}
		case KicadShapeKind::Arc:
		{
			if (shape.points.size() < 3) { return; }
			KicadPoint centre;
			double radius = 0.0, start = 0.0, span = 0.0;
			if (!KicadGeometry::arcCircle(shape.points[0], shape.points[1], shape.points[2],
				centre, radius, start, span))
			{
				// Collinear: no circle, so the "arc" is the straight run through the middle.
				path = { shape.points[0], shape.points[2] };
				break;
			}
			const int steps = std::clamp(
				static_cast<int>(std::abs(span) * radius * 8.0), 4, 48);
			for (int i = 0; i <= steps; ++i)
			{
				const double angle = start + span * i / steps;
				path.push_back({ centre.x + radius * std::cos(angle),
					centre.y + radius * std::sin(angle) });
			}
			break;
		}
		default:
			return;
		}

		// KiCad's own default when a file leaves the width off.
		const float width = static_cast<float>(shape.strokeWidth > 0.0
			? shape.strokeWidth : 0.12);
		for (size_t i = 1; i < path.size(); ++i)
		{
			// Y flips for the same reason the pads' does.
			const double dx = path[i].x - path[i - 1].x;
			const double dy = -(path[i].y - path[i - 1].y);
			const double length = std::hypot(dx, dy);
			if (length < 1e-6) { continue; }

			Qt3DCore::QEntity* segment = new Qt3DCore::QEntity(m_boardEntity);
			Qt3DExtras::QCuboidMesh* mesh = new Qt3DExtras::QCuboidMesh(segment);
			// Overlapped by half a width at each end so the corners of a closed outline join
			// instead of showing a notch.
			mesh->setXExtent(static_cast<float>(length) + width);
			mesh->setYExtent(width);
			mesh->setZExtent(SilkThicknessMm);
			Qt3DCore::QTransform* transform = new Qt3DCore::QTransform(segment);
			transform->setRotationZ(static_cast<float>(qRadiansToDegrees(std::atan2(dy, dx))));
			transform->setTranslation(QVector3D(
				static_cast<float>((path[i].x + path[i - 1].x) / 2.0),
				static_cast<float>(-(path[i].y + path[i - 1].y) / 2.0),
				SilkThicknessMm / 2.0f));
			segment->addComponent(mesh);
			segment->addComponent(material);
			segment->addComponent(transform);
		}
	}

	void Model3DViewer::resetView()
	{
		frameModel();
	}

}
