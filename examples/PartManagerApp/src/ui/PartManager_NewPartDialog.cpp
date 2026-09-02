#include "ui/PartManager_NewPartDialog.h"
#include "ui_PartManager_NewPartDialog.h"

#include "ui/PartManager_EcadFetchDialog.h"

#include "widgets/PartManager_AttributeFormWidget.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
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

		// The last path segment of a URL, for the slot's state label. A Mouser image URL is
		// long enough to push every button off the edge of the dialog if shown whole.
		QString urlLabel(const QString& url)
		{
			const QString name = QUrl(url).fileName();
			return name.isEmpty() ? url : name;
		}
	}

	NewPartDialog::NewPartDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::NewPartDialog)
		, m_controller(handle)
		, m_stock(handle)
		, m_attributeForm(new AttributeFormWidget(this))
	{
		m_ui->setupUi(this);
		m_ui->attributeLayout->addWidget(m_attributeForm);

		std::vector<PartType> types = m_controller.types();
		std::sort(types.begin(), types.end(),
			[](const PartType& a, const PartType& b) { return a.name < b.name; });
		for (const PartType& type : types)
		{
			m_ui->typeCombo->addItem(toQt(type.name), type.id); // user data
		}

		connect(m_ui->typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
			this, &NewPartDialog::onTypeChanged);
		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &NewPartDialog::revalidate);
		// Per keystroke, not per commit: Create has to already be enabled when it is clicked,
		// and clicking it is what would otherwise deliver the commit.
		connect(m_attributeForm, &AttributeFormWidget::valueChanged, this, &NewPartDialog::revalidate);
		connect(m_ui->createButton, &QPushButton::clicked, this, &NewPartDialog::createPart);
		connect(m_ui->cancelButton, &QPushButton::clicked, this, &NewPartDialog::reject);

		// PDF-biased, not PDF-only: plenty of real datasheets arrive as a scan or a zip.
		wireFileSlot(PartFileRole::Datasheet, m_ui->datasheetStateLabel, m_ui->datasheetFileButton,
			m_ui->datasheetUrlButton, m_ui->datasheetClearButton,
			tr("Datasheets (*.pdf);;All files (*)"));
		wireFileSlot(PartFileRole::Image, m_ui->imageStateLabel, m_ui->imageFileButton,
			m_ui->imageUrlButton, m_ui->imageClearButton,
			tr("Images (*.png *.jpg *.jpeg *.gif *.bmp *.webp);;All files (*)"));
		// No URL button: the 3D model has no URL a user could paste. It arrives inside a vendor
		// ZIP, which the §5c download step (fetchEcadModel) unpacks after Create.
		wireFileSlot(PartFileRole::Kicad3DModel, m_ui->modelStateLabel, m_ui->modelFileButton,
			nullptr, m_ui->modelClearButton,
			tr("3D models (*.step *.stp *.obj *.stl *.ply *.wrl *.gltf *.glb);;All files (*)"));

		onTypeChanged();
	}

	NewPartDialog::~NewPartDialog()
	{
		delete m_ui;
	}

	int NewPartDialog::createdPartId() const
	{
		return m_createdPartId;
	}

	void NewPartDialog::wireFileSlot(PartFileRole role, QLabel* state, QPushButton* fileButton,
		QPushButton* urlButton, QPushButton* clearButton, const QString& filter)
	{
		m_slotLabels[role] = state;

		connect(fileButton, &QPushButton::clicked, this, [this, role, filter]()
		{
			const QString path = QFileDialog::getOpenFileName(this, tr("Choose a file"), QString(), filter);
			if (path.isEmpty())
			{
				return;
			}
			// A file and a URL are alternatives, not a pair — picking one clears the other.
			m_pending[role] = PendingFile{ path, QString() };
			updateFileSlot(role);
		});

		if (urlButton)
		{
			connect(urlButton, &QPushButton::clicked, this, [this, role]()
			{
				bool accepted = false;
				const QString url = QInputDialog::getText(this, tr("Download from a URL"), tr("URL"),
					QLineEdit::Normal, m_pending[role].url, &accepted).trimmed();
				if (!accepted)
				{
					return;
				}
				m_pending[role] = PendingFile{ QString(), url };
				updateFileSlot(role);
			});
		}

		connect(clearButton, &QPushButton::clicked, this, [this, role]()
		{
			m_pending.erase(role);
			updateFileSlot(role);
		});

		updateFileSlot(role);
	}

	void NewPartDialog::updateFileSlot(PartFileRole role)
	{
		QLabel* state = m_slotLabels[role];
		if (!state)
		{
			return;
		}

		auto it = m_pending.find(role);
		if (it == m_pending.end() || (it->second.localPath.isEmpty() && it->second.url.isEmpty()))
		{
			state->setText(tr("None"));
			state->setToolTip(QString());
			return;
		}

		if (!it->second.localPath.isEmpty())
		{
			// The name is the user's own file; only the frame is translated.
			state->setText(tr("File: %1").arg(QFileInfo(it->second.localPath).fileName()));
			state->setToolTip(it->second.localPath);
		}
		else
		{
			state->setText(tr("Download: %1").arg(urlLabel(it->second.url)));
			state->setToolTip(it->second.url);
		}
	}

	void NewPartDialog::setPrefill(const MouserPartPrefill& prefill)
	{
		m_prefill = prefill;

		// Type first: switching the combo rebuilds the attribute form from scratch, which would
		// throw away any values written into it beforehand.
		bool typeMatched = false;
		if (!prefill.suggestedTypeName.empty())
		{
			const int index = m_ui->typeCombo->findText(toQt(prefill.suggestedTypeName));
			if (index >= 0)
			{
				m_ui->typeCombo->setCurrentIndex(index);
				typeMatched = true;
			}
		}

		m_ui->nameEdit->setText(toQt(prefill.part.name));
		m_ui->manufacturerEdit->setText(toQt(prefill.part.manufacturer));
		m_ui->mpnEdit->setText(toQt(prefill.part.mpn));
		m_ui->mouserEdit->setText(toQt(prefill.mouserPartNumber));
		m_ui->packageEdit->setText(toQt(prefill.part.package));
		m_ui->descriptionEdit->setPlainText(toQt(prefill.part.description));
		m_attributeForm->setValuesJson(toQt(prefill.part.attributes));

		// §6: everything Mouser publishes as a file is queued here, so Create is genuinely the
		// last step. Both are still visible and clearable — a prefill is a suggestion, not a fact.
		if (!prefill.datasheetUrl.empty())
		{
			m_pending[PartFileRole::Datasheet] = PendingFile{ QString(), toQt(prefill.datasheetUrl) };
		}
		if (!prefill.imageUrl.empty())
		{
			m_pending[PartFileRole::Image] = PendingFile{ QString(), toQt(prefill.imageUrl) };
		}
		updateFileSlot(PartFileRole::Datasheet);
		updateFileSlot(PartFileRole::Image);

		QStringList notes;
		notes.append(prefill.mouserPartNumber.empty()
			? tr("Prefilled from Mouser — check every value before creating the part.")
			: tr("Prefilled from Mouser %1 — check every value before creating the part.")
				.arg(toQt(prefill.mouserPartNumber)));
		if (!typeMatched)
		{
			// Better to say nothing than to attach a wrong template silently (§6).
			notes.append(tr("Mouser's category did not map to a type template — pick one yourself."));
		}
		if (prefill.datasheetUrl.empty())
		{
			// Common enough to be worth naming: it looks like the import lost the datasheet.
			notes.append(tr("Mouser publishes no datasheet link for this part — attach one yourself."));
		}
		if (!prefill.unmappedAttributes.empty())
		{
			QStringList unmapped;
			for (const std::string& name : prefill.unmappedAttributes)
			{
				unmapped.append(toQt(name));
			}
			notes.append(tr("Not filled in automatically: %1.").arg(unmapped.join(tr(", "))));
		}
		m_ui->headerLabel->setText(notes.join(QStringLiteral("\n")));

		revalidate();
	}

	void NewPartDialog::onTypeChanged()
	{
		const int typeId = m_ui->typeCombo->currentData().toInt();
		m_attributeForm->setAttributes(m_controller.attributesFor(typeId));

		// §11 also blocks Create on required *file slots*. The three built-in slots above cover
		// the roles a type template can declare in practice; a template asking for something else
		// still gets listed rather than enforced, since there is no field here to satisfy it with.
		// ponytail: still informational. Ceiling is that a "required" slot outside the three
		// built-ins is not enforced; upgrade path is generating a slot row per declared role.
		QStringList slotLabels;
		for (const PartTypeFileSlot& slot : m_controller.fileSlotsFor(typeId))
		{
			slotLabels.append(slot.required
				? tr("%1 (required)").arg(toQt(slot.label))
				: tr("%1 (optional)").arg(toQt(slot.label)));
		}
		m_ui->fileSlotsLabel->setText(slotLabels.isEmpty()
			? QString()
			: tr("This type expects: %1").arg(slotLabels.join(tr(", "))));

		revalidate();
	}

	void NewPartDialog::revalidate()
	{
		QStringList missing = m_attributeForm->missingRequiredLabels();
		const bool hasName = !m_ui->nameEdit->text().trimmed().isEmpty();
		if (!hasName)
		{
			missing.prepend(tr("Name"));
		}

		m_ui->createButton->setEnabled(missing.isEmpty() && m_ui->typeCombo->count() > 0);
		m_ui->validationLabel->setText(missing.isEmpty()
			? QString()
			: tr("Still required: %1").arg(missing.join(tr(", "))));
	}

	void NewPartDialog::fetchEcadModel(const Part& part)
	{
		if (part.mpn.empty())
		{
			return;
		}

		// Offered rather than done silently: a converted footprint decides how the part solders,
		// so the user sees it drawn before it is attached. EcadFetchDialog starts the EasyEDA
		// lookup itself and only shows the manual-download route when that misses.
		EcadFetchDialog dialog(toQt(part.mpn), toQt(part.manufacturer),
			toQt(m_prefill.productDetailUrl), this);
		if (dialog.exec() != QDialog::Accepted)
		{
			return;
		}

		if (!dialog.archivePath().isEmpty())
		{
			m_controller.importEcadArchive(m_createdPartId, dialog.archivePath().toStdString());
			return;
		}
		const auto attach = [this](PartFileRole role, const QByteArray& bytes, const QString& filename)
			{
				if (!bytes.isEmpty())
				{
					m_controller.attachRoleBytes(m_createdPartId, role,
						std::string(bytes.constData(), static_cast<size_t>(bytes.size())),
						filename.toStdString());
				}
			};
		attach(PartFileRole::KicadSymbol, dialog.symbolBytes(), dialog.symbolFilename());
		attach(PartFileRole::KicadFootprint, dialog.footprintBytes(), dialog.footprintFilename());
	}

	void NewPartDialog::applyPendingFiles(int partId, Part& part)
	{
		QStringList failures;
		QStringList failedUrls;
		for (const auto& entry : m_pending)
		{
			const PartFileRole role = entry.first;
			const PendingFile& pending = entry.second;
			std::string error;
			int fileId = 0;

			if (!pending.localPath.isEmpty())
			{
				fileId = m_controller.attachRoleFile(partId, role, pending.localPath.toStdString(), &error);
			}
			else if (!pending.url.isEmpty())
			{
				fileId = m_controller.downloadRoleFile(partId, role, pending.url.toStdString(), &error);
			}
			else
			{
				continue;
			}

			if (fileId == 0)
			{
				failures.append(tr("%1: %2").arg(m_slotLabels[role] ? m_slotLabels[role]->text() : QString(),
					toQt(error)));
				if (!pending.url.isEmpty())
				{
					failedUrls.append(pending.url);
				}
				continue;
			}
			// The datasheet is the one slot with a column on `part` pointing at it, so the
			// record has to learn about it — the others are found by role.
			if (role == PartFileRole::Datasheet)
			{
				part.datasheetFileId = fileId;
			}
		}

		if (failures.isEmpty())
		{
			return;
		}

		// The part itself exists and is fine — §6 is explicit that a dead vendor URL must not
		// block creating it. Said once, at the end, rather than one modal per slot.
		QMessageBox box(QMessageBox::Warning, tr("Some files could not be attached"),
			tr("The part was created. These files were not:\n\n%1")
				.arg(failures.join(QStringLiteral("\n"))), QMessageBox::NoButton, this);
		QPushButton* openButton = nullptr;
		if (failedUrls.isEmpty())
		{
			box.setInformativeText(tr("You can attach them by hand in the part editor."));
		}
		else
		{
			// Mouser's CDN blocks automated downloads but not browsers, so this is the actual
			// route to the file rather than a consolation prize.
			box.setInformativeText(tr("Your browser is not blocked the way these downloads are. "
				"Open the links, save the files, then attach them in the part editor."));
			openButton = box.addButton(tr("Open %n link(s) in browser", "", failedUrls.size()),
				QMessageBox::ActionRole);
		}
		box.addButton(QMessageBox::Close);
		box.exec();
		if (openButton != nullptr && box.clickedButton() == openButton)
		{
			for (const QString& url : failedUrls)
			{
				QDesktopServices::openUrl(QUrl(url));
			}
		}
	}

	void NewPartDialog::createPart()
	{
		Part part;
		part.partTypeId = m_ui->typeCombo->currentData().toInt();
		part.name = m_ui->nameEdit->text().trimmed().toStdString();
		part.manufacturer = m_ui->manufacturerEdit->text().toStdString();
		part.mpn = m_ui->mpnEdit->text().toStdString();
		part.package = m_ui->packageEdit->text().toStdString();
		part.description = m_ui->descriptionEdit->toPlainText().toStdString();
		part.stockMinQty = m_ui->stockMinSpin->value();
		part.attributes = m_attributeForm->valuesJson().toStdString();
		// Not written here: the transaction log is the source of truth for the quantity (§3), so
		// the opening count goes in as a restock below and the cache follows from it.

		// A second row for a part that already exists is the quiet failure this catches: the
		// duplicate looks fine, but a partlist can resolve to either copy, and the one it picks
		// may be the one with no Mouser link, no datasheet and the wrong stock count. Warned
		// rather than blocked — two genuinely different parts can share an MPN across
		// manufacturers, and only the user knows which case this is.
		const std::vector<Part> existing = m_controller.partsWithMpn(part.mpn);
		if (!existing.empty())
		{
			QStringList lines;
			for (const Part& other : existing)
			{
				lines.append(tr("“%1” — %n in stock", "", other.stockQty)
					.arg(QString::fromStdString(other.name)));
			}
			if (QMessageBox::question(this, tr("That part number already exists"),
				tr("MPN “%1” is already used by:\n\n%2\n\n"
				   "Creating a second one means a partlist can match either copy, and stock is "
				   "counted separately for each. Edit the existing part instead unless this is "
				   "genuinely a different component.\n\nCreate it anyway?")
					.arg(QString::fromStdString(part.mpn))
					.arg(lines.join(QStringLiteral("\n"))),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			{
				return;
			}
		}

		m_createdPartId = m_controller.createPart(part);
		if (m_createdPartId == 0)
		{
			QMessageBox::warning(this, tr("Could not create part"),
				tr("The database rejected the new part."));
			return;
		}
		part.id = m_createdPartId;

		// §3/§6: the part now exists, so the Mouser article number and the quote it was created
		// from finally have somewhere to live. Until this ran, a part prefilled from Mouser kept
		// no trace of where it came from and could never be staged into a cart. The number is read
		// off the field rather than out of the prefill, because it is editable — and a hand-typed
		// one has no product page, so the URL only carries over when the number is untouched.
		const std::string mouserNumber = m_ui->mouserEdit->text().trimmed().toStdString();
		if (!mouserNumber.empty())
		{
			const bool unchanged = mouserNumber == m_prefill.mouserPartNumber;
			m_controller.linkToMouser(m_createdPartId, mouserNumber,
				unchanged ? m_prefill.productDetailUrl : std::string(),
				unchanged ? m_prefill.priceBreaks : std::vector<PriceObservation>());
		}

		// Downloads block with a timeout, so the dialog really does stop responding for a moment.
		QApplication::setOverrideCursor(Qt::WaitCursor);
		applyPendingFiles(m_createdPartId, part);
		QApplication::restoreOverrideCursor();
		if (part.datasheetFileId != 0)
		{
			m_controller.savePart(part);
		}

		// §3: an opening count is a restock, not a column write — otherwise the history starts
		// out disagreeing with the number beside it.
		if (m_ui->stockSpin->value() > 0)
		{
			m_stock.restock(m_createdPartId, m_ui->stockSpin->value(), tr("Initial stock"));
		}

		fetchEcadModel(part);
		accept();
	}

}
