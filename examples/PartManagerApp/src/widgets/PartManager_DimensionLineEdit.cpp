#include "widgets/PartManager_DimensionLineEdit.h"

#include "units/PartManager_ValueParser.h"

namespace PartManager
{
	namespace
	{
		// Only the background is touched, so the widget keeps the platform style otherwise.
		const char* const InvalidStyleSheet = "background-color: #FFD9D9;";
	}

	DimensionLineEdit::DimensionLineEdit(QWidget* parent)
		: QLineEdit(parent)
	{
		connect(this, &QLineEdit::textChanged, this, [this]() { reinterpret(); });
		connect(this, &QLineEdit::editingFinished, this, [this]() { commit(); });
	}

	void DimensionLineEdit::setUnit(const QString& unit)
	{
		m_unit = unit;
		// The unit is what "1k" is measured in, so the same text can mean something else now.
		reinterpret();
	}

	QString DimensionLineEdit::unit() const
	{
		return m_unit;
	}

	bool DimensionLineEdit::isEmpty() const
	{
		return text().trimmed().isEmpty();
	}

	bool DimensionLineEdit::hasValue() const
	{
		return !isEmpty() && m_valid;
	}

	double DimensionLineEdit::value() const
	{
		return hasValue() ? m_value : 0.0;
	}

	void DimensionLineEdit::setValue(double baseSiValue)
	{
		// Deliberately without the unit symbol: the field already knows its unit and the
		// hint spells it out, so the editable text stays a plain typeable number.
		setText(QString::fromStdString(ValueParser::format(baseSiValue)));
	}

	void DimensionLineEdit::clearValue()
	{
		clear();
	}

	QString DimensionLineEdit::hintText() const
	{
		if (isEmpty())
		{
			return QString();
		}
		if (!m_valid)
		{
			return m_unit.isEmpty()
				? tr("not a number")
				: tr("not a valid %1 value").arg(m_unit); // the unit is fixed vocabulary, not translated
		}
		return tr("= %1").arg(QString::fromStdString(ValueParser::format(m_value, m_unit.toStdString())));
	}

	void DimensionLineEdit::reinterpret()
	{
		ValueParseResult result = ValueParser::parse(text().toStdString(), m_unit.toStdString());
		m_valid = isEmpty() || result.ok;
		m_value = result.ok ? result.value : 0.0;

		setStyleSheet(m_valid ? QString() : QString::fromLatin1(InvalidStyleSheet));
		emit hintChanged(hintText());
	}

	void DimensionLineEdit::commit()
	{
		// §2a: "," is an alternate decimal separator, folded to the canonical "." — here, on
		// commit, and never per keystroke. Whatever else the user typed ("4k7") stays as typed
		// until the record is reloaded, when it comes back as the formatted base-SI value.
		const QString normalized = text().replace(',', '.');
		if (normalized != text())
		{
			setText(normalized);
		}
		reinterpret();
		emit committed();
	}

}
