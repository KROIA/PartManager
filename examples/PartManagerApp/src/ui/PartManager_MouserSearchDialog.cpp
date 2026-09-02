#include "ui/PartManager_MouserSearchDialog.h"
#include "ui_PartManager_MouserSearchDialog.h"

#include "controllers/PartManager_MainWindowController.h"
#include "easyeda/PartManager_EasyEdaClient.h"
#include "filestore/PartManager_FileStore.h"
#include "widgets/PartManager_AttachmentIconPainter.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QPointer>
#include <QRunnable>
#include <QTableWidgetItem>
#include <QThreadPool>
#include <QUrl>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// One screenful. The spec caps a keyword response at 50, and a search that needs more
		// than 25 rows to find the part wants a better query, not more paging UI.
		// ponytail: no paging. startingRecord is already plumbed through MouserClient if it is
		// ever wanted; only the Next/Previous chrome would be missing.
		constexpr int KeywordRecords = 25;

		// Row picture height. Mouser's `lrg` variant is around 800 px wide, so it is scaled down
		// hard — but it is the same URL the part will be given on "Use this part", so the bytes
		// pulled for the list are not a second, throwaway fetch.
		constexpr int ThumbnailSize = 48;

		// At most this many product photos are pulled at once. The pool would otherwise open one
		// connection per core against Mouser's CDN for a single keystroke, which is a burst they
		// have no reason to tolerate and the user cannot see the benefit of anyway.
		constexpr int ThumbnailThreads = 4;

		// A thumbnail is decoration, so it may not hold the dialog's close for its full 15 s.
		constexpr int ThumbnailTimeoutMs = 8000;

		// Which picture a row shows, kept on the item so a late download can find its rows again
		// without consulting m_results — which by then may describe a different search entirely.
		constexpr int RoleImageUrl = Qt::UserRole + 1;
		// Same idea for the EasyEDA answer, which arrives even later than the picture.
		constexpr int RoleMpn = Qt::UserRole + 2;
		constexpr int RoleHasDatasheet = Qt::UserRole + 3;

		// The file glyphs are shown before the part exists, so they say what the import *will*
		// bring rather than what is attached — which is exactly the question at this point.
		// Matches the part list's glyphs, and the 52 px row here has room to spare for them.
		constexpr int FileGlyphSize = 18;

		// Mouser's own column order, minus everything the search step cannot act on.
		enum Column
		{
			ColumnThumbnail = 0,
			ColumnFiles,
			ColumnMouserNumber,
			ColumnMpn,
			ColumnManufacturer,
			ColumnDescription,
			ColumnCategory,
			ColumnStock,
			ColumnPrice,
			ColumnCount
		};

		// The cheapest quantity break, which is the one a single-part lookup is asking about.
		// Price is a currency-formatted string in the spec, so it is shown, never computed with.
		QString firstPrice(const MouserPartDto& dto)
		{
			if (dto.priceBreaks.empty())
			{
				return QString();
			}
			const MouserPriceBreak& first = dto.priceBreaks.front();
			QString price = toQt(first.price).trimmed();
			const QString currency = toQt(first.currency).trimmed();
			// Mouser already formats Price with the currency in it ("0.21 CHF"), so appending
			// Currency unconditionally gives "0.21 CHF CHF".
			if (!currency.isEmpty() && !price.contains(currency))
			{
				price += QLatin1Char(' ') + currency;
			}
			return price;
		}

		// One pool for every search dialog ever opened, deliberately not a member owned by the
		// dialog: QThreadPool's destructor calls waitForDone(), so a pool that died with the
		// dialog would hold its close until the last download timed out.
		QThreadPool& thumbnailPool()
		{
			static QThreadPool pool;
			pool.setMaxThreadCount(ThumbnailThreads);
			return pool;
		}
	}

	MouserSearchDialog::MouserSearchDialog(QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::MouserSearchDialog)
		, m_thumbnailPool(&thumbnailPool())
	{
		m_ui->setupUi(this);

		m_ui->resultsTable->setColumnCount(ColumnCount);
		m_ui->resultsTable->setHorizontalHeaderLabels(QStringList()
			<< QString() << tr("Files") << tr("Mouser #") << tr("MPN") << tr("Manufacturer")
			<< tr("Description") << tr("Category") << tr("In stock") << tr("Price"));
		m_ui->resultsTable->setIconSize(QSize(ThumbnailSize, ThumbnailSize));
		m_ui->resultsTable->verticalHeader()->setDefaultSectionSize(ThumbnailSize + 4);
		m_ui->resultsTable->horizontalHeader()->setStretchLastSection(false);
		m_ui->resultsTable->horizontalHeader()->setSectionResizeMode(ColumnThumbnail, QHeaderView::Fixed);
		m_ui->resultsTable->setColumnWidth(ColumnThumbnail, ThumbnailSize + 8);
		m_ui->resultsTable->horizontalHeader()->setSectionResizeMode(ColumnDescription, QHeaderView::Stretch);

		connect(m_ui->searchButton, &QPushButton::clicked, this, &MouserSearchDialog::search);
		connect(m_ui->searchEdit, &QLineEdit::returnPressed, this, &MouserSearchDialog::search);
		connect(m_ui->resultsTable, &QTableWidget::itemSelectionChanged, this, &MouserSearchDialog::updateButtons);
		connect(m_ui->resultsTable, &QTableWidget::itemDoubleClicked, this, &MouserSearchDialog::useSelected);
		connect(m_ui->useButton, &QPushButton::clicked, this, &MouserSearchDialog::useSelected);
		connect(m_ui->openButton, &QPushButton::clicked, this, &MouserSearchDialog::openOnMouser);
		connect(m_ui->cancelButton, &QPushButton::clicked, this, &MouserSearchDialog::reject);

		if (!MouserClient::hasApiKey())
		{
			// Nothing here works without the key, and failing per-search would just repeat the
			// same message. The env var name is safe to print; the key itself is never touched.
			m_ui->searchEdit->setEnabled(false);
			m_ui->searchButton->setEnabled(false);
			m_ui->statusLabel->setText(tr("No Mouser API key. Set the %1 environment variable and "
				"restart the app.").arg(QLatin1String(MouserClient::ApiKeyEnvVar)));
			return;
		}
		m_ui->statusLabel->setText(tr("Enter a Mouser or manufacturer part number, paste a Mouser "
			"product link, or type a keyword."));
	}

	MouserSearchDialog::~MouserSearchDialog()
	{
		// Drops everything still queued. The at most ThumbnailThreads downloads already running
		// are left to finish on their own — they touch nothing but their own local buffer, and
		// their result is posted back through a QPointer that is null by then. Waiting for them
		// instead would hold the dialog's close for up to the download timeout.
		m_thumbnailPool->clear();
		delete m_ui;
	}

	const MouserPartPrefill& MouserSearchDialog::selectedPrefill() const
	{
		return m_prefill;
	}

	void MouserSearchDialog::searchFor(const QString& query)
	{
		m_ui->searchEdit->setText(query);
		if (MouserClient::hasApiKey())
		{
			search();
		}
	}

	void MouserSearchDialog::search()
	{
		const QString query = m_ui->searchEdit->text().trimmed();
		if (query.isEmpty())
		{
			return;
		}

		m_ui->statusLabel->setText(tr("Searching Mouser…"));
		m_ui->searchButton->setEnabled(false);
		QApplication::setOverrideCursor(Qt::WaitCursor);
		// Repaint before the blocking call, or the status line above never reaches the screen.
		QApplication::processEvents();

		// A pasted product-page link is the shape most of the user's own stock list is in, so it
		// is accepted in the same box rather than behind a second one (§6).
		std::string needle = query.toStdString();
		const std::string fromUrl = MouserSearchService::partNumberFromUrl(needle);
		if (!fromUrl.empty())
		{
			needle = fromUrl;
		}

		bool byKeyword = false;
		MouserSearchResult result = m_client.searchByPartNumber(needle);
		if (result.ok && result.parts.empty())
		{
			// Not a part number then. The keyword endpoint is the only other thing to try, and
			// trying it beats making the user pick the right endpoint before they know the answer.
			result = m_client.searchByKeyword(needle, KeywordRecords);
			byKeyword = true;
		}

		QApplication::restoreOverrideCursor();
		m_ui->searchButton->setEnabled(true);

		if (!result.ok)
		{
			m_results.clear();
			showResults();
			m_ui->statusLabel->setText(tr("Search failed: %1").arg(toQt(result.errorMessage)));
			return;
		}

		m_results = result.parts;
		// rankByMatch scores part numbers; on a keyword query nothing scores and it degrades to
		// a stable no-op, leaving Mouser's own relevance order — which is the right order there.
		MouserSearchService::rankByMatch(m_results, needle);
		showResults();

		if (m_results.empty())
		{
			m_ui->statusLabel->setText(tr("Mouser has nothing for “%1”.").arg(query));
			return;
		}
		const int shown = static_cast<int>(m_results.size());
		if (result.numberOfResults > shown)
		{
			m_ui->statusLabel->setText(tr("Showing %1 of %2 keyword matches, most relevant first.")
				.arg(shown).arg(result.numberOfResults));
		}
		else
		{
			m_ui->statusLabel->setText(byKeyword
				? tr("%n keyword match(es), most relevant first.", "", shown)
				: tr("%n part-number match(es), closest first.", "", shown));
		}
	}

	void MouserSearchDialog::showResults()
	{
		m_ui->resultsTable->clearContents();
		m_ui->resultsTable->setRowCount(static_cast<int>(m_results.size()));

		for (size_t i = 0; i < m_results.size(); ++i)
		{
			const MouserPartDto& dto = m_results[i];
			const int row = static_cast<int>(i);
			// Mouser's own data, so no tr() — only the app's chrome is translated.
			const QString cells[ColumnCount] = {
				QString(),   // the picture column carries no text
				QString(),   // nor the file-glyph one
				toQt(dto.mouserPartNumber),
				toQt(dto.manufacturerPartNumber),
				toQt(dto.manufacturer),
				toQt(dto.description),
				toQt(dto.category),
				toQt(dto.availabilityInStock.empty() ? dto.availability : dto.availabilityInStock),
				firstPrice(dto),
			};
			for (int column = 0; column < ColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setToolTip(cells[column]);
				m_ui->resultsTable->setItem(row, column, item);
			}

			// The same URL "Use this part" would hand to the download step, so the picture the
			// user picked the row by is byte-for-byte the one the part ends up carrying.
			const QString imageUrl = toQt(MouserSearchService::previewImageUrl(dto.imagePath));
			QTableWidgetItem* picture = m_ui->resultsTable->item(row, ColumnThumbnail);
			picture->setData(RoleImageUrl, imageUrl);
			picture->setToolTip(QString());
			requestThumbnail(imageUrl);

			// The datasheet answer needs no request at all — it is either in the response or
			// derivable from the manufacturer (MouserSearchService::datasheetUrlFor).
			const MouserPartPrefill prefill = MouserSearchService::toPrefill(dto);
			const QString mpn = toQt(dto.manufacturerPartNumber);
			QTableWidgetItem* files = m_ui->resultsTable->item(row, ColumnFiles);
			files->setData(RoleMpn, mpn);
			files->setData(RoleHasDatasheet, !prefill.datasheetUrl.empty());
			files->setTextAlignment(Qt::AlignCenter);
			requestEcadAvailability(mpn, toQt(dto.manufacturer));
			applyFileGlyphs(mpn);
		}
		m_ui->resultsTable->resizeColumnsToContents();
		m_ui->resultsTable->setColumnWidth(ColumnThumbnail, ThumbnailSize + 8);
		m_ui->resultsTable->setColumnWidth(ColumnFiles, 4 * FileGlyphSize + 3 * 3 + 12);
		m_ui->resultsTable->horizontalHeader()->setSectionResizeMode(ColumnDescription, QHeaderView::Stretch);
		updateButtons();
	}

	void MouserSearchDialog::requestThumbnail(const QString& url)
	{
		if (url.isEmpty())
		{
			return;
		}
		if (m_thumbnails.contains(url))
		{
			applyThumbnail(url);
			return;
		}
		// A series shares one stock photo, so the same URL usually appears on several rows of one
		// result set — without this it would be fetched once per row.
		if (m_thumbnailsInFlight.contains(url))
		{
			return;
		}
		m_thumbnailsInFlight.insert(url);

		// QPointer, not `this`: the download outlives a dialog the user closed mid-search, and the
		// queued call is what would then land on freed memory. A null guard simply drops it.
		const QPointer<MouserSearchDialog> alive(this);
		QThreadPool* const pool = m_thumbnailPool;
		pool->start(QRunnable::create([alive, url]()
			{
				// Qt's own network stack is served Mouser's block page where WinHTTP is served the
				// file, so this goes through FileStore rather than a QNetworkAccessManager here.
				const DownloadedBytes downloaded =
					FileStore::downloadBytes(url.toStdString(), ThumbnailTimeoutMs);

				// QImage, not QPixmap: a pixmap may only be created and touched on the GUI thread.
				// The decode and the expensive smooth scale still happen out here; only the cheap
				// conversion is left for the other side.
				QImage decoded;
				if (downloaded.ok && downloaded.status == 200)
				{
					// loadFromData sniffs the format, which matters here: every one of these is
					// served as WebP behind a `.JPG` name (see FileStore::correctedFilename).
					if (decoded.loadFromData(reinterpret_cast<const uchar*>(downloaded.bytes.data()),
						static_cast<int>(downloaded.bytes.size())))
					{
						decoded = decoded.scaled(ThumbnailSize, ThumbnailSize,
							Qt::KeepAspectRatio, Qt::SmoothTransformation);
					}
					else
					{
						decoded = QImage();
					}
				}

				QMetaObject::invokeMethod(qApp, [alive, url, decoded]()
					{
						if (alive.isNull())
						{
							return;
						}
						alive->m_thumbnailsInFlight.remove(url);
						// A null pixmap is cached too — a dead URL is then tried once, not once
						// per search for the rest of the session.
						alive->m_thumbnails.insert(url, QPixmap::fromImage(decoded));
						alive->applyThumbnail(url);
					}, Qt::QueuedConnection);
			}));
	}

	void MouserSearchDialog::applyThumbnail(const QString& url)
	{
		const QPixmap picture = m_thumbnails.value(url);
		if (picture.isNull())
		{
			return;
		}
		const QIcon icon(picture);
		for (int row = 0; row < m_ui->resultsTable->rowCount(); ++row)
		{
			QTableWidgetItem* item = m_ui->resultsTable->item(row, ColumnThumbnail);
			if (item != nullptr && item->data(RoleImageUrl).toString() == url)
			{
				item->setIcon(icon);
			}
		}
	}

	void MouserSearchDialog::requestEcadAvailability(const QString& mpn, const QString& manufacturer)
	{
		if (mpn.isEmpty() || m_ecadByMpn.contains(mpn) || m_ecadInFlight.contains(mpn))
		{
			return;
		}
		m_ecadInFlight.insert(mpn);

		const QPointer<MouserSearchDialog> alive(this);
		const std::string needle = mpn.toStdString();
		const std::string maker = manufacturer.toStdString();
		m_thumbnailPool->start(QRunnable::create([alive, mpn, needle, maker]()
			{
				EasyEdaClient client;
				// Deliberately not lookup(): that would also fetch the component, which is a
				// second request for a question a glyph answers with one bit. bestMatch() applies
				// the same exact-match rule the import will, so the glyph cannot promise a symbol
				// the import then refuses to take.
				const EasyEdaSearchResult found = client.search(needle);
				const bool available = !EasyEdaClient::bestMatch(found, needle, maker).empty();

				QMetaObject::invokeMethod(qApp, [alive, mpn, available]()
					{
						if (alive.isNull())
						{
							return;
						}
						alive->m_ecadInFlight.remove(mpn);
						alive->m_ecadByMpn.insert(mpn, available);
						alive->applyFileGlyphs(mpn);
					}, Qt::QueuedConnection);
			}));
	}

	void MouserSearchDialog::applyFileGlyphs(const QString& mpn)
	{
		// EasyEDA gives symbol and footprint together or not at all, so one answer sets both.
		const bool hasEcad = m_ecadByMpn.value(mpn, false);

		for (int row = 0; row < m_ui->resultsTable->rowCount(); ++row)
		{
			QTableWidgetItem* item = m_ui->resultsTable->item(row, ColumnFiles);
			if (item == nullptr || item->data(RoleMpn).toString() != mpn)
			{
				continue;
			}

			int flags = 0;
			if (item->data(RoleHasDatasheet).toBool()) { flags |= AttachmentDatasheet; }
			if (hasEcad) { flags |= AttachmentKicadSymbol | AttachmentKicadFootprint; }
			// The 3D model slot is never lit here: EasyEDA carries one, but PartManager does not
			// convert it yet, so promising it would be a lie the import could not keep.
			item->setData(Qt::DecorationRole,
				AttachmentIconPainter::strip(flags, FileGlyphSize, devicePixelRatioF()));

			QStringList lines;
			lines << (item->data(RoleHasDatasheet).toBool()
				? tr("Datasheet: yes") : tr("Datasheet: Mouser publishes none"));
			if (m_ecadByMpn.contains(mpn))
			{
				lines << (hasEcad
					? tr("KiCad symbol and footprint: EasyEDA has this part")
					: tr("KiCad symbol and footprint: not on EasyEDA — download the model by hand"));
			}
			else
			{
				lines << tr("KiCad symbol and footprint: checking EasyEDA…");
			}
			item->setToolTip(lines.join(QLatin1Char('\n')));
		}
	}

	const MouserPartDto* MouserSearchDialog::selectedDto() const
	{
		const int row = m_ui->resultsTable->currentRow();
		if (row < 0 || row >= static_cast<int>(m_results.size()))
		{
			return nullptr;
		}
		return &m_results[static_cast<size_t>(row)];
	}

	void MouserSearchDialog::updateButtons()
	{
		const MouserPartDto* dto = selectedDto();
		m_ui->useButton->setEnabled(dto != nullptr);
		// Mouser leaves ProductDetailUrl empty on some rows; an empty URL would open the
		// browser on nothing at all.
		m_ui->openButton->setEnabled(dto != nullptr && !dto->productDetailUrl.empty());
	}

	void MouserSearchDialog::useSelected()
	{
		const MouserPartDto* dto = selectedDto();
		if (dto == nullptr)
		{
			return;
		}
		m_prefill = MouserSearchService::toPrefill(*dto);
		accept();
	}

	void MouserSearchDialog::openOnMouser()
	{
		const MouserPartDto* dto = selectedDto();
		if (dto == nullptr || dto->productDetailUrl.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl(toQt(dto->productDetailUrl)));
	}

}
