#include "widgets/PartManager_AttributeFormWidget.h"

#include "widgets/PartManager_DimensionLineEdit.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleValidator>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
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

		// How one undeclared value is shown. There is no declaration behind it, so there is no
		// datatype, no unit and no ValueParser formatting to apply (§2a) — what can honestly be
		// shown is what the JSON says. A dimension object is spelled out as "4700 Ω" because
		// printing the raw braces would read as corruption rather than as a value.
		QString describeJsonValue(const QJsonValue& value)
		{
			if (value.isObject())
			{
				const QJsonObject object = value.toObject();
				if (object.contains(QStringLiteral("value")))
				{
					const QString number = QString::number(
						object.value(QStringLiteral("value")).toDouble(), 'g', 10);
					const QString unit = object.value(QStringLiteral("unit")).toString();
					return unit.isEmpty() ? number : (number + QStringLiteral(" ") + unit);
				}
				return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
			}
			if (value.isBool())
			{
				// A bare true/false would read as a literal, not as an answer.
				return value.toBool() ? QObject::tr("yes") : QObject::tr("no");
			}
			if (value.isDouble())
			{
				return QString::number(value.toDouble(), 'g', 10);
			}
			return value.toString();
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
		// Counted rather than looped until takeAt() returns null: QFormLayout warns on the
		// out-of-range call instead of quietly answering nullptr the way QLayout does.
		while (m_layout->count() > 0)
		{
			QLayoutItem* item = m_layout->takeAt(0);
			delete item->widget();
			delete item;
		}
		// The footnote went with them, and what it reported was measured against the old
		// declarations — the next setValuesJson() decides afresh what is undeclared now.
		m_unrecognisedSection = nullptr;
		m_unrecognised.clear();

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
		// readAttributesJson() drops what the list does not declare, so the raw JSON has to be
		// looked at again to see what was dropped.
		collectUnrecognised(attributesJson);

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

		rebuildUnrecognisedSection();
	}

	void AttributeFormWidget::setUnrecognisedValuesVisible(bool visible)
	{
		if (m_showUnrecognised == visible)
		{
			return;
		}
		m_showUnrecognised = visible;
		if (!visible)
		{
			m_unrecognised.clear();
		}
		rebuildUnrecognisedSection();
	}

	QStringList AttributeFormWidget::unrecognisedKeys() const
	{
		QStringList keys;
		for (const UnrecognisedValue& entry : m_unrecognised)
		{
			keys.append(toQt(entry.key));   // an attribute key is data, never translated
		}
		return keys;
	}

	void AttributeFormWidget::collectUnrecognised(const QString& attributesJson)
	{
		m_unrecognised.clear();
		if (!m_showUnrecognised)
		{
			return;
		}

		const QJsonDocument document = QJsonDocument::fromJson(attributesJson.toUtf8());
		if (!document.isObject())
		{
			// An empty or unparsable column is not an exception worth reporting — there is nothing
			// in it to lose.
			return;
		}

		const QJsonObject object = document.object();
		for (auto it = object.begin(); it != object.end(); ++it)
		{
			const std::string key = it.key().toStdString();
			const bool declared = std::any_of(m_rows.begin(), m_rows.end(),
				[&key](const Row& row) { return row.attribute.key == key; });
			if (!declared)
			{
				m_unrecognised.push_back(UnrecognisedValue{ key, it.value() });
			}
		}
	}

	void AttributeFormWidget::dropUnrecognised(const std::string& key)
	{
		m_unrecognised.erase(std::remove_if(m_unrecognised.begin(), m_unrecognised.end(),
			[&key](const UnrecognisedValue& entry) { return entry.key == key; }),
			m_unrecognised.end());
		rebuildUnrecognisedSection();
		// A real edit to the part's JSON, so it travels the same two signals every row does —
		// otherwise §10's autosave never hears about it and the value is back on the next open.
		emit valueChanged();
		emit valueCommitted();
	}

	void AttributeFormWidget::rebuildUnrecognisedSection()
	{
		if (m_unrecognisedSection)
		{
			// takeRow() rather than removeRow(): removeRow() deletes the widget on the spot, and
			// this runs from a delete button *inside* that widget, mid-signal. Taking the row
			// leaves no empty slot behind either — the point of removing instead of hiding.
			QFormLayout::TakeRowResult taken = m_layout->takeRow(m_unrecognisedSection);
			delete taken.labelItem;
			delete taken.fieldItem;
			m_unrecognisedSection->hide();
			m_unrecognisedSection->setParent(nullptr);
			m_unrecognisedSection->deleteLater();
			m_unrecognisedSection = nullptr;
		}
		if (!m_showUnrecognised || m_unrecognised.empty())
		{
			return;
		}

		// Spans both columns: this is a footnote under the form, not another labelled field.
		m_unrecognisedSection = new QWidget(this);
		m_unrecognisedSection->setObjectName(QStringLiteral("unrecognisedSection"));
		QVBoxLayout* sectionLayout = new QVBoxLayout(m_unrecognisedSection);
		sectionLayout->setContentsMargins(0, 8, 0, 0);

		QFrame* separator = new QFrame(m_unrecognisedSection);
		separator->setFrameShape(QFrame::HLine);
		separator->setFrameShadow(QFrame::Sunken);
		sectionLayout->addWidget(separator);

		QLabel* heading = new QLabel(tr("Unrecognised values"), m_unrecognisedSection);
		heading->setObjectName(QStringLiteral("unrecognisedHeading"));
		sectionLayout->addWidget(heading);

		QLabel* explanation = new QLabel(
			tr("Values saved under this part before its category changed. They are kept so nothing "
				"is lost; this category has no field for them. Delete one to remove it for good."),
			m_unrecognisedSection);
		explanation->setObjectName(QStringLiteral("unrecognisedExplanation"));
		explanation->setWordWrap(true);
		explanation->setEnabled(false);   // subordinate to the real rows, like a row's hint
		sectionLayout->addWidget(explanation);

		for (const UnrecognisedValue& entry : m_unrecognised)
		{
			QWidget* row = new QWidget(m_unrecognisedSection);
			QHBoxLayout* rowLayout = new QHBoxLayout(row);
			rowLayout->setContentsMargins(0, 0, 0, 0);

			// Key and value are both part data — shown verbatim, never translated.
			QLabel* text = new QLabel(QStringLiteral("%1: %2")
				.arg(toQt(entry.key), describeJsonValue(entry.value)), row);
			text->setWordWrap(true);
			text->setEnabled(false);
			rowLayout->addWidget(text, 1);

			QToolButton* remove = new QToolButton(row);
			remove->setObjectName(QStringLiteral("unrecognisedRemoveButton"));
			remove->setText(tr("Delete"));
			remove->setToolTip(tr("Remove this value from the part"));
			// The key travels on the button rather than in the lambda's name, so a test (and the
			// next reader) can tell the buttons apart without counting rows.
			remove->setProperty("attributeKey", toQt(entry.key));
			const std::string key = entry.key;
			connect(remove, &QToolButton::clicked, this, [this, key]() { dropUnrecognised(key); });
			rowLayout->addWidget(remove);

			sectionLayout->addWidget(row);
		}

		m_layout->addRow(m_unrecognisedSection);
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
		const QString json = writeAttributesJson(attributes, currentValues());
		if (m_unrecognised.empty())
		{
			// The untouched path, byte for byte: with the flag off m_unrecognised is always empty,
			// so MovePartDialog still gets exactly what the target type declares and nothing else.
			return json;
		}

		// Re-attached verbatim. writeAttributesJson() owns the declared keys and wins on a
		// collision, though there can be none — a key is unrecognised precisely because no row
		// declares it.
		QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
		for (const UnrecognisedValue& entry : m_unrecognised)
		{
			const QString key = toQt(entry.key);
			if (!object.contains(key))
			{
				object.insert(key, entry.value);
			}
		}
		return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
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
