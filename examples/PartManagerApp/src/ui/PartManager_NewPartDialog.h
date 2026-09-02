// @file PartManager_NewPartDialog.h
// @brief The manual New Part flow (`create-part-manual.svg`) — pick a type, fill the generated form.
//
// Renders the same AttributeFormWidget the part editor does, rebuilt whenever
// the type combo changes. §11: "Create Part" stays disabled until the name and
// every `required` attribute have a value, and the reason is spelled out under
// the form rather than left to the user to guess.
//
// Nothing exists in the database until Create is pressed, so this is one of the
// §10 in-progress steps that has no autosave — Cancel simply discards. That is
// also why the three file slots are *pending* rather than attached: a part_file
// row needs a part id, so the chosen path or URL is remembered here and applied
// in one pass right after the insert. Cancel therefore copies and downloads
// nothing at all.
//
// Everything a part can carry is settable here, so nothing forces a trip through
// the editor straight after creating: identity, the Mouser article number, stock,
// the reorder threshold, the type's attributes, and all three file slots.
// @see docs/design/ARCHITECTURE.md §2d, §3, §6, §10, §11, §12b
// @see PartManager_PartEditorController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include "controllers/PartManager_StockController.h"
#include "domain/PartManager_PartFileRole.h"
#include "mouser/PartManager_MouserSearchService.h"
#include <QDialog>
#include <map>

class QLabel;
class QPushButton;

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
		// One file slot's chosen source. Exactly one of the two is set; both empty means the
		// slot stays empty. Nothing touches the disk or the network until Create.
		struct PendingFile
		{
			QString localPath;
			QString url;
		};

		// Wires one slot's File…/URL…/Clear buttons and its state label. `urlButton` may be
		// null for a slot with no download path (the 3D model — no vendor API publishes one).
		void wireFileSlot(PartFileRole role, QLabel* state, QPushButton* fileButton,
			QPushButton* urlButton, QPushButton* clearButton, const QString& filter);
		// Repaints one slot's state label from m_pending.
		void updateFileSlot(PartFileRole role);
		// Applies every pending slot to the freshly created part. Failures are collected and
		// reported once at the end — a datasheet URL Mouser published but no longer serves must
		// not make it look like the part was not created.
		void applyPendingFiles(int partId, Part& part);
		// §5c: right after Create, offer the KiCad symbol and footprint for the part just made.
		// Not part of the pending-file pass above — those slots take a path or a URL, and this one
		// produces converted bytes that only exist once the lookup has run.
		void fetchEcadModel(const Part& part);

		Ui::NewPartDialog* m_ui;
		PartEditorController m_controller;
		StockController m_stock;
		AttributeFormWidget* m_attributeForm;
		// Kept past setPrefill() because the seller link and its price quote can only be written
		// once the part exists — at prefill time there is nothing to hang them on (§3, §6).
		MouserPartPrefill m_prefill;
		std::map<PartFileRole, PendingFile> m_pending;
		// Where each slot's label and buttons live, so updateFileSlot() can find them by role.
		std::map<PartFileRole, QLabel*> m_slotLabels;
		int m_createdPartId = 0;
	};

}
