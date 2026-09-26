#include "ui/PartManager_NewPartDialog.h"
#include "ui_PartManager_NewPartDialog.h"

#include "ui/PartManager_EcadFetchDialog.h"
#include "ui/PartManager_PartTypePickerDialog.h"

#include "domain/PartManager_PartTypeMatcher.h"

#include "mouser/PartManager_MouserClient.h"

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
		, m_handle(handle)
		, m_controller(handle)
		, m_stock(handle)
		, m_attributeForm(new AttributeFormWidget(this))
		, m_types(m_controller.types())
	{
		m_ui->setupUi(this);
		m_ui->attributeLayout->addWidget(m_attributeForm);

		connect(m_ui->typeButton, &QPushButton::clicked, this, &NewPartDialog::chooseType);
		connect(m_ui->nameEdit, &QLineEdit::textChanged, this, &NewPartDialog::revalidate);
		// Per keystroke, not per commit: Create has to already be enabled when it is clicked,
		// and clicking it is what would otherwise deliver the commit.
		connect(m_attributeForm, &AttributeFormWidget::valueChanged, this, &NewPartDialog::revalidate);
		connect(m_ui->createButton, &QPushButton::clicked, this, &NewPartDialog::createPart);
		connect(m_ui->cancelButton, &QPushButton::clicked, this, &NewPartDialog::reject);

		// §6's two Mouser actions on the article-number row. Both need a number to work on, so
		// they follow the field per keystroke rather than per commit — the same reason Create does.
		connect(m_ui->mouserEdit, &QLineEdit::textChanged, this, &NewPartDialog::updateMouserButtons);
		connect(m_ui->openMouserButton, &QPushButton::clicked, this, &NewPartDialog::openOnMouser);
		connect(m_ui->fetchMouserButton, &QPushButton::clicked, this, &NewPartDialog::fetchFromMouser);
		if (!MouserClient::hasApiKey())
		{
			// Said up front as well as on the click: a button that can only ever fail should not
			// have to be pressed before it says why.
			m_ui->fetchMouserButton->setToolTip(tr("No Mouser API key — set the %1 environment "
				"variable and restart the app.").arg(QLatin1String(MouserClient::ApiKeyEnvVar)));
		}
		updateMouserButtons();

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

		// Starts at "no category" rather than at whichever type sorts first. A preselected category
		// is a claim about the part that nobody made: it decides the attribute template, so the
		// wrong one silently gives the part the wrong fields. §11's gate covers it — Create stays
		// disabled until a real type is picked.
		setTypeId(NoParentType);
	}

	NewPartDialog::~NewPartDialog()
	{
		delete m_ui;
	}

	int NewPartDialog::createdPartId() const
	{
		return m_createdPartId;
	}

	bool NewPartDialog::hasType(int typeId) const
	{
		return std::any_of(m_types.begin(), m_types.end(),
			[typeId](const PartType& type) { return type.id == typeId; });
	}

	void NewPartDialog::setTypeId(int typeId)
	{
		m_typeId = hasType(typeId) ? typeId : NoParentType;

		// The full path, not the leaf name: "Ceramic Capacitor" alone hides that it inherits
		// everything "Capacitor" declares, and §2b is exactly what the choice is about. The id also
		// rides on the button as a property, so a test can read the state the widget is in rather
		// than parse its label back into an id.
		m_ui->typeButton->setText(m_typeId == NoParentType
			? tr("(none — select a category)")
			: partTypePath(m_types, m_typeId));   // user data
		m_ui->typeButton->setProperty("partTypeId", m_typeId);

		onTypeChanged();
	}

	void NewPartDialog::chooseType()
	{
		PartTypePickerDialog picker(m_handle, m_typeId, this);
		if (picker.exec() != QDialog::Accepted)
		{
			return;
		}
		// "No category" comes back as 0 through the same door a real type does — it is an answer
		// here, not a cancel, and the §11 gate is what stops it reaching the database.
		setTypeId(picker.selectedTypeId());
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

	void NewPartDialog::setPrefill(const MouserPartPrefill& prefill, PrefillSource source)
	{
		m_prefill = prefill;

		// Type first: changing it rebuilds the attribute form from scratch, which would throw away
		// any values written into it beforehand.
		//
		// Matched against the types actually in this database rather than by looking the vendor's
		// suggested name up as a list entry: that old route could only ever find a built-in template
		// under its original name, so a category the user made themselves never won however plainly
		// the vendor named it. A non-confident answer leaves "(none)" on purpose.
		bool typeMatched = false;
		const TypeMatch match = matchPartType(m_types, prefill.mouserCategory,
			prefill.part.description, prefill.suggestedTypeName);
		if (match.confident && match.typeId != NoParentType && hasType(match.typeId))
		{
			setTypeId(match.typeId);
			typeMatched = true;
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
		if (source == PrefillSource::ImportedList)
		{
			// Nothing on this form came from a vendor, so neither the Mouser sentence nor its
			// "no datasheet published" follow-up is true here. What the user needs told instead is
			// where these values did come from, and that a list line carries no category and no
			// files at all — both of which are still theirs to fill in.
			notes.append(tr("Filled in from the imported list — nothing here was checked against a "
				"catalogue, so correct anything that is wrong before creating the part."));
			if (!typeMatched)
			{
				notes.append(tr("An imported list carries no category — pick the part type yourself."));
			}
		}
		else
		{
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

	void NewPartDialog::setListDefaults(int stock, const QString& notes)
	{
		if (stock > 0)
		{
			m_ui->stockSpin->setValue(stock);
		}
		if (!notes.isEmpty())
		{
			const QString description = m_ui->descriptionEdit->toPlainText();
			m_ui->descriptionEdit->setPlainText(description.isEmpty()
				? notes
				: description + QLatin1Char('\n') + notes);
		}
		revalidate();
	}

	void NewPartDialog::updateMouserButtons()
	{
		const bool hasNumber = !m_ui->mouserEdit->text().trimmed().isEmpty();
		m_ui->openMouserButton->setEnabled(hasNumber);
		// Not disabled without an API key: "no key" is one of the three answers this button owes
		// the user, and a greyed-out button cannot give it. It is reported on the click instead.
		m_ui->fetchMouserButton->setEnabled(hasNumber);
	}

	void NewPartDialog::openOnMouser()
	{
		const std::string number = m_ui->mouserEdit->text().trimmed().toStdString();
		// The same rule createPart() writes the seller link by: the stored ProductDetailUrl is the
		// page for *that* article number, so it only applies while the field still holds it. A
		// number typed over it gets a search instead of somebody else's product page (§6).
		const std::string storedUrl = number == m_prefill.mouserPartNumber
			? m_prefill.productDetailUrl : std::string();
		const std::string url = PartEditorController::mouserPageUrl(number, storedUrl);
		if (url.empty())
		{
			return;
		}
		QDesktopServices::openUrl(QUrl(toQt(url)));
	}

	QStringList NewPartDialog::mergePrefill(const MouserPartPrefill& prefill, bool overwrite,
		bool dryRun)
	{
		QStringList conflicts;

		// One rule for every field below: empty is always filled in, equal is nothing to decide,
		// and anything else is typed work — named to the user and only replaced once they said so.
		const auto mergeLine = [&](QLineEdit* edit, const QString& label, const std::string& value)
		{
			const QString incoming = toQt(value).trimmed();
			if (incoming.isEmpty())
			{
				return;
			}
			const QString current = edit->text().trimmed();
			if (current == incoming)
			{
				return;
			}
			if (current.isEmpty())
			{
				if (!dryRun)
				{
					edit->setText(incoming);
				}
				return;
			}
			conflicts.append(label);
			if (overwrite && !dryRun)
			{
				edit->setText(incoming);
			}
		};

		// The type first, exactly as setPrefill() does it: changing it rebuilds the generated rows
		// from scratch, so any value written into them beforehand is thrown away.
		const TypeMatch match = matchPartType(m_types, prefill.mouserCategory,
			prefill.part.description, prefill.suggestedTypeName);
		const int matchedTypeId = match.confident && match.typeId != NoParentType
			&& hasType(match.typeId) ? match.typeId : NoParentType;
		// A non-confident match changes nothing — "(none — select a category)" is the honest answer
		// and is never traded for a guess (§6).
		const bool typeDiffers = matchedTypeId != NoParentType && matchedTypeId != m_typeId;
		const bool typeConflicting = typeDiffers && m_typeId != NoParentType;
		bool typeWillChange = false;
		if (typeDiffers)
		{
			if (typeConflicting)
			{
				conflicts.append(tr("Part type"));
			}
			if (!typeConflicting || overwrite)
			{
				typeWillChange = true;
				if (!dryRun)
				{
					setTypeId(matchedTypeId);
				}
			}
		}

		mergeLine(m_ui->nameEdit, tr("Name"), prefill.part.name);
		mergeLine(m_ui->manufacturerEdit, tr("Manufacturer"), prefill.part.manufacturer);
		mergeLine(m_ui->mpnEdit, tr("MPN"), prefill.part.mpn);
		mergeLine(m_ui->packageEdit, tr("Package"), prefill.part.package);

		const QString incomingDescription = toQt(prefill.part.description).trimmed();
		if (!incomingDescription.isEmpty())
		{
			const QString current = m_ui->descriptionEdit->toPlainText().trimmed();
			if (current.isEmpty())
			{
				if (!dryRun)
				{
					m_ui->descriptionEdit->setPlainText(incomingDescription);
				}
			}
			else if (current != incomingDescription)
			{
				conflicts.append(tr("Description"));
				if (overwrite && !dryRun)
				{
					m_ui->descriptionEdit->setPlainText(incomingDescription);
				}
			}
		}

		// The generated rows belong to the type rather than being a field of their own, so they are
		// not asked about twice: when the type moved they were just rebuilt empty and Mouser's
		// values simply fill them, and when the *type* is the thing in dispute the answer to that
		// one question already decides these.
		const QString incomingAttributes = toQt(prefill.part.attributes);
		if (!incomingAttributes.isEmpty() && incomingAttributes != QLatin1String("{}"))
		{
			const QString current = m_attributeForm->valuesJson();
			const bool currentEmpty = current.isEmpty() || current == QLatin1String("{}");
			if (typeWillChange || currentEmpty)
			{
				if (!dryRun)
				{
					m_attributeForm->setValuesJson(incomingAttributes);
				}
			}
			else if (current != incomingAttributes && !typeConflicting)
			{
				conflicts.append(tr("Type attributes"));
				if (overwrite && !dryRun)
				{
					m_attributeForm->setValuesJson(incomingAttributes);
				}
			}
		}

		// The two files Mouser publishes, queued exactly the way the URL… buttons queue one. See
		// fetchFromMouser() for why they stay queued rather than being downloaded here.
		const auto mergeUrl = [&](PartFileRole role, const QString& label, const std::string& value)
		{
			const QString incoming = toQt(value);
			if (incoming.isEmpty())
			{
				return;
			}
			const auto it = m_pending.find(role);
			const bool slotEmpty = it == m_pending.end()
				|| (it->second.localPath.isEmpty() && it->second.url.isEmpty());
			const auto apply = [&]()
			{
				m_pending[role] = PendingFile{ QString(), incoming };
				updateFileSlot(role);
			};
			if (slotEmpty)
			{
				if (!dryRun)
				{
					apply();
				}
				return;
			}
			if (it->second.url == incoming)
			{
				return;
			}
			conflicts.append(label);
			if (overwrite && !dryRun)
			{
				apply();
			}
		};
		mergeUrl(PartFileRole::Datasheet, tr("Datasheet"), prefill.datasheetUrl);
		mergeUrl(PartFileRole::Image, tr("Image"), prefill.imageUrl);

		return conflicts;
	}

	void NewPartDialog::fetchFromMouser()
	{
		const QString typed = m_ui->mouserEdit->text().trimmed();
		if (typed.isEmpty())
		{
			return;
		}

		// ---------------------------------------------------------------------------------------
		// This is the one place in the dialog that talks to the network before Create, and it is
		// deliberately limited to *metadata*. The header's invariant — nothing exists in the
		// database and nothing is copied or downloaded until Create, so Cancel discards completely
		// — still holds after a fetch: the datasheet and the product photo are queued into
		// m_pending as URLs, through the same mechanism the URL… buttons use, and only reach the
		// disk inside applyPendingFiles() once the part has an id. Cancel after a fetch leaves
		// nothing behind.
		// ---------------------------------------------------------------------------------------
		if (!MouserClient::hasApiKey())
		{
			m_ui->headerLabel->setText(tr("No Mouser API key — set the %1 environment variable and "
				"restart the app, then try again.").arg(QLatin1String(MouserClient::ApiKeyEnvVar)));
			return;
		}

		// MouserClient is synchronous: this call really does freeze the dialog for up to its
		// timeout. Said on the status line and shown in the cursor before it starts, the way
		// PartMigrationDialog does it, rather than leaving the window looking hung.
		m_ui->headerLabel->setText(tr("Looking %1 up on Mouser…").arg(typed));
		QApplication::setOverrideCursor(Qt::WaitCursor);
		// Repaint before the blocking call, or the status line above never reaches the screen.
		QApplication::processEvents();

		MouserClient client;
		const MouserSearchResult result = client.searchByPartNumber(typed.toStdString());
		QApplication::restoreOverrideCursor();

		if (!result.ok)
		{
			m_ui->headerLabel->setText(tr("Mouser could not be reached (%1) — nothing on the form "
				"was changed.").arg(toQt(result.errorMessage)));
			return;
		}
		if (result.parts.empty())
		{
			m_ui->headerLabel->setText(tr("Mouser knows no part %1 — check the article number, or "
				"fill the form in by hand.").arg(typed));
			return;
		}

		// The same ranking the search dialog applies: Mouser answers an article number with the
		// part itself *and* its packaging variants, and the first row it returns is not reliably
		// the one that was asked for.
		std::vector<MouserPartDto> parts = result.parts;
		MouserSearchService::rankByMatch(parts, typed.toStdString());
		const MouserPartPrefill prefill = MouserSearchService::toPrefill(parts.front());

		// Asked once, naming how many fields are at stake, rather than one question per field or a
		// silent clobber. No is the safe default: typed work survives and only the empty fields
		// are filled.
		const QStringList conflicts = mergePrefill(prefill, false, true);
		bool overwrite = false;
		if (!conflicts.isEmpty())
		{
			overwrite = QMessageBox::question(this, tr("Replace what you already typed?"),
				tr("Mouser has a different value for %n field(s):\n\n%1\n\n"
				   "Replace them with Mouser's? Choosing No keeps what you typed and only fills in "
				   "the fields that are still empty.", "", conflicts.size())
					.arg(conflicts.join(QStringLiteral("\n"))),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
		}
		mergePrefill(prefill, overwrite, false);

		// Kept whole, not merged: the product page and the price quote are facts about the article
		// that was just fetched whatever the user decided about the text fields, and createPart()
		// reads them off m_prefill. That same code only carries them over while the field still
		// holds `m_prefill.mouserPartNumber`, so the field has to end up holding Mouser's own
		// article number — the string typed to *find* the part is a query, not the answer, and a
		// hand-typed "LM358DR" would otherwise link nothing at all.
		m_prefill = prefill;
		const QString canonical = toQt(prefill.mouserPartNumber).trimmed();
		if (!canonical.isEmpty())
		{
			m_ui->mouserEdit->setText(canonical);
		}
		else
		{
			m_prefill.mouserPartNumber = typed.toStdString();
		}

		QStringList notes;
		notes.append(tr("Filled in from Mouser %1 — check every value before creating the part.")
			.arg(m_ui->mouserEdit->text().trimmed()));
		if (!conflicts.isEmpty() && !overwrite)
		{
			notes.append(tr("Kept what you typed in: %1.").arg(conflicts.join(tr(", "))));
		}
		if (m_typeId == NoParentType)
		{
			notes.append(tr("Mouser's category did not map to a type template — pick one yourself."));
		}
		if (prefill.datasheetUrl.empty())
		{
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
		const int typeId = m_typeId;
		if (typeId == NoParentType)
		{
			// "(none)" is not a type to ask the database about — it is the absence of one. The
			// generated form is emptied rather than left showing the previous type's rows, which
			// would otherwise be collected into `attributes` by a Create that never runs.
			m_attributeForm->setAttributes(std::vector<PartTypeAttribute>());
			m_ui->fileSlotsLabel->clear();
			revalidate();
			return;
		}
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
		// The type is a §11 required field like any other now, and it is listed rather than merely
		// enforced: a Create that is disabled with nothing to explain it is the state users read as
		// a broken dialog.
		if (m_typeId == NoParentType)
		{
			missing.prepend(tr("Part type"));
		}
		const bool hasName = !m_ui->nameEdit->text().trimmed().isEmpty();
		if (!hasName)
		{
			missing.prepend(tr("Name"));
		}

		m_ui->createButton->setEnabled(missing.isEmpty());
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
		part.partTypeId = m_typeId;
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
