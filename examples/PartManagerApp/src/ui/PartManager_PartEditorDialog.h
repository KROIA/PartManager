// @file PartManager_PartEditorDialog.h
// @brief The part editor (`part-editor.svg`) — identity fields, tag chips, datasheet, generated form.
//
// Opened by double-clicking a row in the main window's part table. There is
// deliberately **no Save button**: §10 autosave writes the already-existing
// record whenever a field finishes editing, and closing is always silent.
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

	class PartEditorDialog : public QDialog
	{
		Q_OBJECT
	public:
		PartEditorDialog(DatabaseHandle* handle, int partId, QWidget* parent = nullptr);
		~PartEditorDialog() override;

		// §6: the DataSheetUrl a Mouser prefill carried, used to pre-fill the Download prompt.
		// Mouser answers with an empty one for most real parts, which is why attaching a file by
		// hand is the main road and this only saves typing when the URL happens to be there.
		void setDatasheetSourceUrl(const QString& url);

		// True when the user deleted the part from in here, so the caller knows the row it was
		// opened from is gone rather than merely edited.
		bool partWasDeleted() const { return m_deleted; }

	protected:
		// Every way out of a QDialog (Close, Esc, the window's X) funnels through here,
		// so it is the one place a still-pending debounced write has to be flushed.
		void done(int result) override;

	private slots:
		// §10: writes every field back to the existing record.
		void autosave();
		// Rebuilds the §2d chip row from the part's current tags.
		void reloadTags();

		// §3: the quantity field is a correction, not a write — it logs the difference as
		// `manual_adjust` through StockRepository, so `part.stock_qty` can never drift from the
		// log. A no-op when the number did not actually change.
		void commitStockQuantity();
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

		// Deletes the part and closes. Confirmed first, and the confirmation names what goes with
		// it — the stock history in particular is not recoverable from anywhere else.
		void deletePart();

		// §6: the Mouser article number, which is what the Cart API orders by — `part.mpn` is the
		// *manufacturer's* number and Mouser rejects it. Filled in automatically for a part
		// created from a Mouser search; editable here because a part imported from CSV, or one
		// that turns out to duplicate an existing row, has none and cannot otherwise be ordered.
		void commitMouserPartNumber();
		// Opens the part's mouser.com page. Falls back to a search for the article number when
		// no product URL was stored (a hand-typed number has none).
		void openOnMouser();

	private:
		// Fills the identity fields and the generated form from the loaded part.
		void loadPart();
		// Restarts the §10 debounce — a burst of keystrokes becomes one write.
		void scheduleSave();
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
		QString m_datasheetSourceUrl;
	};

}
