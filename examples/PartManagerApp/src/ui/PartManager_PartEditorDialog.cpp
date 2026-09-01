#include "ui/PartManager_PartEditorDialog.h"
#include "ui_PartManager_PartEditorDialog.h"

#include "widgets/PartManager_AttributeFormWidget.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
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
		connect(m_ui->stockSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartEditorDialog::scheduleSave);
		connect(m_ui->stockMinSpin, QOverload<int>::of(&QSpinBox::valueChanged),
			this, &PartEditorDialog::scheduleSave);
		// The generated form commits on focus-loss, which already is the debounce point.
		connect(m_attributeForm, &AttributeFormWidget::valueCommitted, this, &PartEditorDialog::autosave);

		connect(m_ui->openDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::openDatasheet);
		connect(m_ui->attachDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::attachDatasheet);
		connect(m_ui->downloadDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::downloadDatasheet);
		connect(m_ui->removeDatasheetButton, &QPushButton::clicked, this, &PartEditorDialog::removeDatasheet);

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
		m_ui->stockSpin->setValue(m_part.stockQty);
		m_ui->stockMinSpin->setValue(m_part.stockMinQty);

		m_attributeForm->setAttributes(m_controller.attributesFor(m_part.partTypeId));
		m_attributeForm->setValuesJson(toQt(m_part.attributes));

		reloadTags();
		updateDatasheetState();
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
		// ponytail: stock is edited straight on the cached part.stock_qty column. Ceiling is that
		// the change writes no stock_transaction row, so §3's history/"value of wealth" numbers
		// miss it; upgrade path is the Restock/Take Out screens once StockRepository exists.
		m_part.stockQty = m_ui->stockSpin->value();
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

	void PartEditorDialog::done(int result)
	{
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

}
