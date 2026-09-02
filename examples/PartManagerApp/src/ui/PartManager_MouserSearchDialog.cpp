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

#include <algorithm>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// One screenful, which is what the first search fetches — a broad query would otherwise
		// pay for hundreds of rows nobody scrolls to.
		constexpr int KeywordRecords = 25;

		// The spec caps one keyword response at 50, so "load 200 more" is four requests. They run
		// back to back inside one Load more press rather than making the user press it four times.
		constexpr int MaxRecordsPerRequest = 50;

		// Load-more choices. 0 means "everything Mouser says it has", which is bounded by
		// NumberOfResult and so cannot run away.
		const int LoadMoreChoices[] = { 25, 50, 100, 250, 0 };

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

		for (const int choice : LoadMoreChoices)
		{
			m_ui->loadMoreCombo->addItem(choice == 0
				? tr("all of them") : tr("%n more", "", choice), choice);
		}
		m_ui->loadMoreCombo->setCurrentIndex(1);   // 50: one request, a screenful and a half

		connect(m_ui->searchButton, &QPushButton::clicked, this, &MouserSearchDialog::search);
		connect(m_ui->loadMoreButton, &QPushButton::clicked, this, &MouserSearchDialog::loadMore);
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

		m_needle = needle;
		m_byKeyword = false;
		MouserSearchResult result = m_client.searchByPartNumber(needle);
		if (result.ok && result.parts.empty())
		{
			// Not a part number then. The keyword endpoint is the only other thing to try, and
			// trying it beats making the user pick the right endpoint before they know the answer.
			result = m_client.searchByKeyword(needle, KeywordRecords);
			m_byKeyword = true;
		}

		QApplication::restoreOverrideCursor();
		m_ui->searchButton->setEnabled(true);

		if (!result.ok)
		{
			m_results.clear();
			m_totalResults = 0;
			showResults();
			m_ui->statusLabel->setText(tr("Search failed: %1").arg(toQt(result.errorMessage)));
			return;
		}

		m_results = result.parts;
		m_totalResults = result.numberOfResults;
		// rankByMatch scores part numbers; on a keyword query nothing scores and it degrades to
		// a stable no-op, leaving Mouser's own relevance order — which is the right order there.
		MouserSearchService::rankByMatch(m_results, needle);
		showResults();

		if (m_results.empty())
		{
			m_ui->statusLabel->setText(tr("Mouser has nothing for “%1”.").arg(query));
			return;
		}
		showResultCount();
	}

	void MouserSearchDialog::showResultCount()
	{
		const int shown = static_cast<int>(m_results.size());
		if (m_totalResults > shown)
		{
			m_ui->statusLabel->setText(tr("Showing %1 of %2 keyword matches, most relevant first.")
				.arg(shown).arg(m_totalResults));
		}
		else
		{
			m_ui->statusLabel->setText(m_byKeyword
				? tr("%n keyword match(es), most relevant first.", "", shown)
				: tr("%n part-number match(es), closest first.", "", shown));
		}
	}

	void MouserSearchDialog::updateLoadMore()
	{
		// Only a keyword search pages. /search/partnumber takes no startingRecord and hands back
		// everything it matched, so there is never a next page to ask for.
		const int shown = static_cast<int>(m_results.size());
		const bool more = m_byKeyword && shown > 0 && m_totalResults > shown;
		m_ui->loadMoreButton->setEnabled(more);
		m_ui->loadMoreCombo->setEnabled(more);
		m_ui->loadMoreButton->setToolTip(more
			? tr("%n further result(s) available.", "", m_totalResults - shown)
			: QString());
	}

	void MouserSearchDialog::loadMore()
	{
		const int already = static_cast<int>(m_results.size());
		if (!m_byKeyword || m_needle.empty() || m_totalResults <= already)
		{
			return;
		}
		// showResults() rebuilds every row, which drops the selection and scrolls back to the
		// top. Appending 200 rows and losing the one the user was reading is the worse of the
		// two, so the row is put back afterwards.
		const int selectedBefore = m_ui->resultsTable->currentRow();

		const int asked = m_ui->loadMoreCombo->currentData().toInt();
		// 0 is the combo's "all of them"; either way the ceiling is what Mouser says exists, so
		// a wrong NumberOfResult can cost one empty request, not an unbounded loop.
		const int target = asked == 0
			? m_totalResults : std::min(m_totalResults, already + asked);

		m_ui->loadMoreButton->setEnabled(false);
		m_ui->searchButton->setEnabled(false);
		QApplication::setOverrideCursor(Qt::WaitCursor);

		QString failure;
		while (static_cast<int>(m_results.size()) < target)
		{
			const int startingRecord = static_cast<int>(m_results.size());
			const int wanted = std::min(MaxRecordsPerRequest, target - startingRecord);

			// Each request blocks for up to the client's timeout, so "load 250 more" can sit here
			// for a while. The status line is repainted between requests rather than only at the
			// end — without it a five-request fetch looks like a hung window.
			m_ui->statusLabel->setText(tr("Fetching results %1–%2 of %3…")
				.arg(startingRecord + 1).arg(startingRecord + wanted).arg(m_totalResults));
			QApplication::processEvents();

			const MouserSearchResult page =
				m_client.searchByKeyword(m_needle, wanted, startingRecord);
			if (!page.ok)
			{
				failure = toQt(page.errorMessage);
				break;
			}
			if (page.parts.empty())
			{
				// NumberOfResult promised more than the endpoint will hand over. Believing the
				// promise over the evidence is what would loop forever.
				m_totalResults = static_cast<int>(m_results.size());
				break;
			}
			m_results.insert(m_results.end(), page.parts.begin(), page.parts.end());
			// Deliberately not re-ranked: rankByMatch is a no-op on keyword hits anyway, and
			// re-sorting the whole set would shuffle rows the user is already looking at.
		}

		QApplication::restoreOverrideCursor();
		m_ui->searchButton->setEnabled(true);
		showResults();
		if (selectedBefore >= 0 && selectedBefore < m_ui->resultsTable->rowCount())
		{
			m_ui->resultsTable->selectRow(selectedBefore);
			m_ui->resultsTable->scrollToItem(m_ui->resultsTable->item(selectedBefore, 0));
		}
		if (failure.isEmpty())
		{
			showResultCount();
		}
		else
		{
			// The pages that did arrive are kept and shown; only the rest is lost.
			m_ui->statusLabel->setText(tr("Showing %1 of %2 — loading more failed: %3")
				.arg(m_results.size()).arg(m_totalResults).arg(failure));
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
		updateLoadMore();
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
