// @file PartManager_PartEditorDialog.h
// @brief The part editor (`part-editor.svg`) — identity fields, tag chips, datasheet, generated form.
//
// Opened by double-clicking a row in the main window's part table. There is
// deliberately **no Save button**: §10 autosave writes the already-existing
// record whenever a field finishes editing, and closing is always silent.
//
// It is the one **modeless** dialog in the app. An application-modal editor
// blocks the window the §14a chat dock lives in, so the assistant could neither
// be read nor asked anything while a part was open. Everything that follows from
// that is in open(): a registry so one part cannot be edited by two windows at
// once, WA_DeleteOnClose, and finished() instead of a return from exec().
//
// Layout lives in PartManager_PartEditorDialog.ui; only the two genuinely
// data-driven parts are built in code — the attribute rows (AttributeFormWidget)
// and the §2d tag chips, which are one button per tag the part actually carries.
// @see docs/design/ARCHITECTURE.md §2a, §2d, §3, §6, §10, §12b
// @see PartManager_PartEditorController.h, PartManager_AttributeFormWidget.h
#pragma once

#include "controllers/PartManager_PartEditorController.h"
#include "controllers/PartManager_StockController.h"
#include <QDialog>

class QLabel;
class QPushButton;
class QTimer;

namespace Ui { class PartEditorDialog; }

namespace PartManager
{

	class AttributeFormWidget;
	class KeywordCheckList;
	class KicadPreviewWidget;
	// §14d. Forward-declared unconditionally even though the class only exists when QtLLM is
	// available: a pointer to an incomplete type costs nothing, and gating the declaration
	// would make this header say one thing to moc and another to the compiler.
	class LlmController;

	class PartEditorDialog : public QDialog
	{
		Q_OBJECT
	public:
		PartEditorDialog(DatabaseHandle* handle, int partId, QWidget* parent = nullptr);
		~PartEditorDialog() override;

		// The one way in: every call site opens the editor through here, nobody constructs one.
		// Shows it modeless, owned by Qt (WA_DeleteOnClose) — the caller connects to
		// QDialog::finished() for whatever it used to do after exec() and then forgets the
		// pointer. Nothing is ever read back off the dialog; §10 means closing *is* the commit.
		//
		// **One editor per part.** Two windows autosaving the same row would race, and §10 has
		// no merge — so a part that is already open is raised and that same editor returned
		// rather than a second one built. `raisedExisting`, when given, says that is what
		// happened: the earlier caller's finished() handler is still attached and doing the
		// refresh, so a second one must not be connected on top of it.
		static PartEditorDialog* open(DatabaseHandle* handle, int partId, QWidget* parent,
			bool* raisedExisting = nullptr);

		// Deletes every open editor, flushing each (see ~PartEditorDialog) while the database it
		// was opened against is still there. The window that owns the DatabaseHandle has to call
		// this before it goes: a dialog parented to it is a QObject child, and Qt deletes
		// children from ~QWidget — which runs *after* the members holding the handle are already
		// gone. Leaving it to Qt would flush the last keystroke into freed memory.
		static void closeAll();

		// Reloads the editor open on `partId` from the database, false when that part is not open.
		//
		// **The editor is modeless, so anything else that writes a part row is writing behind it.**
		// The editor holds its own copy of the part and §10 autosaves the whole record, so the next
		// keystroke in an open editor would put the *old* value back — the assistant's
		// `set_part_datasheet` is the first caller that can hit this, but nothing about it is
		// specific to that tool. Call this after writing a part row that may be open.
		static bool reloadIfOpen(int partId);

		// §6: the DataSheetUrl a Mouser prefill carried, used to pre-fill the Download prompt.
		// Mouser answers with an empty one for most real parts, which is why attaching a file by
		// hand is the main road and this only saves typing when the URL happens to be there.
		void setDatasheetSourceUrl(const QString& url);

		// §14d: the assistant the "Generate description" button injects its prompt into. Null —
		// the default, and what every caller outside the main window passes — hides the button
		// outright rather than greying it out, because a disabled button invites a hunt for the
		// setting that would enable it and there is none.
		void setLlmController(LlmController* controller);

		// True when the user deleted the part from in here, so the caller knows the row it was
		// opened from is gone rather than merely edited.
		bool partWasDeleted() const { return m_deleted; }

		// The two fields that do **not** write themselves back when something other than a human
		// types in them. Every other widget in here is wired to textChanged/valueChanged, which
		// a programmatic setText()/setValue() emits — these two are on editingFinished, which it
		// does not. So anything filling the editor from code (a tool, a test, a prefill) has to
		// call the matching commit itself, which is why both are reachable from outside rather
		// than private. Both are no-ops when the value did not actually change.
	public slots:
		// §3: the quantity field is a correction, not a write — it logs the difference as
		// `manual_adjust` through StockRepository, so `part.stock_qty` can never drift from the
		// log. A no-op when the number did not actually change.
		void commitStockQuantity();
		// §6: the Mouser article number, which is what the Cart API orders by — `part.mpn` is the
		// *manufacturer's* number and Mouser rejects it. Filled in automatically for a part
		// created from a Mouser search; editable here because a part imported from CSV, or one
		// that turns out to duplicate an existing row, has none and cannot otherwise be ordered.
		void commitMouserPartNumber();

	protected:
		// Every way out of a QDialog (Close, Esc, the window's X) funnels through here,
		// so it is the one place a still-pending debounced write has to be flushed.
		void done(int result) override;

	private slots:
		// §10: writes every field back to the existing record.
		void autosave();
		// Rebuilds the §2d chip row from the part's current tags.
		void reloadTags();

		// Fills the history table from the part's transactions, oldest first.
		void reloadHistory();

		// §3 datasheet slot. Each of these ends in the same autosave() that every other field uses.
		void attachDatasheet();
		void downloadDatasheet();
		void removeDatasheet();
		void openDatasheet();

		// The product photo (`part_file(role='image')`) — what the parts table paints as a
		// thumbnail. Same three ways in as the datasheet; no column on `part` points at it, so
		// none of these needs an autosave afterwards.
		void attachImage();
		void downloadImage();
		void removeImage();
		// §6: the product photo for the part's *Mouser article number*, looked up through the
		// Search API. The URL comes from `MouserPart.ImagePath` and never from the product page —
		// www.mouser.* answers a scraper with a DataDome challenge, and only the image CDN is
		// ungated (measured 2026-09-02, MouserSearchService::datasheetUrlFor()'s note).
		void fetchImageFromMouser();

		// §5c KiCad slots. The symbol and footprint the generated library is built from, and that
		// a KiCad edit is synced back into.
		void importEcadArchive();
		// §5c: fetch the symbol and footprint instead of hunting for them. Tries EasyEDA/LCSC
		// first — the one ECAD source with an open API — and falls back to watching for a vendor
		// ZIP the user downloads by hand. See EcadFetchDialog for why there is no third option.
		void fetchEcadModel();
		void attachKicadSymbol();
		void removeKicadSymbol();
		void attachKicadFootprint();
		void removeKicadFootprint();
		// §5a on demand: the same sharing offer the import paths make, for a part that already
		// has a footprint and therefore never reaches one of those moments again. Enabled only
		// when there is a footprint, because the metric is entirely comparative — with nothing
		// attached there is no geometry to rank the candidates against. Says out loud when the
		// answer is nothing: the offer was asked for by a button press, so silence would read
		// as a button that does not work.
		void shareKicadFootprint();

		// Deletes the part and closes. Confirmed first, and the confirmation names what goes with
		// it — the stock history in particular is not recoverable from anywhere else.
		void deletePart();

		// §14d: builds the prompt from the fields as they stand and injects it into the chat
		// panel, where it renders as a user bubble and costs one visible turn. It does not call
		// the model behind the user's back, and it does not queue: a refusal (a turn already in
		// flight) is reported on the status line instead. Closes the editor on success — see the
		// comment at the implementation for why.
		void generateDescription();

		// Opens the part's mouser.com page. Falls back to a search for the article number when
		// no product URL was stored (a hand-typed number has none).
		void openOnMouser();

	private:
		// Fills the identity fields and the generated form from the loaded part.
		void loadPart();
		// Restarts the §10 debounce — a burst of keystrokes becomes one write.
		void scheduleSave();
		// Everything a field might still be holding on to: the debounced write, and the two
		// fields that only commit on editingFinished. Called from done() and again from the
		// destructor — see both for why the second one is not redundant.
		void flushPendingEdits();
		// §11: re-renders the category's naming pattern against what the fields hold right now,
		// and disables the button when the name it produces is the one the part already has.
		// Reads the widgets rather than m_part, so it is current before the autosave has run.
		void updateSuggestedName();
		// Fills the "+ Tag" menu with the tags this part does not carry yet.
		void refreshAddTagMenu();
		// Puts the datasheet row into one of its three states: none, attached, or attached but
		// missing from the file store. Which buttons are usable follows from that, so "nothing
		// attached yet" reads as a disabled Open/Remove rather than a button that does nothing.
		void updateDatasheetState();
		// Same three states for the image slot, plus the thumbnail itself.
		void updateImageState();
		// The §5c KiCad rows: what is attached, or that the symbol is being generated instead.
		void updateKicadState();
		// Repaints the symbol and footprint previews from whatever is attached now. Called from
		// updateKicadState(), so every attach, import and remove refreshes them without the
		// individual slots having to remember to.
		void updateKicadPreviews();
		// Fills the Mouser row and enables Open only when there is something to open.
		void updateMouserState();
		// Enables "From Mouser" only when there is an article number *and* a key to look it up
		// with, and says in the tooltip which of the two is missing. Driven from updateMouserState()
		// rather than from updateImageState(): what it depends on is the article number, and an
		// image already attached is no reason to refuse a better one.
		void updateMouserImageButton();
		// The shared half of the button and the automatic fetch: looks `number` up through the
		// Search API and downloads the photo into the part's image slot. False with `outError` set
		// on every failure — no key, no result, no photo published, download refused — because the
		// two callers report it differently: the button with a dialog, the automatic path with one
		// status line, since nobody asked it to run.
		bool downloadMouserImage(const std::string& number, QString* outError);

		Ui::PartEditorDialog* m_ui;
		PartEditorController m_controller;
		// Same connection as m_controller, but every quantity change goes through §3's log.
		StockController m_stock;
		AttributeFormWidget* m_attributeForm;
		// Built in code rather than in the .ui, which would need them promoted there first.
		KicadPreviewWidget* m_symbolPreview = nullptr;
		KicadPreviewWidget* m_footprintPreview = nullptr;
		// §7a: the search words this part gets from its category, one tick box each, so a single
		// part can drop one it does not answer to.
		KeywordCheckList* m_inheritedKeywords = nullptr;
		// §11: the name the category's pattern builds out of this part, and the button that takes
		// it. The label shows it even when it equals the current name, so the pattern is always
		// visible; the button is what goes quiet.
		QLabel* m_suggestedNameLabel = nullptr;
		QPushButton* m_applyNameButton = nullptr;
		QString m_suggestedName;
		QTimer* m_saveTimer;
		Part m_part;
		// Blocks autosave while loadPart() writes into the widgets.
		bool m_loading = true;
		bool m_deleted = false;
		// Set by the destructor before its flush. A failed write normally earns a message box,
		// and a modal box parented to a widget that is half destroyed is not something to do on
		// the way out of the process — during destruction the failure goes nowhere instead.
		bool m_destroying = false;
		QString m_datasheetSourceUrl;
		// §14d. Not owned — it belongs to the main window, which outlives this dialog.
		LlmController* m_llm = nullptr;
	};

}
