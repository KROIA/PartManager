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
#include "persistence/PartManager_PartlistRepository.h"
#include <QString>
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

		// The handle this controller borrows, for a dialog that needs one of its own (the CSV
		// import opens New Part, which talks to PartEditorController). Never owned here.
		DatabaseHandle* handle() const { return m_handle; }

	private:
		DatabaseHandle* m_handle;
	};

}
