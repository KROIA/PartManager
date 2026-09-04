#include "ui/PartManager_MovePartDialog.h"

#include "controllers/PartManager_MainWindowController.h"   // formatAttributeValue()
#include "widgets/PartManager_AttributeFormWidget.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		bool declares(const std::vector<PartTypeAttribute>& attributes, const std::string& key)
		{
			return std::any_of(attributes.begin(), attributes.end(),
				[&key](const PartTypeAttribute& attribute) { return attribute.key == key; });
		}

		// The attributes the part has a value for that the target category does not declare. These
		// are the ones the move drops: the JSON is rewritten against the new type, and a key nothing
		// declares has nowhere to be stored and no editor to show it in.
		std::vector<PartTypeAttribute> lostAttributes(const Part& part,
			const std::vector<PartTypeAttribute>& sourceAttributes,
			const std::vector<PartTypeAttribute>& targetAttributes)
		{
			const std::map<std::string, AttributeValue> values =
				readAttributesJson(toQt(part.attributes), sourceAttributes);
			std::vector<PartTypeAttribute> lost;
			for (const PartTypeAttribute& attribute : sourceAttributes)
			{
				const auto found = values.find(attribute.key);
				if (found != values.end() && found->second.present
					&& !declares(targetAttributes, attribute.key))
				{
					lost.push_back(attribute);
				}
			}
			return lost;
		}
	}

	bool MovePartDialog::isCleanMove(PartEditorController& controller, const Part& part,
		int targetTypeId)
	{
		const std::vector<PartTypeAttribute> source = controller.attributesFor(part.partTypeId);
		const std::vector<PartTypeAttribute> target = controller.attributesFor(targetTypeId);
		if (!lostAttributes(part, source, target).empty())
		{
			return false;
		}
		// Values carry over by key, so what the part already holds is what the target would see.
		return missingRequiredKeys(target, readAttributesJson(toQt(part.attributes), target)).empty();
	}

	MovePartDialog::MovePartDialog(PartEditorController& controller, const Part& part,
		const PartType& targetType, QWidget* parent)
		: QDialog(parent)
		, m_part(part)
		, m_targetAttributes(controller.attributesFor(targetType.id))
		, m_form(new AttributeFormWidget(this))
		, m_missingLabel(new QLabel(this))
		, m_applyButton(nullptr)
	{
		const std::vector<PartTypeAttribute> sourceAttributes =
			controller.attributesFor(part.partTypeId);
		QString sourceName;
		for (const PartType& type : controller.types())
		{
			if (type.id == part.partTypeId)
			{
				sourceName = toQt(type.name);
			}
		}

		setWindowTitle(tr("Move part to another category"));
		QVBoxLayout* layout = new QVBoxLayout(this);

		QLabel* header = new QLabel(tr("Moving \"%1\" from %2 to %3.")
			.arg(toQt(m_part.name), sourceName, toQt(targetType.name)), this);
		header->setWordWrap(true);
		layout->addWidget(header);

		// What goes. Shown with the values it is about to lose rather than only the field names —
		// the decision is "am I willing to lose 4.7 kΩ", not "am I willing to lose a resistance".
		const std::vector<PartTypeAttribute> lost =
			lostAttributes(m_part, sourceAttributes, m_targetAttributes);
		if (!lost.empty())
		{
			QGroupBox* lostGroup = new QGroupBox(
				tr("These values are lost — %1 does not have these fields").arg(toQt(targetType.name)),
				this);
			QVBoxLayout* lostLayout = new QVBoxLayout(lostGroup);
			for (const PartTypeAttribute& attribute : lost)
			{
				PartColumn column;
				column.key = toQt(attribute.key);
				column.isAttribute = true;
				column.unit = toQt(attribute.unit);
				column.datatype = attribute.datatype;
				QLabel* line = new QLabel(QStringLiteral("• %1: %2")   // user data both sides
					.arg(toQt(attribute.label),
						formatAttributeValue(toQt(m_part.attributes), column)), lostGroup);
				line->setWordWrap(true);
				lostLayout->addWidget(line);
			}
			layout->addWidget(lostGroup);
		}

		// What stays and what still has to be answered, in one form: the fields the two categories
		// share arrive already filled, so the only empty ones are exactly the new category's own.
		QGroupBox* formGroup = new QGroupBox(tr("Fields in %1").arg(toQt(targetType.name)), this);
		QVBoxLayout* formLayout = new QVBoxLayout(formGroup);
		m_form->setAttributes(m_targetAttributes);
		m_form->setValuesJson(toQt(m_part.attributes));   // carries every shared key across
		formLayout->addWidget(m_form);
		QScrollArea* scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setWidget(formGroup);
		layout->addWidget(scroll, 1);

		m_missingLabel->setWordWrap(true);
		layout->addWidget(m_missingLabel);

		QDialogButtonBox* buttons = new QDialogButtonBox(this);
		m_applyButton = buttons->addButton(tr("Move part"), QDialogButtonBox::AcceptRole);
		buttons->addButton(QDialogButtonBox::Cancel);
		layout->addWidget(buttons);

		connect(m_form, &AttributeFormWidget::valueChanged, this, &MovePartDialog::updateApplyState);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		// By value: `targetType` is the caller's reference and does not outlive this constructor.
		const int targetTypeId = targetType.id;
		connect(m_applyButton, &QPushButton::clicked, this, [this, targetTypeId]()
			{
				m_part.partTypeId = targetTypeId;
				// Written against the *target's* attributes, which is what drops the lost keys:
				// writeAttributesJson only ever emits what the list it is given declares.
				m_part.attributes = m_form->valuesJson().toStdString();
				accept();
			});

		updateApplyState();
		resize(520, 480);
	}

	void MovePartDialog::updateApplyState()
	{
		const QStringList missing = m_form->missingRequiredLabels();
		m_applyButton->setEnabled(missing.isEmpty());
		m_missingLabel->setText(missing.isEmpty()
			? QString()
			: tr("Still needed before the part can move: %1").arg(missing.join(QStringLiteral(", "))));
	}

}
