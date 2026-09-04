#include "ui/PartManager_PartEditorDialog.h"
#include "ui_PartManager_PartEditorDialog.h"

#include "ui/PartManager_EcadFetchDialog.h"
#include "widgets/PartManager_AttributeFormWidget.h"
#include "widgets/PartManager_KeywordCheckList.h"
#include "widgets/PartManager_KicadPreviewWidget.h"
#include "widgets/PartManager_TypeIconPainter.h"
#include "search/PartManager_SearchEngine.h"

#include <fstream>
#include <iterator>

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
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

		// A tag's colour as a menu icon. The chips are coloured, so the menu that adds them has
		// to be too — otherwise picking "SPI" out of the Bus protocols submenu gives no hint
		// which of the blue shades is about to land on the part.
		QIcon tagSwatch(const std::string& color)
		{
			const QColor fill(toQt(color));
			if (!fill.isValid())
			{
				return QIcon();
			}
			QPixmap pixmap(12, 12);
			pixmap.fill(fill);
			return QIcon(pixmap);
		}

		// KiCad files are small (a few kB) and read on demand for the preview, so slurping is
		// fine and keeps the parser taking plain text rather than a stream.
		std::string readWholeFile(const std::string& path)
		{
			std::ifstream stream(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		// A failed download that has a URL behind it is nearly always a vendor CDN refusing an
		// automated request — and the browser it refuses to be is right there. Offering to open
		// it turns a dead end into "save it, then Attach file…", which does work.
		void reportDownloadFailure(QWidget* parent, const QString& title, const QString& reason,
			const QString& url)
		{
			QMessageBox box(QMessageBox::Warning, title,
				QObject::tr("%1\n\nThe part itself is unaffected.").arg(reason), QMessageBox::NoButton, parent);
			QPushButton* openButton = nullptr;
			if (!url.isEmpty())
			{
				box.setInformativeText(QObject::tr("Your browser is not blocked the way this "
					"download is. Open the link, save the file, then use “Attach file…”."));
				openButton = box.addButton(QObject::tr("Open in browser"), QMessageBox::ActionRole);
			}
			box.addButton(QMessageBox::Close);
			box.exec();
			if (openButton != nullptr && box.clickedButton() == openButton)
			{
				QDesktopServices::openUrl(QUrl(url));
			}
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

		// §11: the category's naming pattern applied to this part, and one button to take it.
		// A frame of its own above Identity, with the Name field moved into it, because the
		// suggestion and the field it fills are one thing and reading them apart makes the button
		// look like it acts on nothing.
		m_suggestedNameLabel = new QLabel(this);
		m_suggestedNameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		m_applyNameButton = new QPushButton(tr("Use suggested name"), this);
		m_applyNameButton->setToolTip(tr("Writes the name the category's pattern builds from this "
			"part's attributes into the Name field. The pattern is set in Edit Type Templates."));

		QGroupBox* nameGroup = new QGroupBox(tr("Component name"), this);
		QGridLayout* nameGrid = new QGridLayout(nameGroup);
		nameGrid->addWidget(m_applyNameButton, 0, 0);
		nameGrid->addWidget(m_suggestedNameLabel, 0, 1);
		// Out of the Identity form and into this one — takeRow() rather than a reparent, so the
		// row it came from closes up instead of leaving an empty one behind.
		QFormLayout::TakeRowResult nameRow = m_ui->identityLayout->takeRow(m_ui->nameEdit);
		delete nameRow.labelItem;
		delete nameRow.fieldItem;
		nameGrid->addWidget(m_ui->nameLabel, 1, 0);
		nameGrid->addWidget(m_ui->nameEdit, 1, 1);
		nameGrid->setColumnStretch(1, 1);
		m_ui->scrollLayout->insertWidget(0, nameGroup);

		// §7a: both halves of the same thing, so both live in one frame — the inherited words on
		// top because they are the ones that already apply, the part's own additions below.
		m_inheritedKeywords = new KeywordCheckList(this);
		m_inheritedKeywords->setEmptyText(
			tr("This category declares no search words yet — add them in Edit Type Templates "
			   "to give every part in it the same ones."));
		QGroupBox* searchWordsGroup = new QGroupBox(tr("Search words"), this);
		QFormLayout* searchWordsForm = new QFormLayout(searchWordsGroup);
		QFormLayout::TakeRowResult keywordRow = m_ui->identityLayout->takeRow(m_ui->searchKeywordsEdit);
		delete keywordRow.labelItem;
		delete keywordRow.fieldItem;
		m_ui->searchKeywordsLabel->setText(tr("This part's own"));
		searchWordsForm->addRow(tr("From the category"), m_inheritedKeywords);
		searchWordsForm->addRow(m_ui->searchKeywordsLabel, m_ui->searchKeywordsEdit);

		// The two quantities out of Identity and into a frame of their own: they are the only
		// fields here that describe the shelf rather than the part, and the history table further
		// down is about them and not about anything else in Identity.
		QGroupBox* stockGroup = new QGroupBox(tr("Stock"), this);
		QFormLayout* stockForm = new QFormLayout(stockGroup);
		for (QWidget* field : { static_cast<QWidget*>(m_ui->stockSpin),
			static_cast<QWidget*>(m_ui->stockMinSpin) })
		{
			QFormLayout::TakeRowResult row = m_ui->identityLayout->takeRow(field);
			QWidget* label = row.labelItem != nullptr ? row.labelItem->widget() : nullptr;
			delete row.labelItem;
			delete row.fieldItem;
			stockForm->addRow(label, field);
		}

		// Final order: what it is called, what it is, what it measures, how many are on the shelf,
		// and how to find it again — the type's own fields sit directly under the identity they
		// belong to rather than at the bottom past four file sections.
		m_ui->scrollLayout->removeWidget(m_ui->attributeGroup);
		m_ui->scrollLayout->insertWidget(2, m_ui->attributeGroup);
		m_ui->scrollLayout->insertWidget(3, stockGroup);
		m_ui->scrollLayout->insertWidget(4, searchWordsGroup);

		// Side by side under the KiCad rows: the symbol is what goes on the schematic, the
		// footprint what goes on the board, and seeing both at once is how you catch a vendor
		// zip that turned out to hold the wrong package.
		m_symbolPreview = new KicadPreviewWidget(this);
		m_footprintPreview = new KicadPreviewWidget(this);
		m_symbolPreview->setToolTip(tr("The schematic symbol this part places in KiCad."));
		m_footprintPreview->setToolTip(tr("The PCB footprint this part places in KiCad."));
		// Above the previews, because it is the button that fills them: hunting down a vendor ZIP
		// by hand is the slow path, not the first thing to offer.
		QPushButton* fetchEcadButton = new QPushButton(tr("Download symbol && footprint…"), this);
		fetchEcadButton->setToolTip(tr("Look the part up on EasyEDA, or watch for a library "
			"archive you download yourself."));
		m_ui->kicadLayout->addWidget(fetchEcadButton);
		connect(fetchEcadButton, &QPushButton::clicked, this, &PartEditorDialog::fetchEcadModel);

		QHBoxLayout* previewRow = new QHBoxLayout();
		previewRow->addWidget(m_symbolPreview);
		previewRow->addWidget(m_footprintPreview);
		m_ui->kicadLayout->addLayout(previewRow);

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
		connect(m_ui->searchKeywordsEdit, &QPlainTextEdit::textChanged, this, &PartEditorDialog::scheduleSave);
		connect(m_inheritedKeywords, &KeywordCheckList::excludedChanged, this, &PartEditorDialog::scheduleSave);
		// The suggestion follows the attribute values as they are typed, not only once they commit
		// — the point of showing it is to watch the name the part is about to get take shape.
		connect(m_attributeForm, &AttributeFormWidget::valueChanged,
			this, &PartEditorDialog::updateSuggestedName);
		connect(m_applyNameButton, &QPushButton::clicked, this, [this]()
			{
				// Taking the suggestion makes the button match the name and go disabled, and Qt
				// hands the focus a disabled widget gives up to the next one in the chain — which
				// the scroll area then scrolls into view, throwing the page somewhere else. Moving
				// the focus to the field being filled first keeps it where the user is looking.
				m_ui->nameEdit->setFocus(Qt::OtherFocusReason);
				m_ui->nameEdit->setText(m_suggestedName);   // textChanged carries it into the autosave
			});
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

		connect(m_ui->attachImageButton, &QPushButton::clicked, this, &PartEditorDialog::attachImage);
		connect(m_ui->downloadImageButton, &QPushButton::clicked, this, &PartEditorDialog::downloadImage);
		connect(m_ui->removeImageButton, &QPushButton::clicked, this, &PartEditorDialog::removeImage);

		connect(m_ui->importEcadButton, &QPushButton::clicked, this, &PartEditorDialog::importEcadArchive);
		connect(m_ui->attachSymbolButton, &QPushButton::clicked, this, &PartEditorDialog::attachKicadSymbol);
		connect(m_ui->removeSymbolButton, &QPushButton::clicked, this, &PartEditorDialog::removeKicadSymbol);
		connect(m_ui->attachFootprintButton, &QPushButton::clicked, this, &PartEditorDialog::attachKicadFootprint);
		connect(m_ui->removeFootprintButton, &QPushButton::clicked, this, &PartEditorDialog::removeKicadFootprint);

		connect(m_ui->deletePartButton, &QPushButton::clicked, this, &PartEditorDialog::deletePart);
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
			m_ui->imageGroup->setEnabled(false);
			m_ui->kicadGroup->setEnabled(false);
			m_ui->stockHistoryGroup->setEnabled(false);
			m_ui->deletePartButton->setEnabled(false);
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

		// §7a: this part's *own* extra search words, and — read-only underneath — the ones it
		// already answers to through its category. Showing the inherited list is what stops the
		// field being filled in with words the part already matches on.
		m_ui->searchKeywordsEdit->setPlainText(toQt(m_part.searchKeywords));
		m_ui->searchKeywordsEdit->setToolTip(
			tr("Extra words a search matches this part on, one per line. The category's own words "
			   "already apply and do not need repeating here."));
		// The inherited words are a tick list rather than text: they live on the category, so the
		// only thing this part can say about one of them is whether it applies here.
		m_inheritedKeywords->setKeywords(
			toQt(SearchEngine::inheritedKeywords(m_controller.types(), m_part.partTypeId)),
			toQt(m_part.excludedKeywords));

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
		updateImageState();
		updateKicadState();
		updateMouserState();
		reloadHistory();
		updateSuggestedName();
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
			reportDownloadFailure(this, tr("Could not download the datasheet"), toQt(error), url);
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

	void PartEditorDialog::updateImageState()
	{
		PartFile file;
		const bool attached = m_controller.roleFile(m_part.id, PartFileRole::Image, file);
		const std::string path = m_controller.roleFilePath(m_part.id, PartFileRole::Image);

		QPixmap pixmap;
		if (!path.empty())
		{
			pixmap.load(toQt(path));
		}
		if (pixmap.isNull())
		{
			// The same type placeholder the table and the main preview draw, so the editor does
			// not disagree with them about what a part with no photo looks like.
			std::string typeName;
			for (const PartType& type : m_controller.types())
			{
				if (type.id == m_part.partTypeId) { typeName = type.name; break; }
			}
			m_ui->imagePreviewLabel->setText(QString());
			m_ui->imagePreviewLabel->setPixmap(TypeIconPainter::icon(toQt(typeName),
				m_ui->imagePreviewLabel->maximumHeight(), devicePixelRatioF()));
		}
		else
		{
			m_ui->imagePreviewLabel->setPixmap(pixmap.scaled(m_ui->imagePreviewLabel->maximumWidth(),
				m_ui->imagePreviewLabel->maximumHeight(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
			m_ui->imagePreviewLabel->setText(QString());
		}

		if (!attached)
		{
			m_ui->imageStateLabel->setText(tr("No image attached yet."));
		}
		else if (path.empty())
		{
			// The file name is the user's own data; only the frame is translated.
			m_ui->imageStateLabel->setText(
				tr("%1 — the stored file is missing from this database's file store.")
					.arg(toQt(file.originalFilename)));
		}
		else if (pixmap.isNull())
		{
			// Attached and present, but it does not decode. Two very different causes, and the
			// message used to blame the wrong one: a format Qt has no plugin for (SVG, TIFF), or
			// — far more often — a "download" that stored a vendor CDN's HTML block page under
			// the image's name. Downloads reject that now, but files attached before they did are
			// still sitting in databases, so the state names both.
			m_ui->imageStateLabel->setText(
				tr("%1 — stored, but it will not display. Either the format needs a Qt image "
				   "plugin this build has not got, or the file is not really an image: a download "
				   "that was blocked by the vendor can save their web page under this name. "
				   "Remove it and attach the picture from disk.")
					.arg(toQt(file.originalFilename)));
		}
		else
		{
			m_ui->imageStateLabel->setText(tr("%1 (%2 × %3)").arg(toQt(file.originalFilename))
				.arg(pixmap.width()).arg(pixmap.height()));
		}

		m_ui->removeImageButton->setEnabled(attached);
		m_ui->attachImageButton->setText(attached ? tr("Replace file…") : tr("Attach file…"));
		m_ui->downloadImageButton->setText(attached ? tr("Replace from URL…") : tr("Download…"));
	}

	void PartEditorDialog::attachImage()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose an image"), QString(),
			tr("Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}

		std::string error;
		if (m_controller.attachRoleFile(m_part.id, PartFileRole::Image, path.toStdString(), &error) == 0)
		{
			QMessageBox::warning(this, tr("Could not attach the image"), toQt(error));
			return;
		}
		// No autosave: unlike the datasheet, nothing on `part` points at the image row.
		updateImageState();
	}

	void PartEditorDialog::downloadImage()
	{
		bool accepted = false;
		const QString url = QInputDialog::getText(this, tr("Download an image"), tr("Image URL"),
			QLineEdit::Normal, QString(), &accepted).trimmed();
		if (!accepted || url.isEmpty())
		{
			return;
		}

		// Synchronous with a timeout, so the window really does stop responding for a moment.
		m_ui->statusLabel->setText(tr("Downloading the image…"));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		std::string error;
		const int fileId = m_controller.downloadRoleFile(m_part.id, PartFileRole::Image,
			url.toStdString(), &error);
		QApplication::restoreOverrideCursor();
		m_ui->statusLabel->setText(tr("Changes are saved automatically."));

		if (fileId == 0)
		{
			reportDownloadFailure(this, tr("Could not download the image"), toQt(error), url);
			return;
		}
		updateImageState();
	}

	void PartEditorDialog::removeImage()
	{
		PartFile file;
		if (!m_controller.roleFile(m_part.id, PartFileRole::Image, file))
		{
			return;
		}
		if (QMessageBox::question(this, tr("Remove the image"),
			tr("Remove \"%1\" from this part?").arg(toQt(file.originalFilename)))
			!= QMessageBox::Yes)
		{
			return;
		}
		m_controller.detachRoleFile(m_part.id, PartFileRole::Image);
		updateImageState();
	}

	void PartEditorDialog::updateKicadState()
	{
		auto describe = [this](PartFileRole role, QLabel* label, const QString& emptyText)
		{
			PartFile file;
			if (!m_controller.roleFile(m_part.id, role, file))
			{
				label->setText(emptyText);
				return false;
			}
			const bool onDisk = !m_controller.roleFilePath(m_part.id, role).empty();
			// The file name is the vendor's own data; only the frame is translated.
			label->setText(onDisk
				? tr("%1 (%2)").arg(toQt(file.originalFilename),
					QLocale().formattedDataSize(file.sizeBytes))
				: tr("%1 — the stored file is missing from this database's file store.")
					.arg(toQt(file.originalFilename)));
			return true;
		};

		const bool hasSymbol = describe(PartFileRole::KicadSymbol, m_ui->kicadSymbolLabel,
			tr("Generated from the type template — a working symbol, but with no real pinout."));
		const bool hasFootprint = describe(PartFileRole::KicadFootprint, m_ui->kicadFootprintLabel,
			tr("None. Without one the symbol has no footprint to place."));
		describe(PartFileRole::Kicad3DModel, m_ui->kicadModelLabel,
			tr("None. Attach one in the 3D viewer, or import a vendor ZIP."));

		m_ui->removeSymbolButton->setEnabled(hasSymbol);
		m_ui->removeFootprintButton->setEnabled(hasFootprint);
		m_ui->attachSymbolButton->setText(hasSymbol ? tr("Replace…") : tr("Attach…"));
		m_ui->attachFootprintButton->setText(hasFootprint ? tr("Replace…") : tr("Attach…"));

		updateKicadPreviews();
	}

	void PartEditorDialog::updateKicadPreviews()
	{
		if (!m_symbolPreview || !m_footprintPreview) { return; }

		// The attached symbol when there is one; otherwise what "Generate Libraries" would
		// produce for this type, because that is what the user will actually get.
		const std::string symbolPath = m_controller.roleFilePath(m_part.id, PartFileRole::KicadSymbol);
		if (!symbolPath.empty())
		{
			const std::string text = readWholeFile(symbolPath);
			// By the part's name, because that is what the generator renames a vendor symbol to
			// when it adopts it. An unmatched name falls back to the file's own first symbol.
			const KicadDrawing drawing = KicadGeometry::symbol(text, m_part.name);
			m_symbolPreview->showDrawing(drawing,
				tr("The attached symbol file has nothing this preview can draw."));
			m_symbolPreview->setCaption(drawing.empty() ? QString() : toQt(drawing.name));
		}
		else
		{
			std::string typeName;
			for (const PartType& type : m_controller.types())
			{
				if (type.id == m_part.partTypeId) { typeName = type.name; break; }
			}
			m_symbolPreview->showDrawing(KicadGeometry::genericSymbolForType(typeName),
				tr("No symbol."));
			// Named as generated rather than by the part, so it is obvious this is a template
			// placeholder and not a real pinout somebody drew for this component.
			m_symbolPreview->setCaption(tr("generated — placeholder pinout"));
		}

		const std::string footprintPath =
			m_controller.roleFilePath(m_part.id, PartFileRole::KicadFootprint);
		if (!footprintPath.empty())
		{
			const KicadDrawing drawing = KicadGeometry::footprint(readWholeFile(footprintPath));
			m_footprintPreview->showDrawing(drawing,
				tr("The attached footprint file has nothing this preview can draw."));
			m_footprintPreview->setCaption(toQt(drawing.name));
		}
		else
		{
			m_footprintPreview->showMessage(tr("No footprint attached."));
			m_footprintPreview->setCaption(QString());
		}
	}

	void PartEditorDialog::fetchEcadModel()
	{
		EcadFetchDialog dialog(toQt(m_part.mpn), toQt(m_part.manufacturer),
			toQt(m_controller.mouserUrl(m_part.id)), this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}

		// A vendor archive carries a 3D model too, so it goes through the ZIP importer rather
		// than being taken apart twice.
		if (!dialog.archivePath().isEmpty())
		{
			QApplication::setOverrideCursor(Qt::WaitCursor);
			const PartEditorController::EcadImportSummary summary =
				m_controller.importEcadArchive(m_part.id, dialog.archivePath().toStdString());
			QApplication::restoreOverrideCursor();
			if (!summary.ok)
			{
				QMessageBox::warning(this, tr("Could not read the archive"),
					toQt(summary.errorMessage));
				return;
			}
			updateKicadState();
			return;
		}

		QStringList failures;
		const auto attach = [this, &failures](PartFileRole role, const QByteArray& bytes,
			const QString& filename)
			{
				if (bytes.isEmpty())
				{
					return;
				}
				std::string error;
				if (m_controller.attachRoleBytes(m_part.id, role,
					std::string(bytes.constData(), static_cast<size_t>(bytes.size())),
					filename.toStdString(), &error) == 0)
				{
					failures.append(toQt(error));
				}
			};
		attach(PartFileRole::KicadSymbol, dialog.symbolBytes(), dialog.symbolFilename());
		attach(PartFileRole::KicadFootprint, dialog.footprintBytes(), dialog.footprintFilename());

		updateKicadState();
		if (!failures.isEmpty())
		{
			QMessageBox::warning(this, tr("Not everything was attached"), failures.join('\n'));
		}
	}

	void PartEditorDialog::importEcadArchive()
	{
		const QString path = QFileDialog::getOpenFileName(this,
			tr("Choose a vendor library download"), QString(),
			tr("Library archives (*.zip);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}

		QApplication::setOverrideCursor(Qt::WaitCursor);
		const PartEditorController::EcadImportSummary summary =
			m_controller.importEcadArchive(m_part.id, path.toStdString());
		QApplication::restoreOverrideCursor();

		if (!summary.ok)
		{
			QMessageBox::warning(this, tr("Could not read the archive"),
				tr("%1\n\nA library download is a .zip — if you unpacked it already, use the "
				   "Attach buttons on the files inside instead.").arg(toQt(summary.errorMessage)));
			return;
		}

		QStringList taken;
		if (summary.symbolAttached)    { taken.append(tr("the schematic symbol")); }
		if (summary.footprintAttached) { taken.append(tr("the footprint")); }
		if (summary.modelAttached)     { taken.append(tr("the 3D model")); }

		if (taken.isEmpty())
		{
			// Saying nothing here is what makes an importer look broken — the archive plainly
			// had files in it, so the reason none of them was usable has to be given.
			QMessageBox::information(this, tr("Nothing to import"),
				summary.legacyKicadOnly
					? tr("This archive's KiCad folder holds only the old KiCad 5 format "
						 "(.lib/.dcm/.mod). PartManager writes .kicad_sym libraries and cannot mix "
						 "the two, so nothing was taken.\n\nDownload the KiCad 6+ version, or "
						 "convert it in KiCad and attach the result.")
					: tr("The archive has no KiCad files in it — %n entr(y/ies) for other CAD "
						 "tools were skipped.", "", summary.ignoredEntries));
			return;
		}

		updateKicadState();
		updateImageState();
		QMessageBox::information(this, tr("Imported"),
			tr("Took %1 out of the archive.\n\nThe files were copied into this database, so the "
			   "ZIP can be deleted. The symbol is what “Generate Libraries” will now put into "
			   "KiCad instead of the generic one.")
				.arg(taken.join(tr(", "))));
	}

	void PartEditorDialog::attachKicadSymbol()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose a KiCad symbol"),
			QString(), tr("KiCad symbol libraries (*.kicad_sym);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}
		std::string error;
		if (m_controller.attachRoleFile(m_part.id, PartFileRole::KicadSymbol, path.toStdString(),
			&error) == 0)
		{
			QMessageBox::warning(this, tr("Could not attach the symbol"), toQt(error));
			return;
		}
		updateKicadState();
	}

	void PartEditorDialog::removeKicadSymbol()
	{
		if (QMessageBox::question(this, tr("Remove the symbol"),
			tr("This part goes back to the generic generated symbol, and the next regeneration "
			   "replaces it in the KiCad library.\n\nAny edit you made in KiCad and that was "
			   "synced back into this file is lost with it. Remove it?"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		m_controller.detachRoleFile(m_part.id, PartFileRole::KicadSymbol);
		updateKicadState();
	}

	void PartEditorDialog::attachKicadFootprint()
	{
		const QString path = QFileDialog::getOpenFileName(this, tr("Choose a KiCad footprint"),
			QString(), tr("KiCad footprints (*.kicad_mod);;All files (*)"));
		if (path.isEmpty())
		{
			return;
		}
		std::string error;
		if (m_controller.attachRoleFile(m_part.id, PartFileRole::KicadFootprint, path.toStdString(),
			&error) == 0)
		{
			QMessageBox::warning(this, tr("Could not attach the footprint"), toQt(error));
			return;
		}
		updateKicadState();
	}

	void PartEditorDialog::removeKicadFootprint()
	{
		PartFile file;
		if (!m_controller.roleFile(m_part.id, PartFileRole::KicadFootprint, file))
		{
			return;
		}
		if (QMessageBox::question(this, tr("Remove the footprint"),
			tr("Remove \"%1\" from this part?").arg(toQt(file.originalFilename)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			return;
		}
		m_controller.detachRoleFile(m_part.id, PartFileRole::KicadFootprint);
		updateKicadState();
	}

	void PartEditorDialog::deletePart()
	{
		if (m_part.id == 0)
		{
			return;
		}

		// The stock history is the one thing here that exists nowhere else — a datasheet can be
		// downloaded again, a count that was built up over months cannot. So it is named, along
		// with the number itself, rather than hidden behind a generic "are you sure".
		const int quantity = m_stock.quantity(m_part.id);
		QMessageBox confirm(QMessageBox::Warning, tr("Delete this part?"),
			tr("“%1” will be removed from the database for good.").arg(toQt(m_part.name)),
			QMessageBox::NoButton, this);
		confirm.setInformativeText(tr(
			"This also deletes its stock history (%n unit(s) on record), its tags, its attached "
			"files and its Mouser link. Partlists that use this part keep their line, but it "
			"becomes unresolved.\n\nThis cannot be undone from inside the app — only by restoring "
			"a backup.", "", quantity));
		QPushButton* deleteButton = confirm.addButton(tr("Delete Part"), QMessageBox::DestructiveRole);
		confirm.addButton(QMessageBox::Cancel);
		confirm.setDefaultButton(QMessageBox::Cancel);
		confirm.exec();
		if (confirm.clickedButton() != deleteButton)
		{
			return;
		}

		if (!m_controller.deletePart(m_part.id))
		{
			QMessageBox::warning(this, tr("Could not delete the part"),
				tr("The database rejected the deletion — nothing was removed."));
			return;
		}

		// Blocks done()'s flush from writing the deleted record straight back in.
		m_deleted = true;
		m_loading = true;
		m_part.id = 0;
		accept();
	}

	void PartEditorDialog::updateSuggestedName()
	{
		// Against the widgets, not against m_part: the suggestion has to be right the moment a
		// package or an attribute is typed, which is up to 400 ms before the autosave writes it.
		Part current = m_part;
		current.manufacturer = m_ui->manufacturerEdit->text().toStdString();
		current.mpn = m_ui->mpnEdit->text().toStdString();
		current.package = m_ui->packageEdit->text().toStdString();
		current.attributes = m_attributeForm->valuesJson().toStdString();

		const QString pattern = nameTemplateFor(m_controller.types(), current.partTypeId);
		m_suggestedName = renderNameTemplate(pattern, current,
			m_controller.attributesFor(current.partTypeId));

		m_suggestedNameLabel->setText(m_suggestedName.isEmpty()
			? tr("This category has no naming pattern — set one in Edit Type Templates.")
			: m_suggestedName);   // user data
		m_suggestedNameLabel->setEnabled(!m_suggestedName.isEmpty());
		m_applyNameButton->setEnabled(!m_suggestedName.isEmpty()
			&& m_suggestedName != m_ui->nameEdit->text());
	}

	void PartEditorDialog::scheduleSave()
	{
		updateSuggestedName();
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
		m_part.searchKeywords = m_ui->searchKeywordsEdit->toPlainText().toStdString();
		m_part.excludedKeywords = m_inheritedKeywords->excludedKeywords().toStdString();
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
		// Newest first. The running quantities are computed forwards (each row needs the one
		// before it), so only the row the entry lands on is flipped.
		for (int entry = 0; entry < static_cast<int>(history.size()); ++entry)
		{
			const int row = static_cast<int>(history.size()) - 1 - entry;
			const StockTransaction& transaction = history[static_cast<size_t>(entry)];
			const int resulting = quantities[static_cast<size_t>(entry)];

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
		m_ui->historyTable->scrollToTop();
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

		// One submenu per tag family, same two levels the filter drop-down and Manage Tags show.
		// A flat list of the 31 seeded tags is a menu nobody can aim at, and it also hides which
		// family a tag belongs to — which is half of what the colour is telling you.
		QHash<int, QMenu*> submenus;
		for (const TagCategory& category : m_controller.tagCategories())
		{
			bool hasAny = false;
			for (const Tag& tag : available)
			{
				if (tag.categoryId == category.id) { hasAny = true; break; }
			}
			// A family whose tags the part already carries would otherwise be an empty submenu,
			// which looks broken and cannot be dismissed by clicking it.
			if (hasAny)
			{
				submenus.insert(category.id, menu->addMenu(toQt(category.name)));
			}
		}
		// Loose tags sit at the top level, below the families — they belong to no heading and
		// inventing one for them would be a lie about the vocabulary.
		if (!submenus.isEmpty())
		{
			bool hasLoose = false;
			for (const Tag& tag : available)
			{
				if (tag.categoryId == NoTagCategoryId) { hasLoose = true; break; }
			}
			if (hasLoose) { menu->addSeparator(); }
		}

		for (const Tag& tag : available)
		{
			const int tagId = tag.id;
			QMenu* owner = submenus.value(tag.categoryId, menu);
			QAction* action = owner->addAction(tagSwatch(tag.color), toQt(tag.name));
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
