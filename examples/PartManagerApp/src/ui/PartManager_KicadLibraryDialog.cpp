#include "ui/PartManager_KicadLibraryDialog.h"

#include "kicad/PartManager_KicadLibTable.h"

#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
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
		// Whether the part behind the row is gone from generation entirely — a different decision
		// for the user, so the buttons and the wording have to know.
		constexpr int StaleRole = Qt::UserRole + 1;

		// Part names, which are the user's own data and so are never translated.
		QString joinNames(const std::vector<std::string>& names)
		{
			QStringList list;
			for (const std::string& name : names)
			{
				list.append(QString::fromStdString(name));
			}
			return list.join(QStringLiteral(", "));
		}
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

		// The generated nicknames, spelled out. They are what KiCad asks for in every "add a
		// library" dialog, and before this the screen only ever said how many there were.
		m_librariesLabel = new QLabel(this);
		m_librariesLabel->setWordWrap(true);
		m_librariesLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(m_librariesLabel);

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
		m_installButton = new QPushButton(tr("Install in KiCad..."), this);
		m_installButton->setToolTip(tr("Adds the libraries to KiCad's own library tables, so they "
			"appear in the symbol and footprint choosers without typing anything in."));
		QPushButton* folderButton = new QPushButton(tr("Open Folder"), this);
		QPushButton* closeButton = new QPushButton(tr("Close"), this);
		buttons->addWidget(m_generateButton);
		buttons->addWidget(m_installButton);
		buttons->addWidget(m_regenerateButton);
		buttons->addWidget(m_keepButton);
		buttons->addWidget(folderButton);
		buttons->addStretch(1);
		buttons->addWidget(closeButton);
		layout->addLayout(buttons);

		connect(m_generateButton, &QPushButton::clicked, this, &KicadLibraryDialog::generate);
		connect(m_regenerateButton, &QPushButton::clicked, this, &KicadLibraryDialog::regenerateSelected);
		connect(m_keepButton, &QPushButton::clicked, this, &KicadLibraryDialog::keepSelected);
		connect(m_installButton, &QPushButton::clicked, this, &KicadLibraryDialog::install);
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
			lines.append(tr("No KiCad category, so not generated: %1")
				.arg(joinNames(result.skippedForNoCategory)));
		}
		// Named, not counted, and for the same reason: a user expecting 38 symbols and getting 12
		// has to be told which 26 went and that it was deliberate, not a failure.
		if (!result.skippedForNoKicadFiles.empty())
		{
			lines.append(tr("No KiCad symbol or footprint attached, so not generated at all: %1")
				.arg(joinNames(result.skippedForNoKicadFiles)));
		}
		if (!result.skippedForNoSymbol.empty())
		{
			// Both halves of the reason, because they need different fixes: attach a symbol, or
			// give the footprint's pads numbers.
			lines.append(tr("Footprint generated but no symbol: none is attached, and the "
				"footprint's pads carry no pin numbers to derive one from: %1")
				.arg(joinNames(result.skippedForNoSymbol)));
		}
		m_summary->setPlainText(lines.join(QStringLiteral("\n")));

		m_preserved = result.preserved;
		m_preservedList->clear();
		for (const KicadSkippedItem& item : m_preserved)
		{
			QListWidgetItem* row = new QListWidgetItem(
				QString::fromStdString(item.targetPath), m_preservedList);
			row->setData(TargetPathRole, QString::fromStdString(item.targetPath));
			row->setData(StaleRole, item.stale);
			row->setToolTip(item.stale
				? tr("The part behind this no longer has a KiCad symbol or footprint attached, so "
					"PartManager does not generate it any more. Your edit is kept because it is "
					"yours; removing it is the only thing left to decide.")
				: tr("Your edit is still in the file. Regenerating discards it; "
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
		// A stale artifact is already kept by doing nothing, so "Keep My Version" would do nothing
		// visible while re-baselining it — and the *next* run would then delete it as ours.
		const bool stale = real && item->data(StaleRole).toBool();
		m_regenerateButton->setEnabled(real);
		m_regenerateButton->setText(stale
			? tr("Remove It") : tr("Regenerate It (discard my edit)"));
		m_keepButton->setEnabled(real && !stale);

		// Installing reads the libraries off disk, so it works on a database generated in an
		// earlier session — the button does not wait for a Generate in this one.
		const QStringList names = m_controller.lastLibraryNames();
		m_installButton->setEnabled(!names.isEmpty());
		// The nicknames, not the file names — what KiCad's chooser shows and what the user has
		// to search for there.
		QStringList nicknames;
		for (const QString& name : names)
		{
			nicknames.append(QString::fromStdString(KicadLibTable::nicknameFor(name.toStdString())));
		}
		m_librariesLabel->setText(nicknames.isEmpty()
			? tr("No libraries generated yet.")
			: tr("In KiCad they are called: %1").arg(nicknames.join(QStringLiteral(", "))));
	}

	QString KicadLibraryDialog::chooseInstallTarget(bool& outIsGlobal)
	{
		outIsGlobal = true;
		QMessageBox choice(this);
		choice.setWindowTitle(tr("Install in KiCad"));
		choice.setText(tr("Where should the libraries be available?"));
		choice.setInformativeText(tr(
			"Globally — in every KiCad project on this machine.\n"
			"One project — only in the project you pick; nothing outside it changes.\n\n"
			"Close KiCad first: it rewrites its library tables when it exits and would "
			"overwrite this."));
		QPushButton* globalButton = choice.addButton(tr("Globally"), QMessageBox::AcceptRole);
		QPushButton* projectButton = choice.addButton(tr("One Project..."), QMessageBox::AcceptRole);
		choice.addButton(QMessageBox::Cancel);
		choice.exec();

		if (choice.clickedButton() == projectButton)
		{
			outIsGlobal = false;
			// The project *file* is what the user recognises; KiCad wants the folder it sits in.
			const QString project = QFileDialog::getOpenFileName(this, tr("Pick a KiCad project"),
				QString(), tr("KiCad project (*.kicad_pro *.pro)"));
			return project.isEmpty() ? QString() : QFileInfo(project).absolutePath();
		}
		if (choice.clickedButton() != globalButton)
		{
			return QString();
		}

		const QStringList configDirs = KicadController::kicadConfigDirs();
		if (configDirs.isEmpty())
		{
			// Not an error worth blocking on: a portable KiCad, or one whose settings live
			// somewhere unusual, is a folder the user can point at.
			return QFileDialog::getExistingDirectory(this, tr("KiCad settings folder"));
		}
		if (configDirs.size() == 1)
		{
			return configDirs.first();
		}
		bool confirmed = false;
		const QString picked = QInputDialog::getItem(this, tr("Which KiCad?"),
			tr("Settings folder:"), configDirs, 0, false, &confirmed);
		return confirmed ? picked : QString();
	}

	void KicadLibraryDialog::install()
	{
		bool isGlobal = true;
		const QString target = chooseInstallTarget(isGlobal);
		if (target.isEmpty())
		{
			return;
		}

		// Naming the files before touching them: these are KiCad's, not ours, and the user's
		// other libraries live in the same two files.
		if (QMessageBox::question(this, tr("Install in KiCad"),
			tr("sym-lib-table and fp-lib-table in\n%1\nwill get one entry per PartManager library. "
			   "Everything else in them is kept, and a .bak copy is made first.\n\nContinue?")
			.arg(target), QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
		{
			return;
		}

		QString error;
		if (!m_controller.install(target, true, &error))
		{
			QMessageBox::warning(this, tr("Could not install"), error);
			return;
		}
		QMessageBox::information(this, tr("Installed"),
			isGlobal
			? tr("The libraries are in KiCad's global tables and pinned, so they sit at the top of "
				 "the symbol and footprint choosers under \"%1\".")
				.arg(QString::fromLatin1(KicadLibTable::NicknamePrefix))
			: tr("The libraries are in that project's tables and pinned, so they sit at the top of "
				 "the choosers under \"%1\" when the project is open.")
				.arg(QString::fromLatin1(KicadLibTable::NicknamePrefix)));
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
		const bool stale = item->data(StaleRole).toBool();
		if (QMessageBox::question(this, tr("Discard your edit?"), stale
			? tr("“%1” will be removed from the library. Its part has no KiCad symbol or footprint "
				 "attached any more, so PartManager will not put it back.").arg(target)
			: tr("“%1” will be overwritten with PartManager's generated version. "
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
