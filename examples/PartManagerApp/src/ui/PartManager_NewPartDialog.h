// @file PartManager_NewPartDialog.h
// @brief The manual New Part flow (`create-part-manual.svg`) — pick a type, fill the generated form.
//
// Renders the same AttributeFormWidget the part editor does, rebuilt whenever
// the chosen type changes. The type is picked out of PartTypePickerDialog rather
// than a flat combo: categories are a tree (§2b) and a list of every one of them
// in alphabetical order says nothing about which is a subtype of which.
// §11: "Create Part" stays disabled until the name and
// every `required` attribute have a value, and the reason is spelled out under
// the form rather than left to the user to guess.
//
// Nothing exists in the database until Create is pressed, so this is one of the
// §10 in-progress steps that has no autosave — Cancel simply discards. That is
// also why the three file slots are *pending* rather than attached: a part_file
// row needs a part id, so the chosen path or URL is remembered here and applied
// in one pass right after the insert. Cancel therefore copies and downloads
// nothing at all — including after "Fetch…", which reads metadata off the
// network but still only *queues* the datasheet and the photo.
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
#include <QStringList>
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

		// Where a prefill came from. The *values* land on the form the same way either way; what
		// differs is what the header above them is allowed to claim. A pasted inventory line has
		// no vendor behind it, so telling the user it was "prefilled from Mouser" — and that
		// "Mouser publishes no datasheet for this part" — would be plainly false, and the notes
		// that matter are different ones: pick a type, and nothing here was verified against a
		// catalogue.
		enum class PrefillSource
		{
			Mouser,        // §6, a row the user picked out of a search result
			ImportedList   // §5, a line out of a pasted inventory (PartMigrationDialog)
		};

		// §6: fills the same form from a Mouser row instead of leaving it blank. Nothing is
		// created here — every prefilled value is still an editable field the user confirms,
		// which is the whole point of "auto-fill what's possible, correct the rest".
		// The type is only preselected when the category mapped unambiguously; anything Mouser
		// published that we could not place is listed under the header for manual entry.
		// `source` only changes the wording of that header; it defaults to Mouser, so both
		// MainWindow flows and the CSV import are unaffected.
		void setPrefill(const MouserPartPrefill& prefill,
			PrefillSource source = PrefillSource::Mouser);

		// The two things a *pasted inventory line* knows that a vendor never does: how many are on
		// the user's own shelf, and whatever they wrote next to the number. Deliberately separate
		// from setPrefill() rather than two more fields on MouserPartPrefill — that struct is what
		// a Mouser hit maps to, and stock-on-my-shelf is not something Mouser has an opinion
		// about. Called by PartMigrationDialog after setPrefill(), so it wins where they overlap.
		//
		// `stock` goes into the stock spin box, so the §3 opening-balance transaction is written by
		// createPart() like any other; 0 leaves the box alone. `notes` is appended to the
		// description on its own line, because `Part` has no notes column and dropping the user's
		// own annotation is worse than putting it somewhere slightly wrong.
		void setListDefaults(int stock, const QString& notes);

	private slots:
		// Opens PartTypePickerDialog on the current choice and takes whatever comes back — including
		// "no category", which is a real answer here and not a cancel.
		void chooseType();
		// Rebuilds the generated form for the newly selected type.
		void onTypeChanged();
		// §6: opens the page for whatever article number is in the Mouser field right now. The
		// fetched/prefilled part opens its own ProductDetailUrl, a hand-typed one a search.
		void openOnMouser();
		// §6 in the other direction: looks the typed article number up and fills the form from it.
		// Metadata only — see the comment on the definition for why no file is written here.
		void fetchFromMouser();
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

		// The one path that changes the chosen type. Everything that used to move a combo index goes
		// through here instead: it writes m_typeId, repaints the button with the type's full path,
		// and rebuilds the generated form. There is no second place a type id can be set, so an
		// index and an id can no longer disagree about what the part is.
		void setTypeId(int typeId);
		// Whether `typeId` is a type this database actually has — a matcher may name one that was
		// deleted since, and the old combo's findData() used to answer this implicitly.
		bool hasType(int typeId) const;

		// Both Mouser buttons need a number to work on, so they follow the field live.
		void updateMouserButtons();
		// Lands a fetched prefill on the form. Empty fields are always filled; a field the user
		// already typed something else into is only replaced when `overwrite` is true. Returns the
		// labels of the fields that disagree, without touching anything, when `dryRun` is set —
		// which is how the one overwrite question knows how many fields it is asking about.
		QStringList mergePrefill(const MouserPartPrefill& prefill, bool overwrite, bool dryRun);

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
		// Borrowed, never owned — kept because the category picker opens on the same database.
		DatabaseHandle* m_handle;
		PartEditorController m_controller;
		StockController m_stock;
		AttributeFormWidget* m_attributeForm;
		// Read once: the picker builds its own list, and this copy is only here to spell the chosen
		// type's full path onto the button and to check that a matched id still exists.
		std::vector<PartType> m_types;
		// The chosen part type, NoParentType (0) for "(none — select a category)". The single source
		// of truth the §11 gate, the generated form and createPart() all read — see setTypeId().
		int m_typeId = NoParentType;
		// Kept past setPrefill() because the seller link and its price quote can only be written
		// once the part exists — at prefill time there is nothing to hang them on (§3, §6).
		MouserPartPrefill m_prefill;
		std::map<PartFileRole, PendingFile> m_pending;
		// Where each slot's label and buttons live, so updateFileSlot() can find them by role.
		std::map<PartFileRole, QLabel*> m_slotLabels;
		int m_createdPartId = 0;
	};

}
