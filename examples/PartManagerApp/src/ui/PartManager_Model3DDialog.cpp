#include "ui/PartManager_Model3DDialog.h"

#include "model3d/PartManager_Model3DFormat.h"
#include "widgets/PartManager_Model3DViewer.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace PartManager
{

	Model3DDialog::Model3DDialog(DatabaseHandle* handle, int partId, const QString& partName,
		QWidget* parent)
		: QDialog(parent)
		, m_controller(handle)
		, m_partId(partId)
	{
		// The part name is the user's own data, so only the frame around it is translated.
		setWindowTitle(tr("3D Model — %1").arg(partName));
		setSizeGripEnabled(true);
		resize(700, 560);

		QVBoxLayout* layout = new QVBoxLayout(this);

		m_viewer = new Model3DViewer(this);
		layout->addWidget(m_viewer, 1);

		m_fileLabel = new QLabel(this);
		m_fileLabel->setWordWrap(true);
		layout->addWidget(m_fileLabel);

		QHBoxLayout* buttons = new QHBoxLayout();
		m_attachButton = new QPushButton(tr("Attach Model…"), this);
		m_detachButton = new QPushButton(tr("Remove"), this);
		m_openButton = new QPushButton(tr("Open Externally"), this);
		QPushButton* resetButton = new QPushButton(tr("Reset View"), this);
		QPushButton* closeButton = new QPushButton(tr("Close"), this);
		closeButton->setDefault(true);
		buttons->addWidget(m_attachButton);
		buttons->addWidget(m_detachButton);
		buttons->addWidget(m_openButton);
		buttons->addWidget(resetButton);
		buttons->addStretch(1);
		buttons->addWidget(closeButton);
		layout->addLayout(buttons);

		connect(m_attachButton, &QPushButton::clicked, this, &Model3DDialog::attach);
		connect(m_detachButton, &QPushButton::clicked, this, &Model3DDialog::detach);
		connect(m_openButton, &QPushButton::clicked, this, &Model3DDialog::openInSystemViewer);
		connect(resetButton, &QPushButton::clicked, m_viewer, &Model3DViewer::resetView);
		connect(closeButton, &QPushButton::clicked, this, &Model3DDialog::accept);

		reload();
	}

	void Model3DDialog::reload()
	{
		const QString path = QString::fromStdString(m_controller.model3DPath(m_partId));

		PartFile file;
		const bool hasRow = m_controller.model3DFile(m_partId, file);
		m_detachButton->setEnabled(hasRow);
		m_openButton->setEnabled(!path.isEmpty());

		if (!hasRow)
		{
			m_fileLabel->setText(tr("No 3D model attached."));
			m_viewer->showModel(QString());
			return;
		}
		if (path.isEmpty())
		{
			// The row survives but the bytes are gone — a filestore folder that was moved or
			// cleaned. Saying which file is missing is the only actionable part.
			m_fileLabel->setText(tr("“%1” is recorded but its file is missing from the filestore.")
				.arg(QString::fromStdString(file.originalFilename)));
			m_viewer->showModel(QString());
			return;
		}

		const Model3DFormat format = model3DFormatOf(file.originalFilename);
		m_fileLabel->setText(tr("%1  ·  %2  ·  %3 KB")
			.arg(QString::fromStdString(file.originalFilename))
			.arg(QString::fromStdString(model3DFormatName(format)))
			.arg(file.sizeBytes / 1024));
		m_viewer->showModel(path);
	}

	void Model3DDialog::attach()
	{
		// One list for the filter and the validation, so the dialog cannot offer a format the
		// store would then reject.
		QStringList patterns;
		for (const std::string& extension : model3DExtensions())
		{
			patterns.append(QStringLiteral("*.") + QString::fromStdString(extension));
		}
		const QString filter = tr("3D models (%1);;All files (*)").arg(patterns.join(QChar(' ')));

		const QString path = QFileDialog::getOpenFileName(this, tr("Attach a 3D model"),
			QString(), filter);
		if (path.isEmpty())
		{
			return;
		}

		if (!isModel3D(model3DFormatOf(path.toStdString()))
			&& QMessageBox::question(this, tr("Not a recognised 3D format"),
				tr("“%1” is not a file extension PartManager recognises as a 3D model. "
				   "Attach it anyway?").arg(QFileInfo(path).fileName()),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		std::string error;
		if (m_controller.attachModel3D(m_partId, path.toStdString(), &error) == 0)
		{
			QMessageBox::warning(this, tr("Could not attach the model"),
				QString::fromStdString(error));
			return;
		}
		reload();
	}

	void Model3DDialog::detach()
	{
		PartFile file;
		if (!m_controller.model3DFile(m_partId, file))
		{
			return;
		}
		if (QMessageBox::question(this, tr("Remove this model?"),
			tr("“%1” will no longer be attached to this part. The file is only deleted from the "
			   "filestore if no other part uses it.")
				.arg(QString::fromStdString(file.originalFilename)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		m_controller.detachModel3D(m_partId);
		reload();
	}

	void Model3DDialog::openInSystemViewer()
	{
		const std::string path = m_controller.model3DPath(m_partId);
		if (path.empty())
		{
			return;
		}
		// The way to actually look at a STEP file until there is a kernel here: hand it to
		// whatever the user already has associated with it.
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
	}

}
