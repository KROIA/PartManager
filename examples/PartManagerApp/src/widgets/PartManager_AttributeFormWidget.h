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
// @see docs/design/ARCHITECTURE.md §2a, §10, §11, §12b
// @see PartManager_PartEditorController.h, PartManager_DimensionLineEdit.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
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

		// Builds the editor for one attribute and wires its two change signals.
		QWidget* createEditor(const PartTypeAttribute& attribute, QLabel* hint);
		// Reads every row back out.
		std::map<std::string, AttributeValue> currentValues() const;

		QFormLayout* m_layout;
		std::vector<Row> m_rows;
		// setValuesJson() writes into the editors, which would otherwise look like user edits.
		bool m_loading = false;
	};

}
