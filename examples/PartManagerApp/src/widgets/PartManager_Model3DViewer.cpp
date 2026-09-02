#include "widgets/PartManager_Model3DViewer.h"

#include <Qt3DCore/QEntity>
#include <Qt3DCore/QTransform>
#include <Qt3DExtras/QForwardRenderer>
#include <Qt3DExtras/QOrbitCameraController>
#include <Qt3DExtras/QPhongMaterial>
#include <Qt3DExtras/Qt3DWindow>
#include <Qt3DRender/QCamera>
#include <Qt3DRender/QGeometryRenderer>
#include <Qt3DRender/QMesh>

#include <QColor>
#include <QFileInfo>
#include <QLabel>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector3D>

namespace PartManager
{
	namespace
	{
		enum Page
		{
			PageMessage = 0,
			PageScene
		};

		// A neutral mid-grey background reads well against both light and dark themes, and
		// against the mesh colour below.
		const QColor SceneClearColor(0x3A, 0x3F, 0x44);
		const QColor MeshColor(0xB0, 0xB8, 0xC0);
	}

	Model3DViewer::Model3DViewer(QWidget* parent)
		: QWidget(parent)
	{
		QVBoxLayout* layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);

		m_stack = new QStackedWidget(this);
		m_messageLabel = new QLabel(this);
		m_messageLabel->setAlignment(Qt::AlignCenter);
		m_messageLabel->setWordWrap(true);
		m_stack->addWidget(m_messageLabel);
		layout->addWidget(m_stack);

		buildScene();
		showMessage(tr("Select a part with a 3D model."));
	}

	void Model3DViewer::buildScene()
	{
		m_window = new Qt3DExtras::Qt3DWindow();
		m_window->defaultFrameGraph()->setClearColor(SceneClearColor);

		m_root = new Qt3DCore::QEntity();

		m_modelEntity = new Qt3DCore::QEntity(m_root);
		m_mesh = new Qt3DRender::QMesh(m_modelEntity);
		Qt3DExtras::QPhongMaterial* material = new Qt3DExtras::QPhongMaterial(m_modelEntity);
		material->setDiffuse(MeshColor);
		m_modelEntity->addComponent(m_mesh);
		m_modelEntity->addComponent(material);

		// QMesh loads on a worker thread, so the bounding volume is empty until it reports
		// Ready. Framing at showModel() time would aim the camera at nothing, every time.
		connect(m_mesh, &Qt3DRender::QMesh::statusChanged, this,
			[this](Qt3DRender::QMesh::Status status)
			{
				if (status == Qt3DRender::QMesh::Ready)
				{
					frameModel();
					m_stack->setCurrentIndex(PageScene);
				}
				else if (status == Qt3DRender::QMesh::Error)
				{
					// The extension was one we claim to load, but this particular file did not
					// parse. Saying so beats an empty scene.
					showMessage(tr("This model could not be loaded — the file may be damaged or "
						"use a variant of the format Qt does not read."));
				}
			});

		m_cameraController = new Qt3DExtras::QOrbitCameraController(m_root);
		m_cameraController->setCamera(m_window->camera());
		m_cameraController->setLinearSpeed(50.0f);
		m_cameraController->setLookSpeed(180.0f);

		m_window->setRootEntity(m_root);

		QWidget* container = QWidget::createWindowContainer(m_window, m_stack);
		container->setMinimumSize(200, 160);
		m_stack->addWidget(container);
	}

	void Model3DViewer::showMessage(const QString& message)
	{
		m_messageLabel->setText(message);
		m_stack->setCurrentIndex(PageMessage);
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
		if (!isRenderableModel3D(format))
		{
			// A STEP file is stored, kept and exported perfectly well; it just is not triangles.
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

		m_mesh->setSource(QUrl::fromLocalFile(absolutePath));
		showMessage(tr("Loading %1…").arg(info.fileName()));
		report(QString());
		return true;
	}

	void Model3DViewer::frameModel()
	{
		Qt3DRender::QCamera* camera = m_window->camera();

		// Qt3D exposes no bounding volume on QMesh in 5.15, so the frame is a fixed, generous
		// standoff rather than a computed one. Every model the app deals with is a component
		// footprint at roughly the same scale, so one distance genuinely does fit them; the
		// orbit controller is there for anything that does not.
		// ponytail: fixed standoff instead of a bounding-sphere fit. Upgrade path is reading
		// QGeometry's position attribute min/max once a model turns up that this frames badly.
		constexpr float Standoff = 40.0f;
		camera->lens()->setPerspectiveProjection(45.0f,
			static_cast<float>(width()) / static_cast<float>(height() > 0 ? height() : 1),
			0.1f, 10000.0f);
		camera->setPosition(QVector3D(Standoff, Standoff * 0.6f, Standoff));
		camera->setUpVector(QVector3D(0.0f, 1.0f, 0.0f));
		camera->setViewCenter(QVector3D(0.0f, 0.0f, 0.0f));
	}

	void Model3DViewer::resetView()
	{
		frameModel();
	}

}
