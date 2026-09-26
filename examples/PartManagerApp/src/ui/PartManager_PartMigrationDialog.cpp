#include "ui/PartManager_PartMigrationDialog.h"
#include "ui_PartManager_PartMigrationDialog.h"

#include "mouser/PartManager_MouserClient.h"
#include "ui/PartManager_MouserSearchDialog.h"
#include "ui/PartManager_NewPartDialog.h"
#include "ui/PartManager_PartEditorDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QHeaderView>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidgetItem>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// A paste longer than this still migrates whole — only the *preview* stops, because
		// scrolling five hundred rows to check a mapping is not how anybody checks a mapping.
		const int MaxPreviewRows = 50;

		// Already in the database: nothing to create, so the row reads as quiet rather than as
		// work. Same family of tints the rest of the app paints state with.
		const QColor ExistingRowColor(0xDC, 0xE8, 0xF7);
		// Done, by this dialog, just now.
		const QColor CreatedRowColor(0xD6, 0xEF, 0xD0);
		// Deliberately left out — greyed rather than tinted, so it reads as absent, not as a state.
		const QColor SkippedTextColor(0x90, 0x90, 0x90);
		const QColor ExistingTextColor(0x40, 0x50, 0x60);

		enum PreviewColumn
		{
			PreviewMpn = 0,
			PreviewMouser,
			PreviewStock,
			PreviewManufacturer,
			PreviewDescription,
			PreviewNotes,
			PreviewColumnCount
		};

		enum WorklistColumn
		{
			WorkStatus = 0,
			WorkMpn,
			WorkMouser,
			WorkStock,
			WorkManufacturer,
			WorkDescription,
			WorkNotes,
			WorkColumnCount
		};

		enum StackPage
		{
			PastePage = 0,
			WorklistPage
		};

		// Combo entries are (label, delimiter char); AutoDetectDelimiter means "work it out".
		struct DelimiterChoice { const char* label; char value; };
		const DelimiterChoice DelimiterChoices[] = {
			{ "Detect automatically", AutoDetectDelimiter },
			{ "Tab",                  '\t' },
			{ "Semicolon  ;",         ';' },
			{ "Comma  ,",             ',' },
		};
	}

	// ---------------------------------------------------------------------------------------
	// Prefill-merge rules — what wins when a Mouser hit is used for a pasted row.
	//
	// This is a real decision, not an implementation detail, because the two sources disagree
	// about different things for different reasons:
	//
	// * MPN, package, attributes, datasheet, image, Mouser article number: **Mouser's**. The user
	//   just picked that exact part out of a result list, so the vendor's own data is the more
	//   precise answer and the pasted cell is at best a shorthand for it.
	// * Stock count: **always the pasted row's**. Mouser knows what *it* has on a shelf in Texas,
	//   which says nothing about the drawer this migration is describing. It is applied through
	//   NewPartDialog's stock spin box (setListDefaults) rather than written here, so the §3
	//   opening-balance transaction is produced by the one existing code path instead of a second
	//   one that would have to stay in step with it.
	// * Manufacturer and description: Mouser's when it published one, the row's otherwise. Either
	//   source can be blank and neither being blank is informative.
	// * Notes: **always the row's**, and never dropped. `Part` has no notes column — `description`
	//   is the only free-text field there is — so the note is appended to the description on its
	//   own line. The worklist keeps showing the raw note in its own column either way, so what
	//   was pasted stays visible even after it has been folded into the description.
	// ---------------------------------------------------------------------------------------

	PartMigrationDialog::PartMigrationDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartMigrationDialog)
		, m_controller(handle)
		, m_stock(handle)
	{
		m_ui->setupUi(this);

		m_ui->previewTable->setColumnCount(PreviewColumnCount);
		m_ui->previewTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Part number") << tr("Mouser number") << tr("Stock") << tr("Manufacturer")
			<< tr("Description") << tr("Notes"));
		m_ui->previewTable->horizontalHeader()->setSectionResizeMode(PreviewDescription,
			QHeaderView::Stretch);

		m_ui->worklistTable->setColumnCount(WorkColumnCount);
		m_ui->worklistTable->setHorizontalHeaderLabels(QStringList()
			<< tr("Status") << tr("Part number") << tr("Mouser number") << tr("Stock")
			<< tr("Manufacturer") << tr("Description") << tr("Notes"));
		m_ui->worklistTable->horizontalHeader()->setSectionResizeMode(WorkDescription,
			QHeaderView::Stretch);

		for (const DelimiterChoice& choice : DelimiterChoices)
		{
			m_ui->delimiterCombo->addItem(tr(choice.label), QChar(choice.value));
		}

		connect(m_ui->pasteEdit, &QPlainTextEdit::textChanged, this, &PartMigrationDialog::reparse);
		connect(m_ui->delimiterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &PartMigrationDialog::reparse);
		connect(m_ui->headerCheck, &QCheckBox::toggled, this, &PartMigrationDialog::reparse);
		for (QComboBox* combo : { m_ui->mpnCombo, m_ui->mouserCombo, m_ui->stockCombo,
			m_ui->manufacturerCombo, m_ui->descriptionCombo, m_ui->notesCombo })
		{
			connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
				this, &PartMigrationDialog::refreshPreview);
		}

		connect(m_ui->nextButton, &QPushButton::clicked, this, &PartMigrationDialog::goToWorklist);
		connect(m_ui->backButton, &QPushButton::clicked, this, &PartMigrationDialog::goToPaste);
		connect(m_ui->closeButton, &QPushButton::clicked, this, &PartMigrationDialog::reject);

		connect(m_ui->worklistTable, &QTableWidget::itemSelectionChanged,
			this, &PartMigrationDialog::updateButtons);
		connect(m_ui->mouserButton, &QPushButton::clicked,
			this, &PartMigrationDialog::searchSelectedOnMouser);
		connect(m_ui->addPartButton, &QPushButton::clicked,
			this, &PartMigrationDialog::addSelectedPart);
		connect(m_ui->openPartButton, &QPushButton::clicked,
			this, &PartMigrationDialog::openSelectedPart);
		connect(m_ui->bookStockButton, &QPushButton::clicked,
			this, &PartMigrationDialog::bookSelectedStock);
		connect(m_ui->skipButton, &QPushButton::clicked, this, &PartMigrationDialog::skipSelected);

		if (!MouserClient::hasApiKey())
		{
			m_ui->mouserButton->setToolTip(tr("No Mouser API key — set the %1 environment variable "
				"and restart the app.").arg(QLatin1String(MouserClient::ApiKeyEnvVar)));
		}

		m_parts = m_controller.allParts();
		m_sellerLinks = m_controller.allSellerLinks();

		m_ui->stack->setCurrentIndex(PastePage);
		m_ui->backButton->setVisible(false);
		m_ui->pasteStatusLabel->setText(tr("Paste a list to get started."));
		updateButtons();
	}

	PartMigrationDialog::~PartMigrationDialog()
	{
		delete m_ui;
	}

	int PartMigrationDialog::createdCount() const
	{
		return m_createdCount;
	}

	void PartMigrationDialog::reparse()
	{
		const QString text = m_ui->pasteEdit->toPlainText();
		const char delimiter = m_ui->delimiterCombo->currentData().toChar().toLatin1();

		if (text.trimmed().isEmpty()
			|| !parsePastedList(text.toStdString(), delimiter, m_table, m_ui->headerCheck->isChecked()))
		{
			m_table = CsvTable();
			m_rows.clear();
			m_loading = true;
			for (QComboBox* combo : { m_ui->mpnCombo, m_ui->mouserCombo, m_ui->stockCombo,
				m_ui->manufacturerCombo, m_ui->descriptionCombo, m_ui->notesCombo })
			{
				combo->clear();
				combo->addItem(tr("(not used)"), NoMigrationColumn);
			}
			m_loading = false;
			refreshPreview();
			return;
		}

		// Guessed again on every re-parse, like PartlistImportDialog: a delimiter or header change
		// re-splits the header row, so the indices the user had picked no longer mean the same
		// columns and keeping them would silently map the wrong cells.
		const MigrationColumnMapping mapping = guessMigrationMapping(m_table.headers);

		m_loading = true;
		fillColumnCombo(m_ui->mpnCombo, mapping.mpn);
		fillColumnCombo(m_ui->mouserCombo, mapping.mouser);
		fillColumnCombo(m_ui->stockCombo, mapping.stock);
		fillColumnCombo(m_ui->manufacturerCombo, mapping.manufacturer);
		fillColumnCombo(m_ui->descriptionCombo, mapping.description);
		fillColumnCombo(m_ui->notesCombo, mapping.notes);
		m_loading = false;

		refreshPreview();
	}

	void PartMigrationDialog::fillColumnCombo(QComboBox* combo, int current)
	{
		combo->clear();
		combo->addItem(tr("(not used)"), NoMigrationColumn);
		for (size_t i = 0; i < m_table.headers.size(); ++i)
		{
			// The header is the user's own paste, so no tr(); an empty one still needs a row the
			// user can pick, hence the placeholder.
			const QString header = toQt(m_table.headers[i]);
			combo->addItem(header.isEmpty() ? tr("Column %1").arg(i + 1) : header,
				static_cast<int>(i));
		}
		const int index = combo->findData(current);
		combo->setCurrentIndex(index < 0 ? 0 : index);
	}

	MigrationColumnMapping PartMigrationDialog::currentMapping() const
	{
		MigrationColumnMapping mapping;
		mapping.mpn = m_ui->mpnCombo->currentData().toInt();
		mapping.mouser = m_ui->mouserCombo->currentData().toInt();
		mapping.stock = m_ui->stockCombo->currentData().toInt();
		mapping.manufacturer = m_ui->manufacturerCombo->currentData().toInt();
		mapping.description = m_ui->descriptionCombo->currentData().toInt();
		mapping.notes = m_ui->notesCombo->currentData().toInt();
		return mapping;
	}

	void PartMigrationDialog::refreshPreview()
	{
		if (m_loading)
		{
			return;
		}

		// Resolved against the real inventory already here, not only behind Next: "4 of these are
		// things you already have" is exactly the sentence that tells the user whether the mapping
		// they are looking at is the right one.
		m_rows = m_table.headers.empty()
			? std::vector<MigrationRow>()
			: buildMigrationRows(m_table, currentMapping(), m_parts, m_sellerLinks);

		const int total = static_cast<int>(m_rows.size());
		const int shown = total < MaxPreviewRows ? total : MaxPreviewRows;
		m_ui->previewTable->clearContents();
		m_ui->previewTable->setRowCount(shown);
		int existing = 0;
		for (int row = 0; row < total; ++row)
		{
			const MigrationRow& migrationRow = m_rows[static_cast<size_t>(row)];
			existing += migrationRow.status == MigrationStatus::Existing ? 1 : 0;
			if (row >= shown)
			{
				continue;
			}

			const QString cells[PreviewColumnCount] = {
				toQt(migrationRow.mpn),
				toQt(migrationRow.mouserPartNumber),
				migrationRow.stockGiven ? QString::number(migrationRow.stock) : QString(),
				toQt(migrationRow.manufacturer),
				toQt(migrationRow.description),
				toQt(migrationRow.notes),
			};
			for (int column = 0; column < PreviewColumnCount; ++column)
			{
				QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
				item->setToolTip(cells[column]);
				m_ui->previewTable->setItem(row, column, item);
			}
		}
		m_ui->previewTable->resizeColumnsToContents();
		m_ui->previewTable->horizontalHeader()->setSectionResizeMode(PreviewDescription,
			QHeaderView::Stretch);

		// Either number identifies a part, so either column is enough to continue with: a list of
		// nothing but Mouser article numbers is a complete inventory and demanding a manufacturer
		// number the user does not have would block it on data that does not exist.
		const bool hasPartNumber = hasPartNumberColumn(currentMapping());
		if (total == 0)
		{
			m_ui->pasteStatusLabel->setText(m_ui->pasteEdit->toPlainText().trimmed().isEmpty()
				? tr("Paste a list to get started.")
				: tr("No usable rows — check the delimiter and the column mapping."));
		}
		else if (!hasPartNumber)
		{
			m_ui->pasteStatusLabel->setText(tr("%n row(s) — pick the column holding the part "
				"numbers, or the one holding the Mouser numbers, to continue.", "", total));
		}
		else
		{
			QString status = tr("%n row(s), %1 already in the database.", "", total)
				.arg(existing);
			if (total > shown)
			{
				status += QLatin1Char(' ') + tr("The first %n are previewed.", "", shown);
			}
			m_ui->pasteStatusLabel->setText(status);
		}

		m_ui->nextButton->setEnabled(total > 0 && hasPartNumber);
	}

	void PartMigrationDialog::goToWorklist()
	{
		if (m_rows.empty())
		{
			return;
		}

		m_statusNotes.assign(m_rows.size(), QString());
		m_ui->worklistStatusLabel->clear();
		m_ui->stack->setCurrentIndex(WorklistPage);
		m_ui->nextButton->setVisible(false);
		m_ui->backButton->setVisible(true);
		refreshWorklist();
		selectNextPending(-1);
		updateButtons();
	}

	void PartMigrationDialog::goToPaste()
	{
		// Re-parsing rebuilds the rows from scratch, so everything already decided about them is
		// gone. The parts stay — they are database rows now — but the record of which pasted line
		// produced which one does not, and that is worth a question.
		if (m_createdCount > 0 || handledCount() > 0)
		{
			if (QMessageBox::question(this, tr("Start over?"),
				tr("Going back re-reads the pasted list, which throws this worklist away. "
				   "The %n part(s) already created stay in the database — only the record of "
				   "what you have worked through is lost.\n\nGo back anyway?", "", handledCount()),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}

		m_statusNotes.clear();
		m_ui->stack->setCurrentIndex(PastePage);
		m_ui->nextButton->setVisible(true);
		m_ui->backButton->setVisible(false);
		// The inventory has very likely moved on since the paste page was last shown.
		m_parts = m_controller.allParts();
		m_sellerLinks = m_controller.allSellerLinks();
		refreshPreview();
	}

	int PartMigrationDialog::selectedRow() const
	{
		const int row = m_ui->worklistTable->currentRow();
		if (row < 0 || row >= static_cast<int>(m_rows.size()))
		{
			return -1;
		}
		return row;
	}

	int PartMigrationDialog::handledCount() const
	{
		int handled = 0;
		for (const MigrationRow& row : m_rows)
		{
			// Existing counts as handled without the user doing anything: the part is already in
			// the database, which is the whole point of the migration. Booking its stock is an
			// offer on top, not an outstanding task.
			handled += row.status == MigrationStatus::Pending ? 0 : 1;
		}
		return handled;
	}

	void PartMigrationDialog::writeWorklistRow(int row)
	{
		const MigrationRow& migrationRow = m_rows[static_cast<size_t>(row)];

		QString statusText;
		switch (migrationRow.status)
		{
			case MigrationStatus::Pending:  statusText = tr("To do"); break;
			case MigrationStatus::Existing: statusText = tr("Already there"); break;
			case MigrationStatus::Created:  statusText = tr("Created"); break;
			case MigrationStatus::Skipped:  statusText = tr("Skipped"); break;
		}
		const QString note = m_statusNotes[static_cast<size_t>(row)];
		if (!note.isEmpty())
		{
			statusText += QStringLiteral(" — ") + note;
		}

		const QString cells[WorkColumnCount] = {
			statusText,
			toQt(migrationRow.mpn),
			toQt(migrationRow.mouserPartNumber),
			migrationRow.stockGiven ? QString::number(migrationRow.stock) : QString(),
			toQt(migrationRow.manufacturer),
			toQt(migrationRow.description),
			toQt(migrationRow.notes),
		};
		for (int column = 0; column < WorkColumnCount; ++column)
		{
			QTableWidgetItem* item = new QTableWidgetItem(cells[column]);
			item->setToolTip(cells[column]);
			switch (migrationRow.status)
			{
				case MigrationStatus::Existing:
					item->setBackground(ExistingRowColor);
					item->setForeground(ExistingTextColor);
					break;
				case MigrationStatus::Created:
					item->setBackground(CreatedRowColor);
					break;
				case MigrationStatus::Skipped:
					item->setForeground(SkippedTextColor);
					break;
				case MigrationStatus::Pending:
					break;
			}
			m_ui->worklistTable->setItem(row, column, item);
		}

		const int total = static_cast<int>(m_rows.size());
		m_ui->progressLabel->setText(tr("%1 of %2 handled").arg(handledCount()).arg(total));
	}

	void PartMigrationDialog::refreshWorklist()
	{
		m_ui->worklistTable->clearContents();
		m_ui->worklistTable->setRowCount(static_cast<int>(m_rows.size()));
		for (int row = 0; row < static_cast<int>(m_rows.size()); ++row)
		{
			writeWorklistRow(row);
		}
		m_ui->worklistTable->resizeColumnsToContents();
		m_ui->worklistTable->horizontalHeader()->setSectionResizeMode(WorkDescription,
			QHeaderView::Stretch);
	}

	void PartMigrationDialog::selectNextPending(int row)
	{
		const int total = static_cast<int>(m_rows.size());
		if (total == 0)
		{
			return;
		}
		// Wraps, so a user who jumped to row 20 by hand still gets the rows above it afterwards
		// instead of running out of list.
		for (int step = 1; step <= total; ++step)
		{
			const int candidate = ((row + step) % total + total) % total;
			if (m_rows[static_cast<size_t>(candidate)].status == MigrationStatus::Pending)
			{
				m_ui->worklistTable->selectRow(candidate);
				return;
			}
		}
	}

	void PartMigrationDialog::updateButtons()
	{
		const int row = m_ui->stack->currentIndex() == WorklistPage ? selectedRow() : -1;
		if (row < 0 || m_statusNotes.size() != m_rows.size())
		{
			m_ui->mouserButton->setEnabled(false);
			m_ui->addPartButton->setEnabled(false);
			m_ui->skipButton->setEnabled(false);
			m_ui->openPartButton->setEnabled(false);
			m_ui->bookStockButton->setEnabled(false);
			return;
		}

		const MigrationRow& migrationRow = m_rows[static_cast<size_t>(row)];
		const bool pending = migrationRow.status == MigrationStatus::Pending;
		const bool existing = migrationRow.status == MigrationStatus::Existing
			&& migrationRow.matchedPartId != NoPartId;

		m_ui->mouserButton->setEnabled(pending && MouserClient::hasApiKey());
		m_ui->addPartButton->setEnabled(pending);
		m_ui->skipButton->setEnabled(pending);
		m_ui->openPartButton->setEnabled(existing);
		// A booking already written is recorded in the note, which is the one thing that is set
		// when stock has been booked — so it doubles as the guard against booking it twice.
		m_ui->bookStockButton->setEnabled(existing && migrationRow.stockGiven
			&& migrationRow.stock > 0
			&& m_statusNotes[static_cast<size_t>(row)].isEmpty());
	}

	void PartMigrationDialog::noteCreated(int row, int partId, const QString& datasheetUrl)
	{
		m_rows[static_cast<size_t>(row)].status = MigrationStatus::Created;
		m_rows[static_cast<size_t>(row)].createdPartId = partId;
		++m_createdCount;

		// The new part is matchable now, so a second pasted line naming the same number would be
		// resolved against it — the inventory has to be re-read for that to happen.
		m_parts = m_controller.allParts();
		m_sellerLinks = m_controller.allSellerLinks();

		writeWorklistRow(row);

		if (m_ui->openEditorCheck->isChecked())
		{
			// The same tail MainWindow::openNewPart() runs. The §6 datasheet download already
			// happened inside New Part; the URL is handed on so a retry after a dead link costs
			// one click.
			PartEditorDialog editor(m_controller.handle(), partId, this);
			editor.setDatasheetSourceUrl(datasheetUrl);
			editor.exec();
		}

		selectNextPending(row);
		updateButtons();
	}

	void PartMigrationDialog::searchSelectedOnMouser()
	{
		const int row = selectedRow();
		if (row < 0)
		{
			return;
		}
		const MigrationRow migrationRow = m_rows[static_cast<size_t>(row)];

		// The manufacturer number is the better search term when the row has one — it finds the
		// part at every distributor packaging variant — but a row carrying only a Mouser number
		// still has something to search for, and starting the dialog empty would waste it.
		MouserSearchDialog search(this);
		search.searchFor(toQt(migrationRow.mpn.empty()
			? migrationRow.mouserPartNumber : migrationRow.mpn));
		if (search.exec() != QDialog::Accepted)
		{
			return;
		}

		// See the merge rules at the top of this file.
		MouserPartPrefill prefill = search.selectedPrefill();
		if (prefill.part.manufacturer.empty())
		{
			prefill.part.manufacturer = migrationRow.manufacturer;
		}
		if (prefill.part.description.empty())
		{
			prefill.part.description = migrationRow.description;
		}

		NewPartDialog dialog(m_controller.handle(), this);
		dialog.setPrefill(prefill);
		dialog.setListDefaults(migrationRow.stockGiven ? migrationRow.stock : 0,
			toQt(migrationRow.notes));
		if (dialog.exec() != QDialog::Accepted || dialog.createdPartId() == 0)
		{
			return;
		}
		noteCreated(row, dialog.createdPartId(), toQt(prefill.datasheetUrl));
	}

	bool PartMigrationDialog::fetchMouserPrefill(const std::string& mouserPartNumber,
		MouserPartPrefill& outPrefill, QString& outMessage)
	{
		if (!MouserClient::hasApiKey())
		{
			outMessage = tr("No Mouser API key — filled the form in from the pasted row instead.");
			return false;
		}

		// MouserClient is synchronous: this call really does freeze the dialog for up to its
		// timeout. Said on the status line and shown in the cursor before it starts, exactly as
		// MouserSearchDialog does it, rather than leaving the window looking hung.
		m_ui->worklistStatusLabel->setText(tr("Looking %1 up on Mouser…")
			.arg(toQt(mouserPartNumber)));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		// Repaint before the blocking call, or the status line above never reaches the screen.
		QApplication::processEvents();

		MouserClient client;
		const MouserSearchResult result = client.searchByPartNumber(mouserPartNumber);
		QApplication::restoreOverrideCursor();

		if (!result.ok)
		{
			// Not a modal: this is a per-row action in a bulk flow, and a dialog to dismiss on
			// every one of forty rows would make the failure worse than the thing that failed.
			outMessage = tr("Mouser could not be reached (%1) — filled the form in from the pasted "
				"row instead.").arg(toQt(result.errorMessage));
			return false;
		}
		if (result.parts.empty())
		{
			outMessage = tr("Mouser knows no part %1 — filled the form in from the pasted row "
				"instead.").arg(toQt(mouserPartNumber));
			return false;
		}

		// The same ranking the search dialog applies: Mouser answers an article number with the
		// part itself *and* its packaging variants, and the first row it returns is not reliably
		// the one that was asked for.
		std::vector<MouserPartDto> parts = result.parts;
		MouserSearchService::rankByMatch(parts, mouserPartNumber);
		outPrefill = MouserSearchService::toPrefill(parts.front());
		outMessage = tr("Filled in from Mouser %1.").arg(toQt(outPrefill.mouserPartNumber));
		return true;
	}

	void PartMigrationDialog::addSelectedPart()
	{
		const int row = selectedRow();
		if (row < 0)
		{
			return;
		}
		const MigrationRow migrationRow = m_rows[static_cast<size_t>(row)];

		// Only a row that names a Mouser article number is looked up, and only by that number.
		// A bare manufacturer number is deliberately left to the search button: it needs a result
		// list to choose from, and a silent request behind every Add Part would make a bulk flow
		// unpredictable — some rows instant, some fifteen seconds, none of it explained.
		MouserPartPrefill prefill;
		QString statusMessage;
		const bool fromMouser = !migrationRow.mouserPartNumber.empty()
			&& fetchMouserPrefill(migrationRow.mouserPartNumber, prefill, statusMessage);

		if (fromMouser)
		{
			// See the merge rules at the top of this file: the vendor wins on what it knows
			// better, the pasted row fills what it left blank.
			if (prefill.part.manufacturer.empty())
			{
				prefill.part.manufacturer = migrationRow.manufacturer;
			}
			if (prefill.part.description.empty())
			{
				prefill.part.description = migrationRow.description;
			}
		}
		else
		{
			// A prefill with nothing behind it but the pasted line: no vendor, no datasheet, no
			// attributes. Built as a MouserPartPrefill anyway so there is exactly one way into New
			// Part's form, rather than a second setter that would have to be kept in step with it.
			prefill.part.mpn = migrationRow.mpn;
			prefill.part.manufacturer = migrationRow.manufacturer;
			prefill.part.description = migrationRow.description;
			// The pasted Mouser number travels even when the lookup did not happen or did not
			// answer. It is the one fact about the row that is certainly true, it is what the
			// seller link is written from after Create, and leaving the field empty on exactly the
			// lists that knew the number was the bug this path was rebuilt for.
			prefill.mouserPartNumber = migrationRow.mouserPartNumber;
			// The part number doubles as the name, so §11's "Create needs a name" gate is already
			// satisfied on a dialog the user has nothing to fix on yet. It is an ordinary editable
			// field — the category's naming pattern is offered in the editor afterwards, and renaming
			// here costs one selection. Not done on the Mouser path: a hit carries a real name.
			prefill.part.name = migrationRow.mpn.empty()
				? migrationRow.mouserPartNumber : migrationRow.mpn;
		}
		m_ui->worklistStatusLabel->setText(statusMessage);

		NewPartDialog dialog(m_controller.handle(), this);
		dialog.setPrefill(prefill, fromMouser
			? NewPartDialog::PrefillSource::Mouser
			: NewPartDialog::PrefillSource::ImportedList);
		dialog.setListDefaults(migrationRow.stockGiven ? migrationRow.stock : 0,
			toQt(migrationRow.notes));
		if (dialog.exec() != QDialog::Accepted || dialog.createdPartId() == 0)
		{
			return;
		}
		noteCreated(row, dialog.createdPartId(), toQt(prefill.datasheetUrl));
	}

	void PartMigrationDialog::openSelectedPart()
	{
		const int row = selectedRow();
		if (row < 0 || m_rows[static_cast<size_t>(row)].matchedPartId == NoPartId)
		{
			return;
		}

		PartEditorDialog editor(m_controller.handle(),
			m_rows[static_cast<size_t>(row)].matchedPartId, this);
		editor.exec();

		m_parts = m_controller.allParts();
		writeWorklistRow(row);
		updateButtons();
	}

	void PartMigrationDialog::bookSelectedStock()
	{
		const int row = selectedRow();
		if (row < 0)
		{
			return;
		}
		const MigrationRow& migrationRow = m_rows[static_cast<size_t>(row)];
		if (migrationRow.matchedPartId == NoPartId || !migrationRow.stockGiven
			|| migrationRow.stock <= 0)
		{
			return;
		}

		QString partName = toQt(migrationRow.mpn);
		for (const Part& part : m_parts)
		{
			if (part.id == migrationRow.matchedPartId)
			{
				partName = toQt(part.name);   // the user's own text, never tr()'d
				break;
			}
		}

		// Confirmed rather than booked on the button press: a restock is a §3 transaction that
		// cannot be taken back by clicking again, and a pasted list is as likely to be "what I
		// have" as "what I just bought".
		if (QMessageBox::question(this, tr("Book this stock in?"),
			tr("Add %n unit(s) to “%1”?\n\nThis is written as a restock, so the stock history "
			   "records where the quantity came from.", "", migrationRow.stock).arg(partName),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}

		if (!m_stock.restock(migrationRow.matchedPartId, migrationRow.stock, tr("List import")))
		{
			QMessageBox::warning(this, tr("Could not book the stock"),
				tr("The database rejected the restock for “%1”.").arg(partName));
			return;
		}

		m_statusNotes[static_cast<size_t>(row)] = tr("%n booked in", "", migrationRow.stock);
		m_parts = m_controller.allParts();
		writeWorklistRow(row);
		updateButtons();
	}

	void PartMigrationDialog::skipSelected()
	{
		const int row = selectedRow();
		if (row < 0 || m_rows[static_cast<size_t>(row)].status != MigrationStatus::Pending)
		{
			return;
		}
		m_rows[static_cast<size_t>(row)].status = MigrationStatus::Skipped;
		writeWorklistRow(row);
		selectNextPending(row);
		updateButtons();
	}

	void PartMigrationDialog::done(int result)
	{
		int pending = 0;
		for (const MigrationRow& row : m_rows)
		{
			pending += row.status == MigrationStatus::Pending ? 1 : 0;
		}

		// §10: the worklist is in-progress state and is not persisted anywhere, so closing really
		// does lose it. Said plainly, together with the half that is *not* lost.
		if (pending > 0 && m_ui->stack->currentIndex() == WorklistPage)
		{
			if (QMessageBox::question(this, tr("Close the migration?"),
				tr("%n part(s) not yet handled. The list is not saved anywhere, so closing "
				   "loses it — the parts already created stay in the database.\n\n"
				   "Close anyway?", "", pending),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}
		QDialog::done(result);
	}

}
