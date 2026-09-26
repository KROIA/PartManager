// @file PartManager_AttributeFormWidget.h
// @brief The form generated at runtime from a type's effective attributes (§2, §2b, §11).
//
// One row per PartTypeRepository::effectiveAttributes() entry, the widget picked
// by the attribute's datatype (widgetKindFor()). This is the case CODING_STYLE.md
// names as the legitimate exception to the .ui-first rule — the rows only exist
// once a part_type has been chosen, so Designer has nothing to lay out. The
// frame around it (group box, scroll area, buttons) still lives in the hosting
// screen's .ui.
//
// Required attributes are marked with a "*" and reported by
// missingRequiredLabels() so New Part can block Create (§11); an attribute's
// `tooltip` is hung on both its label and its editor.
//
// Two signals rather than one, because the editor and New Part need different
// moments: valueChanged() fires per keystroke (New Part re-checks whether Create
// can be enabled, which has to be true before the button is clicked), while
// valueCommitted() fires on focus-loss/Return only, which is what §10's autosave
// writes on.
//
// **Unrecognised values** (opt-in, see setUnrecognisedValuesVisible()). A part's
// values live in `part.attributes` keyed by attribute key, what its type declares
// lives in `part_type_attribute` resolved through §2b inheritance — and the two
// can legitimately diverge, most often after a part changes category. Left alone,
// a value under a key nothing declares gets no row, so it is in the database and
// invisible: not shown, not editable, not deletable. With the flag on, those keys
// are listed in a subordinate section below the real rows, each with a delete
// button, and are carried back through valuesJson() untouched. With it off —
// the default — the widget behaves exactly as it always has, which MovePartDialog
// depends on: dropping the keys the target category does not declare *is* that
// move's documented effect, and it happens precisely because valuesJson() emits
// only what its declared list contains.
// @see docs/design/ARCHITECTURE.md §2, §2a, §2b, §10, §11, §12b
// @see PartManager_PartEditorController.h, PartManager_DimensionLineEdit.h, PartManager_MovePartDialog.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QJsonValue>
#include <QStringList>
#include <QWidget>
#include <map>
#include <string>
#include <vector>

class QFormLayout;
class QLabel;

namespace PartManager
{

	class AttributeFormWidget : public QWidget
	{
		Q_OBJECT
	public:
		explicit AttributeFormWidget(QWidget* parent = nullptr);

		// Rebuilds every row for one type's effective attributes, discarding current values.
		void setAttributes(const std::vector<PartTypeAttribute>& attributes);

		// Fills the rows from a part's raw `attributes` JSON. Emits nothing — loading is
		// not an edit, and an autosave triggered by it would immediately write back.
		void setValuesJson(const QString& attributesJson);
		// The rows as `attributes` JSON, in the §2a shape (see PartEditorController.h).
		QString valuesJson() const;

		// §11: labels of the required attributes still empty. Empty list => nothing blocks Create.
		QStringList missingRequiredLabels() const;

		// Opt-in: show the keys the part carries that its type does not declare, and round-trip
		// them through valuesJson(). Off by default *on purpose* — a caller that rewrites a part's
		// JSON against a different type's attribute list (MovePartDialog) means those keys to go,
		// and turning this on globally would silently resurrect them. Set it before setValuesJson();
		// the section is (re)built from whatever the next load finds.
		void setUnrecognisedValuesVisible(bool visible);
		bool unrecognisedValuesVisible() const { return m_showUnrecognised; }
		// The undeclared keys currently held, in the order the JSON listed them. Empty whenever the
		// flag is off, and the section is not built at all while it is empty — a part in a
		// consistent state has to look exactly as it did before this existed.
		QStringList unrecognisedKeys() const;

	signals:
		// A field's content changed at all — for live enable/disable checks.
		void valueChanged();
		// A field finished editing — the §10 autosave trigger.
		void valueCommitted();

	private:
		// One generated row: its declaration, its editor and the §2a interpretation hint.
		struct Row
		{
			PartTypeAttribute attribute;
			QWidget* editor = nullptr;
			QLabel* hint = nullptr;
		};

		// One value the loaded JSON carried under a key no row declares. The JSON value is kept
		// verbatim rather than parsed into an AttributeValue: with no declaration there is no
		// datatype to parse it against, and writing back exactly what was read is the only way to
		// promise nothing is lost.
		struct UnrecognisedValue
		{
			std::string key;
			QJsonValue value;
		};

		// Builds the editor for one attribute and wires its two change signals.
		QWidget* createEditor(const PartTypeAttribute& attribute, QLabel* hint);
		// Reads every row back out.
		std::map<std::string, AttributeValue> currentValues() const;

		// Collects the loaded JSON's keys that no row declares. No-op (and clears) while the flag
		// is off, so nothing is ever carried that the caller did not ask for.
		void collectUnrecognised(const QString& attributesJson);
		// Creates, refreshes or removes the footnote section. Removes it outright when there is
		// nothing to report — hiding an empty section would still cost a layout row.
		void rebuildUnrecognisedSection();
		// The user's explicit delete of one undeclared value. Nothing removes these on its own.
		void dropUnrecognised(const std::string& key);

		QFormLayout* m_layout;
		std::vector<Row> m_rows;
		// setValuesJson() writes into the editors, which would otherwise look like user edits.
		bool m_loading = false;

		bool m_showUnrecognised = false;
		std::vector<UnrecognisedValue> m_unrecognised;
		// The whole footnote, section heading included — null whenever there is nothing to report.
		QWidget* m_unrecognisedSection = nullptr;
	};

}
