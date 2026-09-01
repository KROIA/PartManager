// @file PartManager_DimensionLineEdit.h
// @brief The §2a SI-prefix-aware numeric field — "4k7", "4.7k", "4700" and "4700Ω" all mean 4700.
//
// A plain QLineEdit that knows its attribute's declared dropdown unit and runs
// every entry through ValueParser. Two things happen at different moments, on
// purpose (§2a): the text is *interpreted* on every keystroke, so the hint and
// the invalid flag are always live, but it is only ever *rewritten* (`,` folded
// to `.`) on commit — focus loss or Return — so the cursor cannot jump while
// typing. An unparsable entry is flagged red and reports hasValue() == false;
// it is never silently read as 0.
//
// Used by the generated attribute form on both the part editor and New Part,
// and available to the §7a search boxes later.
// @see docs/design/ARCHITECTURE.md §2a, §12b
// @see PartManager_ValueParser.h, PartManager_AttributeFormWidget.h
#pragma once

#include <QLineEdit>
#include <QString>

namespace PartManager
{

	class DimensionLineEdit : public QLineEdit
	{
		Q_OBJECT
	public:
		explicit DimensionLineEdit(QWidget* parent = nullptr);

		// The attribute's §2a dropdown unit; empty means "(no unit)".
		void setUnit(const QString& unit);
		QString unit() const;

		// True while the field is blank — neither valid nor invalid, just unfilled.
		bool isEmpty() const;
		// True when the field holds a parsable value. A blank field is not a value.
		bool hasValue() const;
		// The parsed base-SI number, 0 when the field is blank or unparsable.
		double value() const;

		// Shows a base-SI value in the shortest prefixed form, e.g. 4700 -> "4.7 kΩ".
		void setValue(double baseSiValue);
		// Blanks the field without emitting committed().
		void clearValue();

		// The live interpretation shown next to the field: "= 4.7 kΩ", the invalid
		// notice, or empty while the field is blank.
		QString hintText() const;

	signals:
		// The user finished editing and the text has been normalized (§10 autosave trigger).
		void committed();
		// hintText() changed — emitted on every keystroke.
		void hintChanged(const QString& hint);

	private:
		// Re-parses the current text and repaints the invalid flag. Never rewrites the text.
		void reinterpret();
		// Focus-loss/Return: folds "," to "." (§2a), then re-parses.
		void commit();

		QString m_unit;
		double m_value = 0.0;
		bool m_valid = true;
	};

}
