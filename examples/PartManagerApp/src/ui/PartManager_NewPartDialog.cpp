#include "ui/PartManager_NewPartDialog.h"
#include "ui_PartManager_NewPartDialog.h"

#include "widgets/PartManager_AttributeFormWidget.h"

#include <QMessageBox>
#include <QPushButton>
#include <algorithm>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}
	}

	NewPartDialog::NewPartDialog(DatabaseHandle* handle, QWidget* parent)
		: QDialog(parent)
		, m_ui(new Ui::NewPartDialog)
		, m_controller(handle)
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

	void NewPartDialog::setPrefill(const MouserPartPrefill& prefill)
	{
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
		m_ui->packageEdit->setText(toQt(prefill.part.package));
		m_ui->descriptionEdit->setPlainText(toQt(prefill.part.description));
		m_attributeForm->setValuesJson(toQt(prefill.part.attributes));

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

		// §11 also blocks Create on required *file slots*. There is no file store or attach
		// UI yet, so a required slot could never be satisfied and would make every part of
		// that type uncreatable — the slots are listed instead of enforced.
		// ponytail: file slots are informational only. Ceiling is that a "required" CAD model
		// is not actually enforced; upgrade path is core/filestore plus the editor's Files section.
		QStringList slotLabels;
		for (const PartTypeFileSlot& slot : m_controller.fileSlotsFor(typeId))
		{
			slotLabels.append(slot.required
				? tr("%1 (required)").arg(toQt(slot.label))
				: tr("%1 (optional)").arg(toQt(slot.label)));
		}
		m_ui->fileSlotsLabel->setText(slotLabels.isEmpty()
			? QString()
			: tr("Expected files, attachable once the part exists: %1").arg(slotLabels.join(tr(", "))));

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

	void NewPartDialog::createPart()
	{
		Part part;
		part.partTypeId = m_ui->typeCombo->currentData().toInt();
		part.name = m_ui->nameEdit->text().trimmed().toStdString();
		part.manufacturer = m_ui->manufacturerEdit->text().toStdString();
		part.mpn = m_ui->mpnEdit->text().toStdString();
		part.package = m_ui->packageEdit->text().toStdString();
		part.description = m_ui->descriptionEdit->toPlainText().toStdString();
		part.attributes = m_attributeForm->valuesJson().toStdString();

		m_createdPartId = m_controller.createPart(part);
		if (m_createdPartId == 0)
		{
			QMessageBox::warning(this, tr("Could not create part"),
				tr("The database rejected the new part."));
			return;
		}
		accept();
	}

}
