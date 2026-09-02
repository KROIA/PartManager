#include "ui/PartManager_PartlistImportDialog.h"
#include "ui_PartManager_PartlistImportDialog.h"

#include "mouser/PartManager_MouserClient.h"
#include "settings/PartManager_Settings.h"
#include "ui/PartManager_MouserSearchDialog.h"
#include "ui/PartManager_NewPartDialog.h"

#include <QColor>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QTextStream>

#include <cctype>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// Same amber the partlist editor paints an unresolved line with — the two screens show
		// the same state and must not disagree about what it looks like.
		const QColor UnmatchedRowColor(0xFB, 0xE1, 0x8F);

		enum Column
		{
			ColumnDesignators = 0,
			ColumnMpn,
			ColumnValue,
			ColumnQuantity,
			ColumnMatch,
			ColumnCount
		};

		// The mapping's identity: the header row itself, so a file with the same columns is
		// recognised whatever it is called and wherever it lives. Case- and space-insensitive,
		// because a hand-edited export routinely differs from KiCad's by exactly that much.
		std::string signatureOf(const std::vector<std::string>& headers)
		{
			std::string signature;
			for (const std::string& header : headers)
			{
				for (const char c : header)
				{
					if (!std::isspace(static_cast<unsigned char>(c)))
					{
						signature += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
				}
				signature += '|';
			}
			return signature;
		}

		// Combo entries are (label, delimiter char); AutoDetectDelimiter means "let the header decide".
		struct DelimiterChoice { const char* label; char value; };
		const DelimiterChoice DelimiterChoices[] = {
			{ "Detect automatically", AutoDetectDelimiter },
			{ "Semicolon  ;",         ';' },
			{ "Comma  ,",             ',' },
			{ "Tab",                  '\t' },
		};
	}

	PartlistImportDialog::PartlistImportDialog(const PartlistController& controller, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartlistImportDialog)
		, m_controller(controller)
	{
		m_ui->setupUi(this);

		m_ui->previewTable->setColumnCount(ColumnCount);
		m_ui->previewTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Designators") << tr("Part number") << tr("Value / name")
			<< tr("Qty / unit") << tr("Match"));
		m_ui->previewTable->horizontalHeader()->setSectionResizeMode(ColumnMatch, QHeaderView::Stretch);

		for (const DelimiterChoice& choice : DelimiterChoices)
		{
			m_ui->delimiterCombo->addItem(tr(choice.label), QChar(choice.value));
		}

		connect(m_ui->browseButton, &QPushButton::clicked, this, &PartlistImportDialog::chooseFile);
		connect(m_ui->delimiterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &PartlistImportDialog::reparse);
		// What a file needs to look like to map itself. Shown up front rather than only after a
		// mapping comes out wrong: KiCad's own BOM export already satisfies it, so for the file
		// this is aimed at the hint reads as confirmation, and for anything else it says what to
		// rename before importing.
		m_ui->hintLabel->setText(tr(
			"<b>Recognised headers</b> — matched case-insensitively, anywhere in the name. "
			"KiCad's BOM export (<i>Reference; Qty; Value; Mouser Part Number; "
			"Manufacturer_Part_Number</i>) maps itself.<br>"
			"Designators: <i>Reference, Designator, RefDes</i> · "
			"Part number: <i>MPN, Part Number, Manufacturer Part, Mouser, Order Code, SKU</i> · "
			"Quantity: <i>Qty, Quantity, Count</i> · "
			"Value: <i>Value, Comment, Name, Description</i><br>"
			"Nothing here is required — any column can be picked by hand below, and the choice "
			"is remembered for the next file with the same headers."));

		for (QComboBox* combo : { m_ui->designatorsCombo, m_ui->mpnCombo, m_ui->mpnAltCombo,
			m_ui->quantityCombo, m_ui->valueCombo })
		{
			connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, &PartlistImportDialog::refreshPreview);
		}
		connect(m_ui->previewTable, &QTableWidget::itemSelectionChanged,
			this, &PartlistImportDialog::updateButtons);
		connect(m_ui->mouserButton, &QPushButton::clicked,
			this, &PartlistImportDialog::lookUpSelectedOnMouser);
		connect(m_ui->importButton, &QPushButton::clicked, this, &PartlistImportDialog::importNow);
		connect(m_ui->cancelButton, &QPushButton::clicked, this, &PartlistImportDialog::reject);

		m_parts = m_controller.allParts();
		m_sellerLinks = m_controller.allSellerLinks();
		m_ui->statusLabel->setText(tr("Choose a CSV or BOM file to import."));
		updateButtons();
	}

	PartlistImportDialog::~PartlistImportDialog()
	{
		delete m_ui;
	}

	int PartlistImportDialog::createdPartlistId() const
	{
		return m_createdPartlistId;
	}

	void PartlistImportDialog::chooseFile()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Import CSV / BOM"), QString(),
			tr("Table files (*.csv *.tsv *.txt);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}

		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		{
			QMessageBox::warning(this, tr("Could not read the file"),
				tr("“%1” could not be opened for reading.").arg(path));
			return;
		}
		QTextStream stream(&file);
		// KiCad and Excel both write UTF-8; Qt5 defaults to the locale codec, which would mangle
		// every non-ASCII value ("Ω", "µF") in the file.
		stream.setCodec("UTF-8");
		m_csvText = stream.readAll();

		m_ui->fileEdit->setText(path);
		if (m_ui->nameEdit->text().trimmed().isEmpty())
		{
			m_ui->nameEdit->setText(QFileInfo(path).completeBaseName());
		}
		reparse();
	}

	void PartlistImportDialog::reparse()
	{
		if (m_csvText.isEmpty())
		{
			return;
		}

		const char delimiter =
			m_ui->delimiterCombo->currentData().toChar().toLatin1();
		if (!parseCsv(m_csvText.toStdString(), delimiter, m_table))
		{
			m_rows.clear();
			refreshPreview();
			m_ui->statusLabel->setText(tr("The file has no header row — nothing to map."));
			return;
		}

		// Guessing again on every re-parse is deliberate: a delimiter change re-splits the header,
		// so the column indices the user had picked no longer mean the same thing.
		BomColumnMapping mapping = guessMapping(m_table.headers);

		// A mapping the user has already corrected for this header shape beats the guess — that
		// correction is the whole point of remembering it. The guess still runs first, so a
		// remembered entry that predates a new column simply leaves that field at its guess.
		ImportMappingMemory remembered;
		const bool wasRemembered = Settings::getImportMapping(headerSignature(), remembered);
		if (wasRemembered)
		{
			mapping.designators = remembered.designators;
			mapping.mpn = remembered.mpn;
			mapping.mpnAlt = remembered.mpnAlt;
			mapping.quantity = remembered.quantity;
			mapping.name = remembered.name;
		}

		m_loading = true;
		fillColumnCombo(m_ui->designatorsCombo, mapping.designators);
		fillColumnCombo(m_ui->mpnCombo, mapping.mpn);
		fillColumnCombo(m_ui->mpnAltCombo, mapping.mpnAlt);
		fillColumnCombo(m_ui->quantityCombo, mapping.quantity);
		fillColumnCombo(m_ui->valueCombo, mapping.name);
		m_loading = false;

		refreshPreview();
		if (wasRemembered)
		{
			// Appended rather than replacing refreshPreview()'s line, which is the one that says
			// whether the rows matched — the more important half of the answer.
			m_ui->statusLabel->setText(m_ui->statusLabel->text() + QLatin1Char(' ')
				+ tr("Columns are set the way you mapped this file shape last time."));
		}
	}

	std::string PartlistImportDialog::headerSignature() const
	{
		return m_table.headers.empty() ? std::string() : signatureOf(m_table.headers);
	}

	void PartlistImportDialog::rememberMapping() const
	{
		const std::string signature = headerSignature();
		if (signature.empty())
		{
			return;
		}
		const BomColumnMapping mapping = currentMapping();
		ImportMappingMemory memory;
		memory.headerSignature = signature;
		memory.designators = mapping.designators;
		memory.mpn = mapping.mpn;
		memory.mpnAlt = mapping.mpnAlt;
		memory.quantity = mapping.quantity;
		memory.name = mapping.name;
		Settings::rememberImportMapping(memory);
	}

	void PartlistImportDialog::fillColumnCombo(QComboBox* combo, int current)
	{
		combo->clear();
		combo->addItem(tr("(not used)"), NoCsvColumn);
		for (size_t i = 0; i < m_table.headers.size(); ++i)
		{
			// The header is the user's own file, so no tr(); an empty one still needs a row the
			// user can pick, hence the placeholder.
			const QString header = toQt(m_table.headers[i]);
			combo->addItem(header.isEmpty() ? tr("Column %1").arg(i + 1) : header,
				static_cast<int>(i));
		}
		const int index = combo->findData(current);
		combo->setCurrentIndex(index < 0 ? 0 : index);
	}

	BomColumnMapping PartlistImportDialog::currentMapping() const
	{
		BomColumnMapping mapping;
		mapping.designators = m_ui->designatorsCombo->currentData().toInt();
		mapping.mpn = m_ui->mpnCombo->currentData().toInt();
		mapping.mpnAlt = m_ui->mpnAltCombo->currentData().toInt();
		mapping.quantity = m_ui->quantityCombo->currentData().toInt();
		mapping.name = m_ui->valueCombo->currentData().toInt();
		return mapping;
	}

	void PartlistImportDialog::refreshPreview()
	{
		if (m_loading)
		{
			return;
		}

		m_rows = m_table.headers.empty()
			? std::vector<BomRow>()
			: buildRows(m_table, currentMapping(), m_parts, m_sellerLinks);

		m_ui->previewTable->clearContents();
		m_ui->previewTable->setRowCount(static_cast<int>(m_rows.size()));
		int matched = 0;
		for (size_t i = 0; i < m_rows.size(); ++i)
		{
			const BomRow& bomRow = m_rows[i];
			const int row = static_cast<int>(i);
			const bool isMatched = bomRow.matchedPartId != NoPartId;
			matched += isMatched ? 1 : 0;

			QString matchText = tr("not in the inventory yet");
			if (isMatched)
			{
				for (const Part& part : m_parts)
				{
					if (part.id == bomRow.matchedPartId)
					{
						matchText = partPickerLabel(part);
						break;
					}
				}
			}

			const QString cells[ColumnCount] = {
				toQt(bomRow.designators),
				// The alternative only when the primary cell is empty on this row — showing both
				// would make the column twice as wide to say the same thing on most rows.
				toQt(bomRow.mpn.empty() ? bomRow.mpnAlt : bomRow.mpn),
				toQt(bomRow.name),
				QString::number(bomRow.quantityPerUnit),
				matchText,
			};
			for (int column = 0; column < ColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setToolTip(cells[column]);
				if (!isMatched)
				{
					item->setBackground(UnmatchedRowColor);
				}
				m_ui->previewTable->setItem(row, column, item);
			}
		}
		m_ui->previewTable->resizeColumnsToContents();
		m_ui->previewTable->horizontalHeader()->setSectionResizeMode(ColumnMatch, QHeaderView::Stretch);

		const int total = static_cast<int>(m_rows.size());
		const int unmatched = total - matched;
		if (total == 0)
		{
			m_ui->statusLabel->setText(m_csvText.isEmpty()
				? tr("Choose a CSV or BOM file to import.")
				: tr("No usable rows — check the delimiter and the column mapping."));
		}
		else if (unmatched == 0)
		{
			m_ui->statusLabel->setText(tr("%n line(s), all matched to parts already in stock.",
				"", total));
		}
		else if (MouserClient::hasApiKey())
		{
			// The rows import either way; the lookup is an offer, not a precondition.
			m_ui->statusLabel->setText(tr("%1 of %2 lines match nothing in the inventory. They import "
				"as unresolved lines — or pick one and look it up on Mouser.")
				.arg(unmatched).arg(total));
		}
		else
		{
			// Without a key there is nothing to offer, so the message stops at the report rather
			// than pointing at a button that cannot work.
			m_ui->statusLabel->setText(tr("%1 of %2 lines match nothing in the inventory and will "
				"import as unresolved lines, to be matched by hand in the editor.")
				.arg(unmatched).arg(total));
		}

		m_ui->importButton->setEnabled(total > 0);
		updateButtons();
	}

	int PartlistImportDialog::selectedRow() const
	{
		const int row = m_ui->previewTable->currentRow();
		if (row < 0 || row >= static_cast<int>(m_rows.size()))
		{
			return -1;
		}
		return row;
	}

	void PartlistImportDialog::updateButtons()
	{
		const int row = selectedRow();
		const bool unmatchedRowSelected = row >= 0
			&& m_rows[static_cast<size_t>(row)].matchedPartId == NoPartId;
		m_ui->mouserButton->setEnabled(unmatchedRowSelected && MouserClient::hasApiKey());
		if (!MouserClient::hasApiKey())
		{
			m_ui->mouserButton->setToolTip(tr("No Mouser API key — set the %1 environment variable "
				"and restart the app.").arg(QLatin1String(MouserClient::ApiKeyEnvVar)));
		}
	}

	void PartlistImportDialog::lookUpSelectedOnMouser()
	{
		const int row = selectedRow();
		if (row < 0)
		{
			return;
		}
		const BomRow& bomRow = m_rows[static_cast<size_t>(row)];

		// The part number if the file gave one, the value otherwise — a BOM without an MPN column
		// still names its parts somewhere, and the search box handles a keyword too.
		const QString query = toQt(bomRowIdentity(bomRow));
		if (query.isEmpty())
		{
			QMessageBox::information(this, tr("Nothing to look up"),
				tr("This line has neither a part number nor a value to search for."));
			return;
		}

		MouserSearchDialog search(this);
		search.searchFor(query);
		if (search.exec() != QDialog::Accepted)
		{
			return;
		}

		// Straight into the same §6 prefill flow the ribbon's "Import from Mouser" uses: the part
		// is created properly, with a type and confirmed fields, never as a typeless stub.
		NewPartDialog newPart(m_controller.handle(), this);
		newPart.setPrefill(search.selectedPrefill());
		if (newPart.exec() != QDialog::Accepted || newPart.createdPartId() == 0)
		{
			return;
		}

		// Re-read and re-match rather than pinning this one row: the new part very often matches
		// several lines of the same BOM, and matching only the selected one would leave the others
		// orange for no reason.
		m_parts = m_controller.allParts();
		// The new part carries a Mouser link, which is what a 'Mouser Part Number' column matches
		// against — re-reading only the parts would leave those rows unmatched.
		m_sellerLinks = m_controller.allSellerLinks();
		refreshPreview();
		m_ui->previewTable->selectRow(row);
	}

	void PartlistImportDialog::importNow()
	{
		if (m_rows.empty())
		{
			return;
		}

		// Remembered on Import rather than on every combo change: what is worth replaying is the
		// mapping the user actually imported with, not every state they passed through on the
		// way to it. Cancel therefore leaves the previous memory alone.
		rememberMapping();

		Partlist partlist;
		partlist.name = m_ui->nameEdit->text().trimmed().isEmpty()
			? tr("Imported partlist").toStdString()
			: m_ui->nameEdit->text().trimmed().toStdString();
		partlist.source = PartlistSource::CsvImport;

		const int id = m_controller.create(partlist);
		if (id == NoPartlistId)
		{
			QMessageBox::warning(this, tr("Could not create the partlist"),
				tr("The database rejected the imported partlist."));
			return;
		}

		std::vector<PartlistItem> items;
		items.reserve(m_rows.size());
		for (const BomRow& bomRow : m_rows)
		{
			PartlistItem item;
			item.partId = bomRow.matchedPartId;
			item.designators = bomRow.designators;
			item.quantityPerUnit = bomRow.quantityPerUnit;
			// Kept for every row, not just the unresolved ones: a wrong *match* is only findable
			// later if the original line is still there to compare against.
			item.rawImportData = bomRow.rawJson;
			items.push_back(item);
		}
		if (!m_controller.saveItems(id, items))
		{
			QMessageBox::warning(this, tr("Could not import the lines"),
				tr("The partlist was created but its lines could not be written."));
		}

		m_createdPartlistId = id;
		accept();
	}

}
