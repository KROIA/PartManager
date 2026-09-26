#include "import/PartManager_PartListMigration.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace PartManager
{
	namespace
	{
		std::string lowered(const std::string& text)
		{
			std::string out = text;
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string trimmed(const std::string& text)
		{
			size_t begin = 0;
			size_t end = text.size();
			while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) { ++begin; }
			while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) { --end; }
			return text.substr(begin, end - begin);
		}

		// Same rule as BomCsvImport's header matching: `_` and `-` read as spaces, so
		// 'Manufacturer_Part_Number' contains "part number" and a spreadsheet's 'Stock_Count'
		// contains "stock count".
		bool headerMatches(const std::string& header, std::initializer_list<const char*> needles)
		{
			std::string low = lowered(trimmed(header));
			std::replace(low.begin(), low.end(), '_', ' ');
			std::replace(low.begin(), low.end(), '-', ' ');
			for (const char* needle : needles)
			{
				if (low.find(needle) != std::string::npos)
				{
					return true;
				}
			}
			return false;
		}

		// A cell that is not mapped reads as empty rather than out-of-range.
		const std::string& cellAt(const std::vector<std::string>& row, int column)
		{
			static const std::string empty;
			if (column < 0 || static_cast<size_t>(column) >= row.size())
			{
				return empty;
			}
			return row[static_cast<size_t>(column)];
		}

		std::vector<std::string> splitLines(const std::string& text)
		{
			std::vector<std::string> lines;
			std::string line;
			for (const char c : text)
			{
				if (c == '\n')
				{
					if (!line.empty() && line.back() == '\r') { line.pop_back(); }
					lines.push_back(line);
					line.clear();
				}
				else
				{
					line += c;
				}
			}
			if (!line.empty() && line.back() == '\r') { line.pop_back(); }
			if (!line.empty()) { lines.push_back(line); }
			return lines;
		}

		bool hasAnyDelimiter(const std::string& line)
		{
			return line.find(';') != std::string::npos
				|| line.find(',') != std::string::npos
				|| line.find('\t') != std::string::npos;
		}

		// "12", " 12 ", "12 pcs", "12 Stk", "12x". Anything else is not a stock count: the
		// caller gets stockGiven == false and decides, rather than the row silently claiming
		// zero parts in the drawer.
		bool parseStock(const std::string& cell, int& outStock)
		{
			const std::string text = trimmed(cell);
			if (text.empty()) { return false; }

			size_t digits = 0;
			while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits])))
			{
				++digits;
			}
			if (digits == 0) { return false; }

			const std::string suffix = lowered(trimmed(text.substr(digits)));
			if (!suffix.empty() && suffix != "pcs" && suffix != "stk" && suffix != "x"
				&& suffix != "pc" && suffix != "stk." && suffix != "pcs.")
			{
				return false;
			}

			try
			{
				outStock = std::stoi(text.substr(0, digits));
			}
			catch (const std::exception&)
			{
				return false;   // more digits than an int holds is not a stock count either
			}
			return true;
		}
	}

	bool parsePastedList(const std::string& text, char delimiter, CsvTable& outTable, bool hasHeader)
	{
		outTable = CsvTable();

		std::vector<std::string> lines = splitLines(text);
		lines.erase(std::remove_if(lines.begin(), lines.end(),
			[](const std::string& line) { return trimmed(line).empty(); }), lines.end());
		if (lines.empty())
		{
			return false;
		}

		// The bare-list case: one part number per line, no separator anywhere on the first line.
		// detectDelimiter() would answer ';' here and parseCsv() would then eat line one as a
		// header, costing the user their first part. This is the paste the feature exists for.
		if (delimiter == AutoDetectDelimiter && !hasAnyDelimiter(lines.front()))
		{
			outTable.delimiter = detectDelimiter(lines.front());
			outTable.headers.push_back(DefaultMigrationHeader);
			size_t first = 0;
			if (hasHeader)
			{
				outTable.headers.front() = trimmed(lines.front());
				first = 1;
			}
			for (size_t i = first; i < lines.size(); ++i)
			{
				outTable.rows.push_back({ trimmed(lines[i]) });
			}
			// Excel's UTF-8 BOM would otherwise ride along on the first value.
			if (!outTable.rows.empty() && outTable.rows.front().front().rfind("\xEF\xBB\xBF", 0) == 0)
			{
				outTable.rows.front().front().erase(0, 3);
			}
			if (outTable.headers.front().rfind("\xEF\xBB\xBF", 0) == 0)
			{
				outTable.headers.front().erase(0, 3);
			}
			return true;
		}

		if (!parseCsv(text, delimiter, outTable))
		{
			return false;
		}

		if (!hasHeader)
		{
			// parseCsv() has taken line one for a header; put it back as a row and number the
			// columns instead. Prepending keeps the file's own order.
			std::vector<std::string> firstRow = outTable.headers;
			for (size_t i = 0; i < outTable.headers.size(); ++i)
			{
				outTable.headers[i] = "Column " + std::to_string(i + 1);
			}
			outTable.rows.insert(outTable.rows.begin(), firstRow);
		}
		return true;
	}

	MigrationColumnMapping guessMigrationMapping(const std::vector<std::string>& headers)
	{
		MigrationColumnMapping mapping;

		// A single column is a bare part-number list whatever its header says — that is the
		// only thing a one-column paste can be.
		if (headers.size() == 1)
		{
			mapping.mpn = 0;
			return mapping;
		}

		for (size_t i = 0; i < headers.size(); ++i)
		{
			const int column = static_cast<int>(i);
			const std::string& header = headers[i];

			// First match wins per field, and the chain order is the priority order:
			// 'Manufacturer Part Number' is a part number, not the manufacturer column, and a
			// Mouser column is tested before it because its header carries "part number" too.
			// "mouser" alone covers every spelling of it there is — 'Mouser', 'Mouser Part
			// Number', 'Mouser No', 'Mouser Nr', 'Mouser #', 'Mouser Artikelnummer' — because
			// headerMatches() is a substring test. The distributor spellings are the ones that
			// do not contain the word.
			if (mapping.mouser == NoMigrationColumn
				&& headerMatches(header, { "mouser", "distributor part number",
					"distributor part no" }))
			{
				mapping.mouser = column;
			}
			else if (mapping.mpn == NoMigrationColumn
				&& headerMatches(header, { "mpn", "part number", "partnumber", "part no",
					"order code", "ordercode", "manufacturer part", "sku",
					"article", "artikelnummer", "artikelnr", "bestellnummer" }))
			{
				mapping.mpn = column;
			}
			else if (mapping.stock == NoMigrationColumn
				&& headerMatches(header, { "quantity", "qty", "stock", "on hand", "onhand",
					"count", "bestand", "anzahl", "menge" }))
			{
				mapping.stock = column;
			}
			else if (mapping.manufacturer == NoMigrationColumn
				&& headerMatches(header, { "manufacturer", "hersteller", "brand", "maker",
					"vendor" }))
			{
				mapping.manufacturer = column;
			}
			else if (mapping.description == NoMigrationColumn
				&& headerMatches(header, { "description", "value", "comment", "beschreibung",
					"wert" }))
			{
				mapping.description = column;
			}
			else if (mapping.notes == NoMigrationColumn
				&& headerMatches(header, { "note", "notes", "remark", "comment", "bemerkung",
					"kommentar" }))
			{
				// 'Comment' appears in both lists on purpose: a file with a Description *and* a
				// Comment means the second one as a note, but a file with only a Comment means
				// it as the description. Whichever slot is still free gets it, description first.
				mapping.notes = column;
			}
		}
		return mapping;
	}

	bool hasPartNumberColumn(const MigrationColumnMapping& mapping)
	{
		return mapping.mpn != NoMigrationColumn || mapping.mouser != NoMigrationColumn;
	}

	std::vector<MigrationRow> buildMigrationRows(const CsvTable& table,
		const MigrationColumnMapping& mapping, const std::vector<Part>& existingParts,
		const std::vector<PartSellerLink>& sellerLinks)
	{
		// Two indices for the whole paste rather than a scan per row, kept apart so each pasted
		// number can be looked up in the place that number actually lives — part.mpn for a
		// manufacturer number, part_seller_link for a distributor one. First writer wins in both.
		std::unordered_map<std::string, int> byMpn;
		for (const Part& part : existingParts)
		{
			if (!part.mpn.empty()) { byMpn.emplace(lowered(part.mpn), part.id); }
		}
		// A pasted list is as likely to hold Mouser numbers as manufacturer ones — the user is
		// pasting their order history. Those live in part_seller_link, never in part.mpn.
		std::unordered_map<std::string, int> bySellerNumber;
		for (const PartSellerLink& link : sellerLinks)
		{
			if (!link.sellerPartNumber.empty())
			{
				bySellerNumber.emplace(lowered(link.sellerPartNumber), link.partId);
			}
		}

		// Field first, then the other index. The fallback is what keeps a list mapped entirely to
		// the Part-number column matching its Mouser numbers, which is how every such paste was
		// detected before this column existed — losing that would be a silent regression into
		// duplicating the inventory.
		const auto lookUp = [&byMpn, &bySellerNumber](const std::string& number, bool sellerFirst)
		{
			if (number.empty()) { return NoPartId; }
			const std::string key = lowered(number);
			const std::unordered_map<std::string, int>& first = sellerFirst ? bySellerNumber : byMpn;
			const std::unordered_map<std::string, int>& second = sellerFirst ? byMpn : bySellerNumber;
			auto found = first.find(key);
			if (found != first.end()) { return found->second; }
			found = second.find(key);
			return found != second.end() ? found->second : NoPartId;
		};

		std::vector<MigrationRow> out;
		out.reserve(table.rows.size());

		for (const std::vector<std::string>& row : table.rows)
		{
			MigrationRow migrationRow;
			migrationRow.mpn = trimmed(cellAt(row, mapping.mpn));
			migrationRow.mouserPartNumber = trimmed(cellAt(row, mapping.mouser));
			migrationRow.manufacturer = trimmed(cellAt(row, mapping.manufacturer));
			migrationRow.description = trimmed(cellAt(row, mapping.description));
			migrationRow.notes = trimmed(cellAt(row, mapping.notes));
			const std::string stockCell = trimmed(cellAt(row, mapping.stock));

			// A Mouser number alone is a whole row: the list that only carries those is the one
			// this column was added for, so it counts here like any other mapped cell.
			if (migrationRow.mpn.empty() && migrationRow.mouserPartNumber.empty()
				&& migrationRow.manufacturer.empty()
				&& migrationRow.description.empty() && migrationRow.notes.empty()
				&& stockCell.empty())
			{
				continue;   // a spreadsheet's trailing empty rows, not parts
			}

			int stock = 0;
			if (parseStock(stockCell, stock))
			{
				migrationRow.stock = stock;
				migrationRow.stockGiven = true;
			}

			// Either number identifying a part we already own makes the row Existing — the two
			// columns describe the same part from two sides, so one of them being known is enough.
			int matched = lookUp(migrationRow.mouserPartNumber, true);
			if (matched == NoPartId)
			{
				matched = lookUp(migrationRow.mpn, false);
			}
			if (matched != NoPartId)
			{
				migrationRow.status = MigrationStatus::Existing;
				migrationRow.matchedPartId = matched;
			}

			// The whole original row, header-keyed, so a row imported under a wrong mapping is
			// still readable afterwards.
			QJsonObject json;
			for (size_t column = 0; column < table.headers.size() && column < row.size(); ++column)
			{
				json.insert(QString::fromStdString(table.headers[column]),
					QString::fromStdString(row[column]));
			}
			migrationRow.rawJson = QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();

			out.push_back(migrationRow);
		}
		return out;
	}

	const char* migrationStatusKey(MigrationStatus status)
	{
		switch (status)
		{
			case MigrationStatus::Pending:  return "pending";
			case MigrationStatus::Existing: return "existing";
			case MigrationStatus::Created:  return "created";
			case MigrationStatus::Skipped:  return "skipped";
		}
		return "pending";
	}

}
