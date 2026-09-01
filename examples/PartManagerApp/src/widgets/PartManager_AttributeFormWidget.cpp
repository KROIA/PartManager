#include "widgets/PartManager_AttributeFormWidget.h"

#include "widgets/PartManager_DimensionLineEdit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

namespace PartManager
{
	namespace
	{
		QString toQt(const std::string& text)
		{
			return QString::fromStdString(text);
		}

		// "Resistance (Ω) * ⓘ" — the label and unit are user/vocabulary data, only the
		// required marker and the tooltip hint belong to the app's own chrome.
		QString rowLabel(const PartTypeAttribute& attribute)
		{
			QString label = toQt(attribute.label);
			if (!attribute.unit.empty())
			{
				label += QStringLiteral(" (%1)").arg(toQt(attribute.unit));
			}
			if (attribute.required)
			{
				label = QObject::tr("%1 *", "required attribute").arg(label);
			}
			if (!attribute.tooltip.empty())
			{
				label = QObject::tr("%1 \xE2\x93\x98", "attribute with a tooltip").arg(label);
			}
			return label;
		}
	}

	AttributeFormWidget::AttributeFormWidget(QWidget* parent)
		: QWidget(parent)
		, m_layout(new QFormLayout(this))
	{
		m_layout->setContentsMargins(0, 0, 0, 0);
		m_layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	}

	void AttributeFormWidget::setAttributes(const std::vector<PartTypeAttribute>& attributes)
	{
		m_rows.clear();
		// Deleting the widgets is what empties the layout — takeAt() alone leaves them
		// parented here and still painted. Each row is one label plus one field container,
		// so deleting those two takes the editor and hint inside them with it.
		while (QLayoutItem* item = m_layout->takeAt(0))
		{
			delete item->widget();
			delete item;
		}

		for (const PartTypeAttribute& attribute : attributes)
		{
			Row row;
			row.attribute = attribute;
			row.hint = new QLabel(this);
			row.hint->setEnabled(false);   // a hint, never a control
			row.editor = createEditor(attribute, row.hint);

			QLabel* label = new QLabel(rowLabel(attribute), this);
			if (!attribute.tooltip.empty())
			{
				// §11's multiline (i) hint — user-authored text, so never translated.
				label->setToolTip(toQt(attribute.tooltip));
				row.editor->setToolTip(toQt(attribute.tooltip));
			}

			QWidget* field = new QWidget(this);
			QHBoxLayout* fieldLayout = new QHBoxLayout(field);
			fieldLayout->setContentsMargins(0, 0, 0, 0);
			fieldLayout->addWidget(row.editor, 1);
			fieldLayout->addWidget(row.hint);

			m_layout->addRow(label, field);
			m_rows.push_back(row);
		}
	}

	QWidget* AttributeFormWidget::createEditor(const PartTypeAttribute& attribute, QLabel* hint)
	{
		auto emitChanged = [this]()
		{
			if (!m_loading)
			{
				emit valueChanged();
			}
		};
		auto emitCommitted = [this]()
		{
			if (!m_loading)
			{
				emit valueCommitted();
			}
		};

		switch (widgetKindFor(attribute))
		{
		case AttributeWidgetKind::Dimension:
		{
			DimensionLineEdit* edit = new DimensionLineEdit(this);
			edit->setUnit(toQt(attribute.unit));
			connect(edit, &DimensionLineEdit::hintChanged, hint, &QLabel::setText);
			connect(edit, &QLineEdit::textChanged, this, emitChanged);
			connect(edit, &DimensionLineEdit::committed, this, emitCommitted);
			return edit;
		}
		case AttributeWidgetKind::Number:
		{
			QLineEdit* edit = new QLineEdit(this);
			// Plain count/ratio field — no SI prefixes here, that is what Dimension is for (§2a).
			QDoubleValidator* validator = new QDoubleValidator(edit);
			validator->setNotation(QDoubleValidator::StandardNotation);
			validator->setLocale(QLocale::c());
			edit->setValidator(validator);
			connect(edit, &QLineEdit::textChanged, this, emitChanged);
			connect(edit, &QLineEdit::editingFinished, this, emitCommitted);
			return edit;
		}
		case AttributeWidgetKind::Bool:
		{
			QCheckBox* box = new QCheckBox(this);
			connect(box, &QCheckBox::toggled, this, emitChanged);
			connect(box, &QCheckBox::toggled, this, emitCommitted);
			return box;
		}
		case AttributeWidgetKind::Enum:
		{
			QComboBox* combo = new QComboBox(this);
			// A blank first entry so "not chosen yet" stays expressible — a required enum
			// otherwise looks filled the moment the form is built.
			combo->addItem(QString());
			for (const std::string& option : attribute.enumOptions)
			{
				combo->addItem(toQt(option)); // user-defined option — not translated
			}
			connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, emitChanged);
			connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, emitCommitted);
			return combo;
		}
		case AttributeWidgetKind::Text:
		default:
		{
			QLineEdit* edit = new QLineEdit(this);
			connect(edit, &QLineEdit::textChanged, this, emitChanged);
			connect(edit, &QLineEdit::editingFinished, this, emitCommitted);
			return edit;
		}
		}
	}

	void AttributeFormWidget::setValuesJson(const QString& attributesJson)
	{
		std::vector<PartTypeAttribute> attributes;
		for (const Row& row : m_rows)
		{
			attributes.push_back(row.attribute);
		}
		std::map<std::string, AttributeValue> values = readAttributesJson(attributesJson, attributes);

		m_loading = true;
		for (const Row& row : m_rows)
		{
			const AttributeValue& value = values[row.attribute.key];
			switch (widgetKindFor(row.attribute))
			{
			case AttributeWidgetKind::Dimension:
			{
				DimensionLineEdit* edit = static_cast<DimensionLineEdit*>(row.editor);
				if (value.present)
				{
					edit->setValue(value.number);
				}
				else
				{
					edit->clearValue();
				}
				break;
			}
			case AttributeWidgetKind::Number:
				static_cast<QLineEdit*>(row.editor)->setText(
					value.present ? QString::number(value.number, 'g', 10) : QString());
				break;
			case AttributeWidgetKind::Bool:
				static_cast<QCheckBox*>(row.editor)->setChecked(value.present && value.flag);
				break;
			case AttributeWidgetKind::Enum:
			{
				QComboBox* combo = static_cast<QComboBox*>(row.editor);
				int index = value.present ? combo->findText(value.text) : 0;
				// A value the template no longer offers would otherwise vanish silently on save.
				if (index < 0)
				{
					combo->addItem(value.text);
					index = combo->count() - 1;
				}
				combo->setCurrentIndex(index);
				break;
			}
			case AttributeWidgetKind::Text:
			default:
				static_cast<QLineEdit*>(row.editor)->setText(value.present ? value.text : QString());
				break;
			}
		}
		m_loading = false;
	}

	std::map<std::string, AttributeValue> AttributeFormWidget::currentValues() const
	{
		std::map<std::string, AttributeValue> values;
		for (const Row& row : m_rows)
		{
			AttributeValue value;
			switch (widgetKindFor(row.attribute))
			{
			case AttributeWidgetKind::Dimension:
			{
				const DimensionLineEdit* edit = static_cast<const DimensionLineEdit*>(row.editor);
				// An unparsable entry is left out rather than stored as 0 — the field stays
				// flagged red so the user can see why nothing was written.
				value.present = edit->hasValue();
				value.number = edit->value();
				break;
			}
			case AttributeWidgetKind::Number:
			{
				const QString text = static_cast<const QLineEdit*>(row.editor)->text().trimmed();
				bool ok = false;
				const double parsed = text.toDouble(&ok);
				value.present = ok;
				value.number = ok ? parsed : 0.0;
				break;
			}
			case AttributeWidgetKind::Bool:
				value.present = true;   // a checkbox always has an answer
				value.flag = static_cast<const QCheckBox*>(row.editor)->isChecked();
				break;
			case AttributeWidgetKind::Enum:
				value.text = static_cast<const QComboBox*>(row.editor)->currentText();
				value.present = !value.text.isEmpty();
				break;
			case AttributeWidgetKind::Text:
			default:
				value.text = static_cast<const QLineEdit*>(row.editor)->text();
				value.present = !value.text.isEmpty();
				break;
			}
			values[row.attribute.key] = value;
		}
		return values;
	}

	QString AttributeFormWidget::valuesJson() const
	{
		std::vector<PartTypeAttribute> attributes;
		for (const Row& row : m_rows)
		{
			attributes.push_back(row.attribute);
		}
		return writeAttributesJson(attributes, currentValues());
	}

	QStringList AttributeFormWidget::missingRequiredLabels() const
	{
		std::vector<PartTypeAttribute> attributes;
		for (const Row& row : m_rows)
		{
			attributes.push_back(row.attribute);
		}

		QStringList labels;
		for (const std::string& key : missingRequiredKeys(attributes, currentValues()))
		{
			for (const Row& row : m_rows)
			{
				if (row.attribute.key == key)
				{
					labels.append(toQt(row.attribute.label)); // user data
					break;
				}
			}
		}
		return labels;
	}

}
