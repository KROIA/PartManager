#include "ui/PartManager_MouserSearchDialog.h"
#include "ui_PartManager_MouserSearchDialog.h"

#include <QApplication>
#include <QDesktopServices>
#include <QHeaderView>
#include <QTableWidgetItem>
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

		// Mouser's own column order, minus everything the search step cannot act on.
		enum Column
		{
			ColumnMouserNumber = 0,
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
	}

	MouserSearchDialog::MouserSearchDialog(QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::MouserSearchDialog)
	{
		m_ui->setupUi(this);

		m_ui->resultsTable->setColumnCount(ColumnCount);
		m_ui->resultsTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Mouser #") << tr("MPN") << tr("Manufacturer") << tr("Description")
			<< tr("Category") << tr("In stock") << tr("Price"));
		m_ui->resultsTable->horizontalHeader()->setStretchLastSection(false);
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
		delete m_ui;
	}

	const MouserPartPrefill& MouserSearchDialog::selectedPrefill() const
	{
		return m_prefill;
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
		}
		m_ui->resultsTable->resizeColumnsToContents();
		m_ui->resultsTable->horizontalHeader()->setSectionResizeMode(ColumnDescription, QHeaderView::Stretch);
		updateButtons();
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
