// @file PartManager_PartlistController.h
// @brief Widget-free logic behind the partlist manager and editor screens (§4, §12b).
//
// Same split as the other controllers: the pure parts (the summary line a
// manager row shows, the shortfall wording) are free functions unit-tested in
// TST_PartlistController; the controller class is the thin PartlistRepository
// wrapper the dialogs talk to.
//
// Deliberately stops short of §4's checkout flow — "Check Stock" pushing
// shortfall rows into a `mouser_order` draft is item 9 and needs tables that do
// not exist yet. The shortfall *number* is already here, because the editor
// shows it per line and item 9 will want the same arithmetic rather than a
// second copy of it.
// @see docs/design/ARCHITECTURE.md §4, §12b
// @see PartManager_PartlistRepository.h, PartManager_StockController.h
#pragma once

#include "database/PartManager_DatabaseHandle.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_Seller.h"
#include "persistence/PartManager_PartlistRepository.h"
#include <QString>
#include <map>
#include <string>
#include <vector>

namespace PartManager
{

	// Display text for a `partlist.source`. The vocabulary is app chrome and gets tr()'d;
	// an unknown/legacy string is shown unchanged rather than dropped.
	QString partlistSourceLabel(const std::string& source);

	// The one-line state of a partlist's rows, for the editor's footer: how many lines are still
	// unresolved and how many are short. Empty when everything is resolved and in stock, which is
	// the state that needs no words.
	QString partlistStatusSummary(const std::vector<PartlistLine>& lines);

	// How many of `lines` still point at no part (§4). The editor blocks nothing on this — an
	// unresolved row is a to-do, not an error — but it is what the row highlight keys off.
	int unresolvedCount(const std::vector<PartlistLine>& lines);

	// The label a part gets in the editor's picker and its Part column: "name (MPN)", or just the
	// name when there is no MPN. Never tr()'d, both halves are the user's own data.
	QString partPickerLabel(const Part& part);

	// The designator list of two lines that turned out to be the same part, merged into one.
	//
	// A part may sit on a partlist exactly once — a second row would measure its own shortfall
	// against the same untouched stock and under-order the build. So adding a part the list
	// already carries merges the two rows, and the designators are the half that must survive
	// that: they are the BMKs on the board (`R1,R4`), and losing one loses a placement.
	//
	// Split on comma or semicolon, whitespace trimmed, duplicates dropped, order kept, joined
	// with ", ". Never tr()'d — designators are the user's own data.
	std::string mergeDesignators(const std::string& first, const std::string& second);

	// Collapses rows that point at the same part into the first of them, in place. Quantities are
	// summed and designators mergeDesignators()'d; unresolved rows (partId == NoPartId) are left
	// alone, since "not matched yet" is not a part they have in common. Returns true when anything
	// was merged, which is the caller's cue to write the list back.
	bool mergeDuplicateItems(std::vector<PartlistItem>& items);

	// PartlistRepository wrapper shared by PartlistManagerDialog and PartlistEditorDialog.
	// Holds the caller's DatabaseHandle without owning it — MainWindow's controller does.
	class PartlistController
	{
	public:
		explicit PartlistController(DatabaseHandle* handle);

		std::vector<Partlist> partlists() const;
		int itemCount(int partlistId) const;
		bool load(int partlistId, Partlist& outPartlist) const;
		// Returns the new id, NoPartlistId on failure.
		int create(const Partlist& partlist) const;
		bool save(const Partlist& partlist) const;
		// Takes the list's items with it.
		bool remove(int partlistId) const;

		// The editor's grid: items joined against `part`, needed/shortfall already worked out.
		std::vector<PartlistLine> lines(int partlistId) const;
		// Replaces the list's items wholesale — see PartlistRepository::saveItems().
		bool saveItems(int partlistId, const std::vector<PartlistItem>& items) const;

		// Every part in the database, for the editor's part picker. Sorted by name so the
		// combo is navigable by typing.
		std::vector<Part> allParts() const;

		// Absolute path of every part's `part_file(role='image')`, by part id. One query for the
		// whole grid rather than one per row — the same trade MainWindowController::partsFor()
		// makes for the table's thumbnails, and for the same reason.
		std::map<int, QString> imagePaths() const;

		// Every part_seller_link row, so a BOM's distributor-number column ('Mouser Part
		// Number') can be matched too — those numbers live nowhere in `part.mpn`.
		std::vector<PartSellerLink> allSellerLinks() const;

		// The handle this controller borrows, for a dialog that needs one of its own (the CSV
		// import opens New Part, which talks to PartEditorController). Never owned here.
		DatabaseHandle* handle() const { return m_handle; }

	private:
		DatabaseHandle* m_handle;
	};

}
