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

		// §6: the DataSheetUrl a Mouser prefill carried, used to pre-fill the Download prompt.
		// Mouser answers with an empty one for most real parts, which is why attaching a file by
		// hand is the main road and this only saves typing when the URL happens to be there.
		void setDatasheetSourceUrl(const QString& url);

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

	private:
		// Fills the identity fields and the generated form from the loaded part.
		void loadPart();
		// Restarts the §10 debounce — a burst of keystrokes becomes one write.
		void scheduleSave();
		// Fills the "+ Tag" menu with the tags this part does not carry yet.
		void refreshAddTagMenu();
		// Puts the datasheet row into one of its three states: none, attached, or attached but
		// missing from the file store. Which buttons are usable follows from that, so "nothing
		// attached yet" reads as a disabled Open/Remove rather than a button that does nothing.
		void updateDatasheetState();

		Ui::PartEditorDialog* m_ui;
		PartEditorController m_controller;
		// Same connection as m_controller, but every quantity change goes through §3's log.
		StockController m_stock;
		AttributeFormWidget* m_attributeForm;
		QTimer* m_saveTimer;
		Part m_part;
		// Blocks autosave while loadPart() writes into the widgets.
		bool m_loading = true;
		QString m_datasheetSourceUrl;
	};

}
