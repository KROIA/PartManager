// @file PartManager_NewPartDialog.h
// @brief The manual New Part flow (`create-part-manual.svg`) — pick a type, fill the generated form.
//
// Renders the same AttributeFormWidget the part editor does, rebuilt whenever
// the type combo changes. §11: "Create Part" stays disabled until the name and
// every `required` attribute have a value, and the reason is spelled out under
// the form rather than left to the user to guess.
//
// Nothing exists in the database until Create is pressed, so this is one of the
// §10 in-progress steps that has no autosave — Cancel simply discards.
// @see docs/design/ARCHITECTURE.md §2d, §10, §11, §12b
// @see PartManager_PartEditorController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include "mouser/PartManager_MouserSearchService.h"
#include <QDialog>

namespace Ui { class NewPartDialog; }

namespace PartManager
{

	class AttributeFormWidget;

	class NewPartDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit NewPartDialog(DatabaseHandle* handle, QWidget* parent = nullptr);
		~NewPartDialog() override;

		// The part Create wrote, 0 while the dialog was cancelled or the insert failed.
		int createdPartId() const;

		// §6: fills the same form from a Mouser row instead of leaving it blank. Nothing is
		// created here — every prefilled value is still an editable field the user confirms,
		// which is the whole point of "auto-fill what's possible, correct the rest".
		// The type is only preselected when the category mapped unambiguously; anything Mouser
		// published that we could not place is listed under the header for manual entry.
		void setPrefill(const MouserPartPrefill& prefill);

	private slots:
		// Rebuilds the generated form for the newly selected type.
		void onTypeChanged();
		// §11: enables/disables Create and explains what is still missing.
		void revalidate();
		// Writes the new part; its default tags are seeded inside insertPart() (§2d).
		void createPart();

	private:
		Ui::NewPartDialog* m_ui;
		PartEditorController m_controller;
		AttributeFormWidget* m_attributeForm;
		// Kept past setPrefill() because the seller link and its price quote can only be written
		// once the part exists — at prefill time there is nothing to hang them on (§3, §6).
		MouserPartPrefill m_prefill;
		int m_createdPartId = 0;
	};

}
