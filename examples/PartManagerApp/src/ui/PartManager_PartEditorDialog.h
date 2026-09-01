// @file PartManager_PartEditorDialog.h
// @brief The part editor (`part-editor.svg`) — identity fields, tag chips and the generated form.
//
// Opened by double-clicking a row in the main window's part table. There is
// deliberately **no Save button**: §10 autosave writes the already-existing
// record whenever a field finishes editing, and closing is always silent.
//
// Layout lives in PartManager_PartEditorDialog.ui; only the two genuinely
// data-driven parts are built in code — the attribute rows (AttributeFormWidget)
// and the §2d tag chips, which are one button per tag the part actually carries.
// @see docs/design/ARCHITECTURE.md §2a, §2d, §10, §12b
// @see PartManager_PartEditorController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include <QDialog>

class QTimer;

namespace Ui { class PartEditorDialog; }

namespace PartManager
{

	class AttributeFormWidget;

	class PartEditorDialog : public QDialog
	{
		Q_OBJECT
	public:
		PartEditorDialog(DatabaseHandle* handle, int partId, QWidget* parent = nullptr);
		~PartEditorDialog() override;

	protected:
		// Every way out of a QDialog (Close, Esc, the window's X) funnels through here,
		// so it is the one place a still-pending debounced write has to be flushed.
		void done(int result) override;

	private slots:
		// §10: writes every field back to the existing record.
		void autosave();
		// Rebuilds the §2d chip row from the part's current tags.
		void reloadTags();

	private:
		// Fills the identity fields and the generated form from the loaded part.
		void loadPart();
		// Restarts the §10 debounce — a burst of keystrokes becomes one write.
		void scheduleSave();
		// Fills the "+ Tag" menu with the tags this part does not carry yet.
		void refreshAddTagMenu();

		Ui::PartEditorDialog* m_ui;
		PartEditorController m_controller;
		AttributeFormWidget* m_attributeForm;
		QTimer* m_saveTimer;
		Part m_part;
		// Blocks autosave while loadPart() writes into the widgets.
		bool m_loading = true;
	};

}
