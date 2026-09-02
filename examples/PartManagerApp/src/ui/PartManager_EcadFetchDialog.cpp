#include "ui/PartManager_EcadFetchDialog.h"
#include "widgets/PartManager_KicadPreviewWidget.h"

#include "easyeda/PartManager_EasyEdaClient.h"
#include "easyeda/PartManager_EasyEdaConverter.h"
#include "import/PartManager_EcadArchive.h"
#include "kicad/PartManager_KicadGeometry.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace PartManager
{
	namespace
	{
		// Everything but letters and digits, lowercased. A vendor names its archive
		// "LIB_74HC4051PW-Q100,11(5).zip" for the part "74HC4051PW-Q100,11" — comma, brackets and
		// a browser's duplicate-download counter all differ from the part number, and none of them
		// survives this.
		QString squashed(const QString& text)
		{
			QString out;
			out.reserve(text.size());
			for (QChar c : text)
			{
				if (c.isLetterOrNumber())
				{
					out += c.toLower();
				}
			}
			return out;
		}

		QString downloadsFolder()
		{
			const QString folder = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
			return folder.isEmpty()
				? QStandardPaths::writableLocation(QStandardPaths::HomeLocation) : folder;
		}
	}

	struct EcadFetchDialog::Private
	{
		QString mpn;
		QString manufacturer;
		QString mouserUrl;

		QLabel* status = nullptr;
		KicadPreviewWidget* symbolPreview = nullptr;
		KicadPreviewWidget* footprintPreview = nullptr;
		QLabel* skippedHeader = nullptr;
		QListWidget* skippedList = nullptr;
		QPushButton* mouserButton = nullptr;
		QPushButton* browseButton = nullptr;
		QPushButton* attachButton = nullptr;
		QLabel* watchLabel = nullptr;

		QFileSystemWatcher* watcher = nullptr;
		QTimer* poll = nullptr;
		QDateTime watchingSince;

		EasyEdaConversion conversion;
		QByteArray symbolBytes;
		QString symbolFilename;
		QByteArray footprintBytes;
		QString footprintFilename;
		QString archivePath;
	};

	EcadFetchDialog::EcadFetchDialog(const QString& mpn, const QString& manufacturer,
		const QString& mouserUrl, QWidget* parent)
		: QDialog(parent)
		, d(new Private)
	{
		d->mpn = mpn.trimmed();
		d->manufacturer = manufacturer.trimmed();
		d->mouserUrl = mouserUrl.trimmed();

		setWindowTitle(tr("Get KiCad symbol and footprint"));
		resize(720, 520);

		QVBoxLayout* layout = new QVBoxLayout(this);

		d->status = new QLabel(this);
		d->status->setWordWrap(true);
		layout->addWidget(d->status);

		QHBoxLayout* previews = new QHBoxLayout();
		d->symbolPreview = new KicadPreviewWidget(this);
		d->symbolPreview->setCaption(tr("Symbol"));
		d->symbolPreview->showMessage(tr("Looking…"));
		d->footprintPreview = new KicadPreviewWidget(this);
		d->footprintPreview->setCaption(tr("Footprint"));
		d->footprintPreview->showMessage(tr("Looking…"));
		previews->addWidget(d->symbolPreview);
		previews->addWidget(d->footprintPreview);
		layout->addLayout(previews, 1);

		// Named, not hidden: a footprint missing a copper region looks complete on screen and is
		// not, and the user is the only one who can judge whether that matters for this part.
		d->skippedHeader = new QLabel(tr("Not converted:"), this);
		d->skippedHeader->setVisible(false);
		layout->addWidget(d->skippedHeader);
		d->skippedList = new QListWidget(this);
		d->skippedList->setMaximumHeight(70);
		d->skippedList->setVisible(false);
		layout->addWidget(d->skippedList);

		d->watchLabel = new QLabel(this);
		d->watchLabel->setWordWrap(true);
		d->watchLabel->setVisible(false);
		layout->addWidget(d->watchLabel);

		QHBoxLayout* buttons = new QHBoxLayout();
		d->mouserButton = new QPushButton(tr("Open on Mouser…"), this);
		d->mouserButton->setVisible(false);
		d->browseButton = new QPushButton(tr("Choose ZIP…"), this);
		d->browseButton->setVisible(false);
		buttons->addWidget(d->mouserButton);
		buttons->addWidget(d->browseButton);
		buttons->addStretch(1);
		d->attachButton = new QPushButton(tr("Attach"), this);
		d->attachButton->setDefault(true);
		d->attachButton->setEnabled(false);
		QPushButton* cancel = new QPushButton(tr("Cancel"), this);
		buttons->addWidget(d->attachButton);
		buttons->addWidget(cancel);
		layout->addLayout(buttons);

		connect(d->mouserButton, &QPushButton::clicked, this, &EcadFetchDialog::openOnMouser);
		connect(d->browseButton, &QPushButton::clicked, this, &EcadFetchDialog::chooseArchive);
		connect(d->attachButton, &QPushButton::clicked, this, &EcadFetchDialog::accept);
		connect(cancel, &QPushButton::clicked, this, &EcadFetchDialog::reject);

		startLookup();
	}

	EcadFetchDialog::~EcadFetchDialog()
	{
		delete d;
	}

	QByteArray EcadFetchDialog::symbolBytes() const { return d->symbolBytes; }
	QString EcadFetchDialog::symbolFilename() const { return d->symbolFilename; }
	QByteArray EcadFetchDialog::footprintBytes() const { return d->footprintBytes; }
	QString EcadFetchDialog::footprintFilename() const { return d->footprintFilename; }
	QString EcadFetchDialog::archivePath() const { return d->archivePath; }

	void EcadFetchDialog::startLookup()
	{
		if (d->mpn.isEmpty())
		{
			d->status->setText(tr("This part has no manufacturer part number, so there is nothing "
				"to look up. Download the model by hand instead."));
			beginWatchingDownloads();
			return;
		}

		d->status->setText(tr("Looking up %1 on EasyEDA…").arg(d->mpn));

		// EasyEdaClient blocks (nested event loop, like MouserClient), so it runs off the GUI
		// thread — otherwise the previews above never paint until the answer is already in.
		const QPointer<EcadFetchDialog> alive(this);
		const std::string mpn = d->mpn.toStdString();
		const std::string manufacturer = d->manufacturer.toStdString();
		QThreadPool::globalInstance()->start(QRunnable::create([alive, mpn, manufacturer]()
			{
				EasyEdaClient client;
				const EasyEdaComponent component = client.lookup(mpn, manufacturer);
				EasyEdaConversion conversion = EasyEdaConverter::convert(component);
				if (!conversion.ok && conversion.errorMessage.empty())
				{
					conversion.errorMessage = component.errorMessage;
				}

				QMetaObject::invokeMethod(qApp, [alive, conversion]()
					{
						if (alive.isNull())
						{
							return;
						}
						alive->d->conversion = conversion;
						alive->showConversion();
					}, Qt::QueuedConnection);
			}));
	}

	void EcadFetchDialog::showConversion()
	{
		const EasyEdaConversion& conversion = d->conversion;
		if (!conversion.ok)
		{
			d->status->setText(tr("EasyEDA has nothing for %1. %2")
				.arg(d->mpn, QString::fromStdString(conversion.errorMessage)));
			d->symbolPreview->showMessage(tr("Not found"));
			d->footprintPreview->showMessage(tr("Not found"));
			beginWatchingDownloads();
			return;
		}

		d->symbolBytes = QByteArray::fromStdString(conversion.symbolLibraryText);
		d->symbolFilename = QString::fromStdString(conversion.symbolName) + QStringLiteral(".kicad_sym");
		d->footprintBytes = QByteArray::fromStdString(conversion.footprintText);
		d->footprintFilename = QString::fromStdString(conversion.footprintName) + QStringLiteral(".kicad_mod");

		if (!conversion.symbolLibraryText.empty())
		{
			// Drawn from the generated text rather than from the EasyEDA data, so what is on
			// screen is what will be written — a conversion bug shows up here instead of in KiCad.
			d->symbolPreview->showDrawing(
				KicadGeometry::symbol(conversion.symbolLibraryText, conversion.symbolName),
				tr("Nothing to draw"));
			d->symbolPreview->setCaption(QString::fromStdString(conversion.symbolName));
		}
		else
		{
			d->symbolPreview->showMessage(tr("No symbol"));
		}

		if (!conversion.footprintText.empty())
		{
			d->footprintPreview->showDrawing(KicadGeometry::footprint(conversion.footprintText),
				tr("Nothing to draw"));
			d->footprintPreview->setCaption(QString::fromStdString(conversion.footprintName));
		}
		else
		{
			d->footprintPreview->showMessage(tr("No footprint"));
		}

		d->status->setText(tr("EasyEDA: %1, %n pin(s), %2 pad(s). Check both drawings before "
			"attaching — the footprint decides how the part solders.", "", conversion.pinCount)
			.arg(QString::fromStdString(conversion.symbolName))
			.arg(conversion.padCount));

		if (!conversion.skipped.empty())
		{
			d->skippedHeader->setVisible(true);
			d->skippedList->setVisible(true);
			for (const std::string& entry : conversion.skipped)
			{
				d->skippedList->addItem(QString::fromStdString(entry));
			}
		}

		// The manual route stays available even on success: the user may prefer the vendor's own
		// model, and EasyEDA sometimes carries a part under a package that is close but not right.
		d->mouserButton->setVisible(!d->mouserUrl.isEmpty());
		d->browseButton->setVisible(true);
		updateButtons();
	}

	void EcadFetchDialog::beginWatchingDownloads()
	{
		d->mouserButton->setVisible(!d->mouserUrl.isEmpty());
		d->browseButton->setVisible(true);

		const QString folder = downloadsFolder();
		d->watchingSince = QDateTime::currentDateTime().addSecs(-5);
		d->watchLabel->setVisible(true);
		d->watchLabel->setText(tr("Open the part on Mouser, download its KiCad model, and it will "
			"be picked up automatically from %1.").arg(QDir::toNativeSeparators(folder)));

		if (d->watcher == nullptr)
		{
			d->watcher = new QFileSystemWatcher(this);
			d->watcher->addPath(folder);
			connect(d->watcher, &QFileSystemWatcher::directoryChanged,
				this, &EcadFetchDialog::scanDownloads);
		}
		if (d->poll == nullptr)
		{
			// The watcher alone is not enough: a browser writes the archive under a temporary
			// name and renames it, and a directory-change signal can arrive while the file is
			// still half-written and unreadable. Polling covers both — a re-scan is cheap.
			d->poll = new QTimer(this);
			d->poll->setInterval(1500);
			connect(d->poll, &QTimer::timeout, this, &EcadFetchDialog::scanDownloads);
			d->poll->start();
		}
	}

	void EcadFetchDialog::scanDownloads()
	{
		if (!d->archivePath.isEmpty())
		{
			return;
		}

		const QString wanted = squashed(d->mpn);
		if (wanted.isEmpty())
		{
			return;
		}

		QDir folder(downloadsFolder());
		const QFileInfoList entries = folder.entryInfoList(QStringList() << QStringLiteral("*.zip"),
			QDir::Files, QDir::Time);
		for (const QFileInfo& entry : entries)
		{
			// Only archives that arrived since the dialog opened, so an old download of a
			// different part sitting in the folder is never silently attached to this one.
			if (entry.lastModified() < d->watchingSince)
			{
				continue;
			}
			if (!squashed(entry.completeBaseName()).contains(wanted))
			{
				continue;
			}
			// read() is what decides whether it is really finished: a partial ZIP has no readable
			// central directory, so this fails and the next poll tries again.
			const EcadArchivePayload payload = EcadArchive::read(entry.absoluteFilePath().toStdString());
			if (!payload.contents.ok || !payload.contents.hasAnything())
			{
				continue;
			}
			showArchive(entry.absoluteFilePath());
			return;
		}
	}

	void EcadFetchDialog::showArchive(const QString& zipPath)
	{
		const EcadArchivePayload payload = EcadArchive::read(zipPath.toStdString());
		if (!payload.contents.ok || !payload.contents.hasAnything())
		{
			d->status->setText(payload.contents.legacyKicadOnly
				? tr("%1 has a KiCad folder, but only KiCad 5 files in it. Re-download it choosing "
					"KiCad 6 or later.").arg(QFileInfo(zipPath).fileName())
				: tr("%1 holds no KiCad symbol or footprint.").arg(QFileInfo(zipPath).fileName()));
			return;
		}

		if (d->poll != nullptr)
		{
			d->poll->stop();
		}
		d->archivePath = zipPath;
		// The archive wins over anything EasyEDA produced: the user went and fetched the vendor's
		// own model, which is a clearer statement of intent than a fallback we picked.
		d->symbolBytes.clear();
		d->footprintBytes.clear();

		d->symbolPreview->setCaption(QString::fromStdString(payload.symbolName));
		d->footprintPreview->setCaption(QString::fromStdString(payload.footprintName));
		if (!payload.symbolBytes.empty())
		{
			const std::string name = KicadGeometry::symbol(payload.symbolBytes, std::string()).name;
			d->symbolPreview->showDrawing(KicadGeometry::symbol(payload.symbolBytes, name),
				tr("Nothing to draw"));
		}
		else
		{
			d->symbolPreview->showMessage(tr("No symbol in the archive"));
		}
		d->footprintPreview->showDrawing(KicadGeometry::footprint(payload.footprintBytes),
			tr("No footprint in the archive"));

		QStringList found;
		if (!payload.contents.symbolEntry.empty()) { found << tr("symbol"); }
		if (!payload.contents.footprintEntry.empty()) { found << tr("footprint"); }
		if (!payload.contents.modelEntry.empty()) { found << tr("3D model"); }
		d->status->setText(tr("Found %1 in %2: %3.")
			.arg(found.join(tr(", ")), QFileInfo(zipPath).fileName())
			.arg(tr("%n other CAD tool's file(s) ignored", "", payload.contents.ignoredEntries)));
		d->watchLabel->setVisible(false);
		d->skippedHeader->setVisible(false);
		d->skippedList->setVisible(false);
		updateButtons();
	}

	void EcadFetchDialog::openOnMouser()
	{
		if (d->mouserUrl.isEmpty())
		{
			return;
		}
		// Watching starts before the browser does, so an archive that lands quickly is not missed.
		beginWatchingDownloads();
		QDesktopServices::openUrl(QUrl(d->mouserUrl));
	}

	void EcadFetchDialog::chooseArchive()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose an ECAD archive"),
			downloadsFolder(), tr("ECAD archives (*.zip);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}
		showArchive(path);
	}

	void EcadFetchDialog::updateButtons()
	{
		d->attachButton->setEnabled(!d->archivePath.isEmpty()
			|| !d->symbolBytes.isEmpty() || !d->footprintBytes.isEmpty());
	}

}
