// @file PartManager_PartListMigration.h
// @brief Bulk migration of a pasted component list into the database (§4, §5).
//
// The sibling of PartManager_BomCsvImport.h, for the other direction of the same
// problem: a BOM says "this board needs these parts", a migration says "these
// parts are already in my drawer, put them in the database". The user arrives
// with whatever their old inventory was — a column copied out of Excel, a text
// file of order codes, a semicolon list mailed to them — so the entry point is
// the clipboard, not a file format.
//
// The paste shape this must not get wrong is the simplest one: a bare list with
// one part number per line and no header, no delimiter and no columns at all.
// detectDelimiter() cannot help there (it answers ';' for any separator-free
// line), so parsePastedList() checks for a separator itself and falls back to a
// synthetic single-column table rather than treating line one as a header.
//
// Everything here is free functions over plain data: no database, no widgets, no
// network. Whether a pasted number is already known is decided against a `Part`
// vector the caller supplies, exactly as buildRows() does it, so the same code
// runs in a test with three fake parts.
//
// A row is never written by this module. `status` only records what *would*
// happen — Existing for a duplicate, Pending for a new part the caller may
// create, Created/Skipped for what the caller then did. The decision stays with
// the dialog, same as §4's rule that an unmatched BOM line is never silently
// invented as a part.
// @see docs/design/ARCHITECTURE.md §4, §5
// @see PartManager_BomCsvImport.h, PartManager_Part.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartlistItem.h"
#include "domain/PartManager_Seller.h"
#include "import/PartManager_BomCsvImport.h"
#include <string>
#include <vector>

namespace PartManager
{

	// What the migration intends to do with one pasted line, or what it did.
	enum class MigrationStatus
	{
		Pending,    // new to the database; the caller may create it
		Existing,   // already in the database, matched by part number
		Created,    // the caller inserted it
		Skipped     // the caller chose not to
	};

	// Which column feeds which migration field; NoMigrationColumn = not mapped.
	constexpr int NoMigrationColumn = -1;
	struct PART_MANAGER_API MigrationColumnMapping
	{
		// The two part numbers are separate columns because they are separate things and they live
		// in separate places: `mpn` is the manufacturer's own number and ends up on `part.mpn`,
		// `mouser` is the distributor's article number and ends up on a `part_seller_link` row. A
		// list carrying only Mouser numbers used to have to pretend they were manufacturer ones,
		// which left New Part's Mouser field empty for exactly the lists that knew it.
		// Neither is required on its own — see hasPartNumberColumn().
		int mpn = NoMigrationColumn;           // the manufacturer part number
		int mouser = NoMigrationColumn;        // the Mouser article number ('595-LM358DR')
		int stock = NoMigrationColumn;         // stock count / quantity on hand
		int manufacturer = NoMigrationColumn;
		int description = NoMigrationColumn;
		int notes = NoMigrationColumn;
	};

	// One pasted line, ready to become a Part.
	struct PART_MANAGER_API MigrationRow
	{
		std::string mpn;
		std::string mouserPartNumber;    // the distributor number, empty when the list carries none
		std::string manufacturer;
		std::string description;
		std::string notes;
		int stock = 0;                   // 0 when unmapped or unparseable
		bool stockGiven = false;         // distinguishes "0 in stock" from "no stock column"
		MigrationStatus status = MigrationStatus::Pending;
		int matchedPartId = NoPartId;    // set when status == Existing
		int createdPartId = NoPartId;    // set when status == Created
		std::string rawJson;             // the whole original row, for `raw_import_data`
	};

	// The header a separator-free paste gets, so the mapping combo has something to name.
	constexpr const char* DefaultMigrationHeader = "Part Number";

	// Splits a clipboard paste into a table. With AutoDetectDelimiter a line carrying none of
	// ; , or tab is read as a plain one-value-per-line list — a single synthetic column, every
	// line a row — instead of being handed to the CSV reader, which would eat line one as a
	// header. Otherwise this is parseCsv(), plus synthetic "Column N" headers when `hasHeader`
	// is false so the first line stays a data row. Returns false only when `text` holds no
	// usable line at all.
	PART_MANAGER_API bool parsePastedList(const std::string& text, char delimiter,
		CsvTable& outTable, bool hasHeader);

	// Best guess at what the columns mean, by header name, in the spirit of guessMapping().
	// Recognises the English and German spellings a pasted inventory actually carries; anything
	// unrecognised is left NoMigrationColumn for the user to set. A single-column table is a
	// bare part-number list by definition, so column 0 is the MPN whatever it is called.
	//
	// The Mouser column is tested *before* the manufacturer one, because 'Mouser Part Number'
	// contains "part number" and would otherwise be taken for the manufacturer's. The reverse trap
	// does not exist: 'Manufacturer_Part_Number' names no distributor, so it stays the MPN.
	PART_MANAGER_API MigrationColumnMapping guessMigrationMapping(
		const std::vector<std::string>& headers);

	// Whether the mapping identifies a part at all — either part-number column will do.
	//
	// The decision behind it: the Part-number column is *not* strictly required any more. A list
	// that holds nothing but Mouser article numbers is a perfectly complete inventory — it names
	// every part exactly once and looks each one up better than an MPN does — so demanding a
	// manufacturer number the user does not have would block the migration on data that does not
	// exist. What stays required is that *some* number identifies the row.
	PART_MANAGER_API bool hasPartNumberColumn(const MigrationColumnMapping& mapping);

	// Turns the table into migration rows and resolves each against `existingParts`: a Mouser
	// number is matched case-insensitively against `PartSellerLink::sellerPartNumber` and a
	// manufacturer number against `part.mpn` — each against the field it actually belongs to. Both
	// then fall back to the other index, so a list whose single column was mapped to the "wrong"
	// one of the two is still recognised, exactly as it was before the Mouser column existed.
	// A hit on either number becomes Existing with `matchedPartId`; everything else stays Pending.
	// Rows whose mapped cells are all empty are dropped — trailing spreadsheet junk, not parts.
	PART_MANAGER_API std::vector<MigrationRow> buildMigrationRows(const CsvTable& table,
		const MigrationColumnMapping& mapping, const std::vector<Part>& existingParts,
		const std::vector<PartSellerLink>& sellerLinks = std::vector<PartSellerLink>());

	// Stable lowercase ascii key for a status — for storage, logging and tests. Never a display
	// string: core does not translate, so the app layer maps these to tr()'d text itself.
	PART_MANAGER_API const char* migrationStatusKey(MigrationStatus status);

}
