// @file PartManager_BomCsvImport.h
// @brief Generic CSV/BOM reading and column mapping for partlist import (§4, §5).
//
// Deliberately *not* hardcoded to one exporter's layout: §5 says KiCad's BOM
// export, Excel and anything else have to come through the same path, so the
// file is read as an anonymous table of strings and a `BomColumnMapping` says
// which column means what. guessMapping() only pre-fills that mapping — the
// import dialog always shows it and the user always gets to correct it.
//
// The layout it is *tuned* for is KiCad 9's grouped BOM export, because that is
// the file this project is actually fed:
//
//     Reference;Qty;Value;DNP;Mouser Part Number;Manufacturer_Name;Manufacturer_Part_Number
//
// Both part-number columns are mapped (`mpn` + `mpnAlt`) rather than only the
// first: a KiCad BOM fills whichever of the two the schematic symbol happened to
// carry, so per row either one may be the only identity there is.
//
// Everything here is free functions over plain data: no database, no widgets,
// no network. Matching a row to a part is a lookup against a `Part` vector the
// caller supplies, so the same code runs in a test with three fake parts.
//
// A row that matches nothing stays unmatched. It is never turned into a new
// typeless part — decided 2026-09-01: the user resolves it by hand, or the
// import dialog offers a Mouser lookup when a key and a network are there.
// @see docs/design/ARCHITECTURE.md §4, §5
// @see PartManager_PartlistItem.h, PartManager_PartlistRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartlistItem.h"
#include "domain/PartManager_Seller.h"
#include <string>
#include <vector>

namespace PartManager
{

	// Delimiter value meaning "work it out from the header line".
	constexpr char AutoDetectDelimiter = '\0';

	// A CSV file as read: one header row plus data rows, every cell a string.
	// Short rows are padded so `row[i]` is always safe for any i < headers.size().
	struct PART_MANAGER_API CsvTable
	{
		std::vector<std::string> headers;
		std::vector<std::vector<std::string>> rows;
		char delimiter = ';';   // what was actually used, so the dialog can show it
	};

	// Which column feeds which BOM field; NoCsvColumn = not mapped.
	constexpr int NoCsvColumn = -1;
	struct PART_MANAGER_API BomColumnMapping
	{
		int designators = NoCsvColumn;   // 'Reference'/'Designator' — R1,R2,R5
		int mpn = NoCsvColumn;           // the order code, what matching keys off first
		// The *other* part-number column. KiCad's BOM export routinely carries both a
		// distributor number ('Mouser Part Number') and a manufacturer one
		// ('Manufacturer_Part_Number'), and which of the two a given row fills in is a
		// property of the row, not of the file — so both are mapped and both are tried.
		int mpnAlt = NoCsvColumn;
		int quantity = NoCsvColumn;      // per unit; absent => counted from the designators
		int name = NoCsvColumn;          // 'Value'/'Comment' — the fallback match and the row label
	};

	// One BOM line, ready to become a PartlistItem.
	struct PART_MANAGER_API BomRow
	{
		std::string designators;
		std::string mpn;
		std::string mpnAlt;              // the second part-number cell, empty when unmapped
		std::string name;
		int quantityPerUnit = 1;
		int matchedPartId = NoPartId;    // NoPartId => unresolved (§4)
		std::string rawJson;             // the whole original row, for `raw_import_data`
	};

	// The part number this row is best identified by: the primary cell, the alternative
	// when that is empty, the value/name column when neither is filled. What the Mouser
	// lookup searches for and what the preview shows.
	PART_MANAGER_API const std::string& bomRowIdentity(const BomRow& row);

	// Splits `text` into a table. Handles RFC-4180 quoting ("a;b", "" for a literal quote),
	// CRLF and LF, and pads short rows. Returns false only when there is no header line at all.
	// `delimiter` may be AutoDetectDelimiter, in which case the header line decides.
	PART_MANAGER_API bool parseCsv(const std::string& text, char delimiter, CsvTable& outTable);

	// Whichever of ; , or tab occurs most often in `headerLine`; ';' when none does.
	PART_MANAGER_API char detectDelimiter(const std::string& headerLine);

	// Best guess at what the columns mean, by header name. Recognises KiCad's own BOM headers
	// and the usual spreadsheet spellings; anything unrecognised is left NoCsvColumn for the
	// user to set. A second part-number column becomes `mpnAlt`; a distributor number is
	// preferred as the primary one, because that is what an order can be placed against.
	PART_MANAGER_API BomColumnMapping guessMapping(const std::vector<std::string>& headers);

	// Turns the table into BOM lines and resolves each against `existingParts`. Match order per
	// row: the two part-number cells first (case-insensitive, against `part.mpn` and against any
	// distributor number in `sellerLinks`), then the name column against a part's MPN or name.
	// Rows whose mapped fields are all empty are dropped — trailing spreadsheet junk, not BOM
	// lines. `sellerLinks` may be empty, in which case only `part.mpn` is matchable.
	PART_MANAGER_API std::vector<BomRow> buildRows(const CsvTable& table,
		const BomColumnMapping& mapping, const std::vector<Part>& existingParts,
		const std::vector<PartSellerLink>& sellerLinks = std::vector<PartSellerLink>());

	// How many designators a 'R1,R2 R5' style cell lists. 0 for an empty cell.
	PART_MANAGER_API int countDesignators(const std::string& designators);

}
