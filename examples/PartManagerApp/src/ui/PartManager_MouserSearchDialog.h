// @file PartManager_MouserSearchDialog.h
// @brief The embedded Mouser search browser (`mouser-search.svg`, §6) — find a part, hand it on.
//
// One search box rather than the usual part-number/keyword radio pair: a part
// number search is tried first and a keyword search only runs when that came
// back empty, which is the same answer a user would get after flipping the
// switch themselves.
//
// MouserClient is synchronous, so a search really does freeze the dialog for up
// to its timeout — the status line says so and the cursor changes, rather than
// leaving the window looking hung. The dialog does no database work at all; it
// produces a MouserPartPrefill and NewPartDialog decides what to do with it.
// @see docs/design/ARCHITECTURE.md §6, §12b
// @see PartManager_MouserSearchDialog.cpp, PartManager_NewPartDialog.h
#pragma once

#include "mouser/PartManager_MouserClient.h"
#include "mouser/PartManager_MouserSearchService.h"
#include <QDialog>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <vector>

class QThreadPool;

namespace Ui { class MouserSearchDialog; }

namespace PartManager
{

	class MouserSearchDialog : public QDialog
	{
		Q_OBJECT
	public:
		explicit MouserSearchDialog(QWidget* parent = nullptr);
		~MouserSearchDialog() override;

		// The row "Use this part" was pressed on. Only meaningful after exec() returned Accepted.
		const MouserPartPrefill& selectedPrefill() const;

		// Opens on an already-known query and searches it straight away — how the CSV import
		// arrives here, with the unmatched row's part number in hand. Does nothing without an
		// API key; the constructor has already said so on the status line.
		void searchFor(const QString& query);

	private slots:
		// Runs the search and refills the table. Blocking, see the header note.
		void search();
		// Turns the selected row into a prefill and closes.
		void useSelected();
		// Opens the selected row's ProductDetailUrl in the system browser (§6).
		void openOnMouser();
		// Both footer buttons need a selected row that actually has a URL behind it.
		void updateButtons();

	private:
		// Fills the table from m_results, already ranked.
		void showResults();
		// The MouserPartDto behind the selected row, nullptr when nothing is selected.
		const MouserPartDto* selectedDto() const;

		// Queues one product photo for download, or paints it straight away if it is already in
		// hand. Downloads run on m_thumbnailPool because FileStore::downloadBytes() blocks — 25
		// rows fetched on the GUI thread would freeze the dialog for several seconds, which is
		// exactly the stall the search itself already costs and does not need doubling.
		void requestThumbnail(const QString& url);
		// Paints `url`'s picture onto every row that shows it. Matched by URL rather than by row
		// index on purpose: a download that lands after the user searched again would otherwise
		// paint a picture onto whatever part now occupies that row.
		void applyThumbnail(const QString& url);

		// Asks EasyEDA whether it carries `mpn`, so the row can say up front whether a KiCad
		// symbol and footprint will be there after the import. Only the *search* endpoint is
		// called — one request per row rather than the two a full fetch would need, and the
		// answer ("is there an exact match") is all a glyph can express anyway.
		void requestEcadAvailability(const QString& mpn, const QString& manufacturer);
		// Repaints the file glyphs of every row for `mpn`, from m_ecadByMpn plus the datasheet
		// state, which needs no lookup at all.
		void applyFileGlyphs(const QString& mpn);

		Ui::MouserSearchDialog* m_ui;
		MouserClient m_client;
		std::vector<MouserPartDto> m_results;
		MouserPartPrefill m_prefill;
		// Mouser reuses one stock photo across a whole series, so a 25-row result set is
		// routinely a handful of distinct pictures. Cache holds a null pixmap for a failed or
		// undecodable download too, so a dead URL is attempted once and not once per search.
		QHash<QString, QPixmap> m_thumbnails;
		QSet<QString> m_thumbnailsInFlight;
		QThreadPool* m_thumbnailPool;
		// MPN -> does EasyEDA carry an exact match. Absent means "not looked up yet", which the
		// row draws as the faint outline — the same thing it draws for a definite no, because
		// until the answer arrives those two really are indistinguishable to the user.
		QHash<QString, bool> m_ecadByMpn;
		QSet<QString> m_ecadInFlight;
	};

}
