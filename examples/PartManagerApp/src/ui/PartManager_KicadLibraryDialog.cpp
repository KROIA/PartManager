#include "ui/PartManager_KicadLibraryDialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		// The artifact a row stands for. The list is rebuilt on every run, so the target travels
		// with the item rather than being looked up by index.
		constexpr int TargetPathRole = Qt::UserRole;
	}

	KicadLibraryDialog::KicadLibraryDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_controller(handle)
	{
		setWindowTitle(tr("KiCad Libraries"));
		setSizeGripEnabled(true);
		resize(760, 560);

		QVBoxLayout* layout = new QVBoxLayout(this);

		m_pathLabel = new QLabel(this);
		m_pathLabel->setWordWrap(true);
		m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		m_pathLabel->setText(m_controller.setupInstructions());
		layout->addWidget(m_pathLabel);

		m_summary = new QPlainTextEdit(this);
		m_summary->setReadOnly(true);
		m_summary->setMaximumHeight(90);
		m_summary->setPlaceholderText(tr("Press Generate to write the libraries."));
		layout->addWidget(m_summary);

		layout->addWidget(new QLabel(tr("Edited in KiCad — left untouched:"), this));
		m_preservedList = new QListWidget(this);
		layout->addWidget(m_preservedList, 1);

		QHBoxLayout* buttons = new QHBoxLayout();
		m_generateButton = new QPushButton(tr("Generate"), this);
		m_generateButton->setDefault(true);
		m_regenerateButton = new QPushButton(tr("Regenerate It (discard my edit)"), this);
		m_keepButton = new QPushButton(tr("Keep My Version"), this);
		QPushButton* folderButton = new QPushButton(tr("Open Folder"), this);
		QPushButton* closeButton = new QPushButton(tr("Close"), this);
		buttons->addWidget(m_generateButton);
		buttons->addWidget(m_regenerateButton);
		buttons->addWidget(m_keepButton);
		buttons->addWidget(folderButton);
		buttons->addStretch(1);
		buttons->addWidget(closeButton);
		layout->addLayout(buttons);

		connect(m_generateButton, &QPushButton::clicked, this, &KicadLibraryDialog::generate);
		connect(m_regenerateButton, &QPushButton::clicked, this, &KicadLibraryDialog::regenerateSelected);
		connect(m_keepButton, &QPushButton::clicked, this, &KicadLibraryDialog::keepSelected);
		connect(folderButton, &QPushButton::clicked, this, &KicadLibraryDialog::openFolder);
		connect(closeButton, &QPushButton::clicked, this, &KicadLibraryDialog::accept);
		connect(m_preservedList, &QListWidget::itemSelectionChanged,
			this, &KicadLibraryDialog::updateButtons);

		updateButtons();
	}

	void KicadLibraryDialog::generate()
	{
		showResult(m_controller.generate());
	}

	void KicadLibraryDialog::showResult(const KicadGenerationResult& result)
	{
		QStringList lines;
		lines.append(QString::fromStdString(result.summary()));
		if (!result.skippedForNoCategory.empty())
		{
			// Named, not just counted: without a KiCad category a part simply never appears in
			// KiCad, and there is nothing on screen to explain why.
			QStringList names;
			for (const std::string& name : result.skippedForNoCategory)
			{
				names.append(QString::fromStdString(name));
			}
			lines.append(tr("No KiCad category, so not generated: %1")
				.arg(names.join(QStringLiteral(", "))));
		}
		m_summary->setPlainText(lines.join(QStringLiteral("\n")));

		m_preserved = result.preserved;
		m_preservedList->clear();
		for (const KicadSkippedItem& item : m_preserved)
		{
			QListWidgetItem* row = new QListWidgetItem(
				QString::fromStdString(item.targetPath), m_preservedList);
			row->setData(TargetPathRole, QString::fromStdString(item.targetPath));
			row->setToolTip(tr("Your edit is still in the file. Regenerating discards it; "
				"keeping it stops this warning without changing anything."));
		}
		if (m_preserved.empty() && result.ok)
		{
			m_preservedList->addItem(tr("Nothing — no hand-edited artifacts."));
			m_preservedList->item(0)->setFlags(Qt::NoItemFlags);
		}
		updateButtons();
	}

	void KicadLibraryDialog::updateButtons()
	{
		QListWidgetItem* item = m_preservedList->currentItem();
		const bool real = item != nullptr && !item->data(TargetPathRole).toString().isEmpty();
		m_regenerateButton->setEnabled(real);
		m_keepButton->setEnabled(real);
	}

	void KicadLibraryDialog::regenerateSelected()
	{
		QListWidgetItem* item = m_preservedList->currentItem();
		if (item == nullptr)
		{
			return;
		}
		const QString target = item->data(TargetPathRole).toString();
		if (target.isEmpty())
		{
			return;
		}
		// There is no undo for this, and the edit is the user's own work.
		if (QMessageBox::question(this, tr("Discard your edit?"),
			tr("“%1” will be overwritten with PartManager's generated version. "
			   "Your changes to it are lost.").arg(target),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		showResult(m_controller.generate({ target.toStdString() }));
	}

	void KicadLibraryDialog::keepSelected()
	{
		QListWidgetItem* item = m_preservedList->currentItem();
		if (item == nullptr)
		{
			return;
		}
		const QString target = item->data(TargetPathRole).toString();
		if (target.isEmpty())
		{
			return;
		}
		// Re-baseline: the file is not touched at all, only what PartManager considers the
		// version it last wrote. From here the artifact regenerates normally again.
		if (!m_controller.rebaseline(target.toStdString()))
		{
			QMessageBox::warning(this, tr("Could not keep that version"),
				tr("“%1” could not be read back from disk.").arg(target));
			return;
		}
		showResult(m_controller.generate());
	}

	void KicadLibraryDialog::openFolder()
	{
		const std::string path = m_controller.libraryPath();
		if (path.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(path)));
	}

}
