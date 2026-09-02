#include "ui/PartManager_PartEditorDialog.h"
#include "ui_PartManager_PartEditorDialog.h"

#include "widgets/PartManager_AttributeFormWidget.h"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>

namespace PartManager
{
	namespace
	{
		// §10 debounce: long enough that typing a word is one write, short enough that
		// closing the window right after a keystroke never races the flush in done().
		constexpr int AutosaveDelayMs = 400;

		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// Same rule the part table's chips use: pick the readable text colour for the fill.
		QString chipStyleSheet(const QString& color)
		{
			const QColor background(color);
			const QString foreground = background.isValid() && background.lightness() < 128
				? QStringLiteral("#FFFFFF") : QStringLiteral("#000000");
			return QStringLiteral("QPushButton { background-color: %1; color: %2; border: none;"
				" border-radius: 8px; padding: 2px 8px; }")
				.arg(background.isValid() ? background.name() : QStringLiteral("#CCCCCC"), foreground);
		}
	}

	PartEditorDialog::PartEditorDialog(DatabaseHandle* handle, int partId, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::PartEditorDialog)
		, m_controller(handle)
		, m_stock(handle)
		, m_attributeForm(new AttributeFormWidget(this))
		, m_saveTimer(new QTimer(this))
	{
		m_ui->setupUi(this);
		m_ui->attributeLayout->addWidget(m_attributeForm);

		m_saveTimer->setSingleShot(true);
		m_saveTimer->setInterval(AutosaveDelayMs);
		connect(m_saveTimer, &QTimer::timeout, this, &PartEditorDialog::autosave);

		m_part.id = partId;
		loadPart();

		// §10: no Save button anywhere — every one of these ends in the same debounced write.
		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		connect(m_ui->manufacturerEdit, &QLineEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		connect(m_ui->mpnEdit, &QLineEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		connect(m_ui->packageEdit, &QLineEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		connect(m_ui->descriptionEdit, &QPlainTextEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		// The quantity is the one field that is not a plain autosave: it becomes a §3 correction,
		// and it commits on focus-loss/Enter rather than per keystroke so typing "12" logs one
		// adjustment to 12 instead of one to 1 and another to 12.
		connect(m_ui->stockSpin, &QAbstractSpinBox::editingFinished,
			this, &PartEditorDialog::commitStockQuantity);
		connect(m_ui->stockSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, [this](int value)
			{
				// §3 allows a negative count and it means "recount me" — so it is painted, not hidden.
				m_ui->stockSpin->setStyleSheet(value < 0
					? QStringLiteral("color: #c0392b; font-weight: bold;") : QString());
			});
		connect(m_ui->stockMinSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartEditorDialog::scheduleSave);
		// The generated form commits on focus-loss, which already is the debounce point.
		connect(m_attributeForm, &AttributeFormWidget::valueCommitted, this, &PartEditorDialog::autosave);

		connect(m_ui->openDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::openDatasheet);
		connect(m_ui->attachDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::attachDatasheet);
		connect(m_ui->downloadDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::downloadDatasheet);
		connect(m_ui->removeDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::removeDatasheet);

		// The Mouser number is committed on editingFinished rather than per keystroke: every
		// change rewrites the seller link row, and doing that mid-word would churn the table and
		// throw away the stored product URL on the way through.
		connect(m_ui->mouserEdit, &QLineEdit::editingFinished,
			this, &PartEditorDialog::commitMouserPartNumber);
		connect(m_ui->openMouserButton, &QPushButton::clicked, this, &PartEditorDialog::openOnMouser);

		connect(m_ui->closeButton, &QPushButton::clicked, this, &PartEditorDialog::accept);
	}

	void PartEditorDialog::setDatasheetSourceUrl(const QString& url)
	{
		m_datasheetSourceUrl = url;
	}

	PartEditorDialog::~PartEditorDialog()
	{
		delete m_ui;
	}

	void PartEditorDialog::loadPart()
	{
		m_loading = true;
		if (!m_controller.loadPart(m_part.id, m_part))
		{
			m_ui->headerLabel->setText(tr("This part no longer exists."));
			m_ui->scrollArea->setEnabled(false);
			m_ui->addTagButton->setEnabled(false);
			m_ui->datasheetGroup->setEnabled(false);
			m_ui->stockHistoryGroup->setEnabled(false);
			m_loading = false;
			return;
		}

		// The part's name is user data; only the frame around it is translated.
		m_ui->headerLabel->setText(toQt(m_part.name));
		setWindowTitle(tr("Edit Part — %1").arg(toQt(m_part.name)));

		m_ui->nameEdit->setText(toQt(m_part.name));
		m_ui->manufacturerEdit->setText(toQt(m_part.manufacturer));
		m_ui->mpnEdit->setText(toQt(m_part.mpn));
		m_ui->packageEdit->setText(toQt(m_part.package));
		m_ui->descriptionEdit->setPlainText(toQt(m_part.description));
		// The log is the source of truth, `part.stock_qty` only its cache (§3) — so the field shows
		// the log's number, and m_part follows it. A database whose cache had drifted (an import
		// that wrote the column directly) is then corrected by the next autosave rather than
		// re-saved wrong, and correct()'s delta is computed against the number the user can see.
		m_part.stockQty = m_stock.quantity(m_part.id);
		m_ui->stockSpin->setValue(m_part.stockQty);
		m_ui->stockMinSpin->setValue(m_part.stockMinQty);

		m_attributeForm->setAttributes(m_controller.attributesFor(m_part.partTypeId));
		m_attributeForm->setValuesJson(toQt(m_part.attributes));

		reloadTags();
		updateDatasheetState();
		reloadHistory();
		m_loading = false;
	}

	void PartEditorDialog::updateDatasheetState()
	{
		PartFile file;
		const bool attached = m_controller.datasheetFile(m_part, file);
		const bool onDisk = attached && !m_controller.datasheetPath(m_part).empty();

		if (!attached)
		{
			m_ui->datasheetLabel->setText(tr("No datasheet attached yet."));
		}
		else
		{
			// The file name is the user's own data; only the size and the frame are translated.
			const QString name = toQt(file.originalFilename);
			m_ui->datasheetLabel->setText(onDisk
				? tr("%1 (%2)").arg(name, QLocale().formattedDataSize(file.sizeBytes))
				: tr("%1 — the stored file is missing from this database's file store.").arg(name));
		}

		m_ui->openDatasheetButton->setEnabled(onDisk);
		m_ui->removeDatasheetButton->setEnabled(attached);
		m_ui->attachDatasheetButton->setText(attached ? tr("Replace file…") : tr("Attach file…"));
		m_ui->downloadDatasheetButton->setText(attached ? tr("Replace from URL…") : tr("Download…"));
	}

	void PartEditorDialog::openDatasheet()
	{
		const std::string path = m_controller.datasheetPath(m_part);
		if (path.empty())
		{
			QMessageBox::warning(this, tr("Datasheet unavailable"),
				tr("The stored file is no longer in this database's file store."));
			updateDatasheetState();
			return;
		}
		QDesktopServices::openUrl(QUrl::fromLocalFile(toQt(path)));
	}

	void PartEditorDialog::attachDatasheet()
	{
		// PDF-biased, not PDF-only: plenty of real datasheets arrive as a scan or a zip.
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose a datasheet"), QString(),
			tr("Datasheets (*.pdf);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}

		std::string error;
		if (m_controller.attachDatasheet(m_part, path.toStdString(), &error) == 0)
		{
			QMessageBox::warning(this, tr("Could not attach the datasheet"), toQt(error));
			return;
		}
		autosave();
		updateDatasheetState();
		updateMouserState();
	}

	void PartEditorDialog::downloadDatasheet()
	{
		bool accepted = false;
		const QString url = QInputDialog::getText(this, tr("Download a datasheet"), tr("Datasheet URL"),
			QLineEdit::Normal, m_datasheetSourceUrl, &accepted).trimmed();
		if (!accepted || url.isEmpty())
		{
			return;
		}

		// FileStore::downloadFile() is synchronous with a timeout, so the window really does stop
		// responding for up to that long — say so rather than just freezing.
		m_ui->statusLabel->setText(tr("Downloading the datasheet…"));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		std::string error;
		const int fileId = m_controller.downloadDatasheet(m_part, url.toStdString(), &error);
		QApplication::restoreOverrideCursor();

		if (fileId == 0)
		{
			// A failed download changes nothing about the part — §6 explicitly must not block saving.
			m_ui->statusLabel->setText(tr("Changes are saved automatically."));
			QMessageBox::warning(this, tr("Could not download the datasheet"),
				tr("%1\n\nThe part itself is unaffected — you can attach a file by hand instead.")
					.arg(toQt(error)));
			return;
		}
		autosave();
		updateDatasheetState();
	}

	void PartEditorDialog::removeDatasheet()
	{
		PartFile file;
		if (!m_controller.datasheetFile(m_part, file))
		{
			return;
		}
		// The file store may hold the only copy left, so this one asks first.
		if (QMessageBox::question(this, tr("Remove the datasheet"),
			tr("Remove \"%1\" from this part?").arg(toQt(file.originalFilename)))
			!= QMessageBox::Yes)
		{
			return;
		}

		m_controller.detachDatasheet(m_part);
		autosave();
		updateDatasheetState();
	}

	void PartEditorDialog::scheduleSave()
	{
		if (!m_loading)
		{
			m_saveTimer->start();
		}
	}

	void PartEditorDialog::autosave()
	{
		m_saveTimer->stop();
		if (m_loading || m_part.id == 0)
		{
			return;
		}

		m_part.name = m_ui->nameEdit->text().toStdString();
		m_part.manufacturer = m_ui->manufacturerEdit->text().toStdString();
		m_part.mpn = m_ui->mpnEdit->text().toStdString();
		m_part.package = m_ui->packageEdit->text().toStdString();
		m_part.description = m_ui->descriptionEdit->toPlainText().toStdString();
		// Not read off the spin box here: the quantity is only ever changed by commitStockQuantity(),
		// which logs it (§3). m_part.stockQty already holds what the log says, so the write below
		// re-states the cache instead of overwriting it.
		m_part.stockMinQty = m_ui->stockMinSpin->value();
		m_part.attributes = m_attributeForm->valuesJson().toStdString();

		if (m_controller.savePart(m_part))
		{
			m_ui->headerLabel->setText(toQt(m_part.name));
			m_ui->statusLabel->setText(tr("Saved automatically."));
		}
		else
		{
			m_ui->statusLabel->setText(tr("Could not save — the database rejected the change."));
		}
	}

	void PartEditorDialog::commitStockQuantity()
	{
		const int target = m_ui->stockSpin->value();
		if (m_loading || m_part.id == 0 || target == m_part.stockQty)
		{
			return;
		}

		// §3: an absolute count becomes a delta, logged as a manual adjustment. The note is the
		// app's own chrome, not the user's text, so it is translated.
		if (!m_stock.setQuantity(m_part.id, target, tr("Corrected in the part editor")))
		{
			m_ui->statusLabel->setText(tr("Could not save — the database rejected the change."));
			return;
		}
		// setQuantity() already refreshed part.stock_qty; keeping m_part in step is what stops the
		// next autosave() from writing the old number back over it.
		m_part.stockQty = target;
		m_ui->statusLabel->setText(tr("Stock corrected — the adjustment is in the history."));
		reloadHistory();
	}

	void PartEditorDialog::reloadHistory()
	{
		const std::vector<StockTransaction> history = m_stock.history(m_part.id);
		const std::vector<int> quantities = runningQuantities(history);

		m_ui->historyTable->clearContents();
		m_ui->historyTable->setRowCount(static_cast<int>(history.size()));
		for (int row = 0; row < static_cast<int>(history.size()); ++row)
		{
			const StockTransaction& transaction = history[static_cast<size_t>(row)];
			const int resulting = quantities[static_cast<size_t>(row)];

			// A delta is signed both ways so a column of numbers reads as a ledger.
			const QString delta = transaction.deltaQty > 0
				? QStringLiteral("+%1").arg(transaction.deltaQty)
				: QString::number(transaction.deltaQty);

			auto cell = [this, row](int column, const QString& text)
			{
				m_ui->historyTable->setItem(row, column, new QTableWidgetItem(text));
			};
			cell(0, toQt(transaction.createdAt));   // SQLite's own timestamp
			cell(1, delta);
			cell(2, QString::number(resulting));
			cell(3, stockReasonLabel(transaction.reason));
			cell(4, toQt(transaction.note));        // user data

			if (resulting < 0)
			{
				// Same signal as the part table's red count: the books say less than nothing is left.
				m_ui->historyTable->item(row, 2)->setForeground(QBrush(QColor(0xC0, 0x39, 0x2B)));
			}
		}

		m_ui->historyTable->resizeColumnsToContents();
		m_ui->historyTable->horizontalHeader()->setStretchLastSection(true);
		m_ui->historyTable->scrollToBottom();
	}

	void PartEditorDialog::done(int result)
	{
		// Arrow-clicking the spin box and closing at once never fires editingFinished, so the
		// pending count is committed here as well — a no-op when nothing changed.
		commitStockQuantity();
		// A keystroke less than the debounce interval old would otherwise be lost on close.
		if (m_saveTimer->isActive())
		{
			autosave();
		}
		QDialog::done(result);
	}

	void PartEditorDialog::reloadTags()
	{
		while (QLayoutItem* item = m_ui->tagChipsLayout->takeAt(0))
		{
			// reloadTags() is called *from* a chip's own clicked() handler, so the chips cannot
			// be deleted outright here — reparenting hides them now, deleteLater() frees them
			// once the signal that got us here has finished unwinding.
			if (QWidget* chip = item->widget())
			{
				chip->setParent(nullptr);
				chip->deleteLater();
			}
			delete item;
		}

		for (const Tag& tag : m_controller.partTags(m_part.id))
		{
			// §2d: name plus an × to remove it. The name is user data, the × is chrome.
			QPushButton* chip = new QPushButton(tr("%1 \xC3\x97", "tag chip with remove marker")
				.arg(toQt(tag.name)), m_ui->tagChipsHost);
			chip->setFlat(true);
			chip->setCursor(Qt::PointingHandCursor);
			chip->setStyleSheet(chipStyleSheet(toQt(tag.color)));
			chip->setToolTip(tr("Remove this tag from the part"));

			const int tagId = tag.id;
			connect(chip, &QPushButton::clicked, this, [this, tagId]()
			{
				m_controller.removePartTag(m_part.id, tagId);
				reloadTags();
			});
			m_ui->tagChipsLayout->addWidget(chip);
		}

		refreshAddTagMenu();
	}

	void PartEditorDialog::refreshAddTagMenu()
	{
		QMenu* menu = new QMenu(m_ui->addTagButton);
		const std::vector<Tag> available =
			availableTagsToAdd(m_controller.allTags(), m_controller.partTags(m_part.id));

		for (const Tag& tag : available)
		{
			const int tagId = tag.id;
			QAction* action = menu->addAction(toQt(tag.name)); // user data
			connect(action, &QAction::triggered, this, [this, tagId]()
			{
				m_controller.addPartTag(m_part.id, tagId);
				reloadTags();
			});
		}
		if (available.empty())
		{
			// §2d ships no built-in vocabulary, so a fresh database really has nothing to offer.
			QAction* empty = menu->addAction(tr("No tags left — create some in Manage Tags"));
			empty->setEnabled(false);
		}

		// setMenu() does not take ownership, so the previous menu has to go explicitly — but
		// we get here from one of ITS actions being triggered, so it has to outlive this call.
		QMenu* previous = m_ui->addTagButton->menu();
		m_ui->addTagButton->setMenu(menu);
		if (previous)
		{
			previous->deleteLater();
		}
	}

	void PartEditorDialog::updateMouserState()
	{
		const std::string number = m_controller.mouserPartNumber(m_part.id);
		// Guarded: this runs from loadPart(), and setText() would otherwise look like a user edit
		// and fire editingFinished on the way out of the field.
		const bool wasLoading = m_loading;
		m_loading = true;
		m_ui->mouserEdit->setText(QString::fromStdString(number));
		m_loading = wasLoading;

		m_ui->openMouserButton->setEnabled(!number.empty());
		m_ui->openMouserButton->setToolTip(number.empty()
			? tr("Enter this part's Mouser article number first — it is what the cart orders by, "
				 "and it is not the same as the MPN.")
			: tr("Opens %1 on mouser.com.").arg(QString::fromStdString(number)));
	}

	void PartEditorDialog::commitMouserPartNumber()
	{
		if (m_loading)
		{
			return;
		}
		const std::string number = m_ui->mouserEdit->text().trimmed().toStdString();
		if (number == m_controller.mouserPartNumber(m_part.id))
		{
			return;
		}
		// The stored product URL is carried over when the number is unchanged, and deliberately
		// dropped when it changes: a URL for the old article would open the wrong page.
		if (!m_controller.setMouserPartNumber(m_part.id, number))
		{
			QMessageBox::warning(this, tr("Could not save the Mouser part number"),
				tr("The database rejected the change."));
		}
		updateMouserState();
	}

	void PartEditorDialog::openOnMouser()
	{
		const std::string number = m_controller.mouserPartNumber(m_part.id);
		const std::string url = PartEditorController::mouserPageUrl(number,
			m_controller.mouserUrl(m_part.id));
		if (url.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
	}

}
