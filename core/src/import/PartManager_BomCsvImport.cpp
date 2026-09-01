#include "import/PartManager_BomCsvImport.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cctype>

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

		bool headerMatches(const std::string& header, std::initializer_list<const char*> needles)
		{
			const std::string low = lowered(trimmed(header));
			for (const char* needle : needles)
			{
				if (low.find(needle) != std::string::npos)
				{
					return true;
				}
			}
			return false;
		}

		// One CSV line -> cells. Quotes are RFC-4180: a quoted field may contain the delimiter
		// and "" stands for one quote character.
		std::vector<std::string> splitLine(const std::string& line, char delimiter)
		{
			std::vector<std::string> cells;
			std::string cell;
			bool inQuotes = false;
			for (size_t i = 0; i < line.size(); ++i)
			{
				const char c = line[i];
				if (inQuotes)
				{
					if (c == '"')
					{
						if (i + 1 < line.size() && line[i + 1] == '"')
						{
							cell += '"';
							++i;
						}
						else
						{
							inQuotes = false;
						}
					}
					else
					{
						cell += c;
					}
				}
				else if (c == '"')
				{
					inQuotes = true;
				}
				else if (c == delimiter)
				{
					cells.push_back(trimmed(cell));
					cell.clear();
				}
				else
				{
					cell += c;
				}
			}
			cells.push_back(trimmed(cell));
			return cells;
		}

		// Splits on LF and drops a trailing CR, so a file saved on either platform reads the same.
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

		bool isBlankLine(const std::string& line)
		{
			return trimmed(line).find_first_not_of(",;\t") == std::string::npos;
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
	}

	char detectDelimiter(const std::string& headerLine)
	{
		// Counted outside quotes: a single quoted header like "Ref, Value" would otherwise
		// hand the vote to the comma in a semicolon file.
		int counts[3] = { 0, 0, 0 };
		const char candidates[3] = { ';', ',', '\t' };
		bool inQuotes = false;
		for (const char c : headerLine)
		{
			if (c == '"') { inQuotes = !inQuotes; continue; }
			if (inQuotes) { continue; }
			for (int i = 0; i < 3; ++i)
			{
				if (c == candidates[i]) { ++counts[i]; }
			}
		}
		int best = 0;
		for (int i = 1; i < 3; ++i)
		{
			if (counts[i] > counts[best]) { best = i; }
		}
		// No separator at all means a one-column file; ';' keeps it a single column either way.
		return counts[best] == 0 ? ';' : candidates[best];
	}

	bool parseCsv(const std::string& text, char delimiter, CsvTable& outTable)
	{
		outTable = CsvTable();

		std::vector<std::string> lines = splitLines(text);
		lines.erase(std::remove_if(lines.begin(), lines.end(), isBlankLine), lines.end());
		if (lines.empty())
		{
			return false;
		}

		outTable.delimiter = delimiter == AutoDetectDelimiter ? detectDelimiter(lines.front()) : delimiter;
		outTable.headers = splitLine(lines.front(), outTable.delimiter);

		// Excel writes a UTF-8 BOM in front of the first header, which would otherwise make that
		// header match nothing in guessMapping() and show up with an invisible character.
		if (!outTable.headers.empty() && outTable.headers.front().rfind("\xEF\xBB\xBF", 0) == 0)
		{
			outTable.headers.front().erase(0, 3);
		}

		for (size_t i = 1; i < lines.size(); ++i)
		{
			std::vector<std::string> row = splitLine(lines[i], outTable.delimiter);
			row.resize(outTable.headers.size());
			outTable.rows.push_back(row);
		}
		return true;
	}

	BomColumnMapping guessMapping(const std::vector<std::string>& headers)
	{
		BomColumnMapping mapping;
		for (size_t i = 0; i < headers.size(); ++i)
		{
			const int column = static_cast<int>(i);
			const std::string& header = headers[i];

			// First match wins per field: a KiCad BOM has both "Reference" and "References",
			// and a spreadsheet often has both "MPN" and "Mouser Part Number".
			if (mapping.designators == NoCsvColumn
				&& headerMatches(header, { "designator", "reference", "refdes" }))
			{
				mapping.designators = column;
			}
			else if (mapping.mpn == NoCsvColumn
				// "mouser" catches the user's own "MouserNR" heading; it is a distributor number
				// rather than a manufacturer one, but it is still what the row is identified by.
				&& headerMatches(header, { "mpn", "part number", "partnumber", "part no",
					"order code", "ordercode", "manufacturer part", "mouser", "sku" }))
			{
				mapping.mpn = column;
			}
			else if (mapping.quantity == NoCsvColumn
				&& headerMatches(header, { "quantity", "qty", "count", "stockcount" }))
			{
				mapping.quantity = column;
			}
			else if (mapping.name == NoCsvColumn
				&& headerMatches(header, { "value", "comment", "name", "description" }))
			{
				mapping.name = column;
			}
		}
		return mapping;
	}

	int countDesignators(const std::string& designators)
	{
		int count = 0;
		bool inToken = false;
		for (const char c : designators)
		{
			const bool separator = c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c));
			if (separator)
			{
				inToken = false;
			}
			else if (!inToken)
			{
				inToken = true;
				++count;
			}
		}
		return count;
	}

	std::vector<BomRow> buildRows(const CsvTable& table, const BomColumnMapping& mapping,
		const std::vector<Part>& existingParts)
	{
		std::vector<BomRow> out;
		out.reserve(table.rows.size());

		for (const std::vector<std::string>& row : table.rows)
		{
			BomRow bomRow;
			bomRow.designators = cellAt(row, mapping.designators);
			bomRow.mpn = cellAt(row, mapping.mpn);
			bomRow.name = cellAt(row, mapping.name);

			if (bomRow.designators.empty() && bomRow.mpn.empty() && bomRow.name.empty())
			{
				continue;   // a spreadsheet's trailing empty rows, not BOM lines
			}

			// A quantity column wins; otherwise the designator count is the quantity, which is
			// what a KiCad BOM grouped by value means. Neither present => one.
			bomRow.quantityPerUnit = 1;
			const std::string quantityCell = cellAt(row, mapping.quantity);
			bool quantityRead = false;
			if (!quantityCell.empty())
			{
				try
				{
					const int parsed = std::stoi(quantityCell);
					if (parsed > 0)
					{
						bomRow.quantityPerUnit = parsed;
						quantityRead = true;
					}
				}
				catch (const std::exception&)
				{
					// A non-numeric quantity is not worth failing the whole import over; the
					// designator count below is a better guess than refusing the row.
				}
			}
			if (!quantityRead)
			{
				bomRow.quantityPerUnit = std::max(1, countDesignators(bomRow.designators));
			}

			// MPN first, exactly: that is the identity a BOM and the inventory actually share.
			// The name column is only tried afterwards, because 'Value' columns hold things like
			// "4k7" that match far too eagerly.
			const std::string mpnKey = lowered(bomRow.mpn);
			const std::string nameKey = lowered(bomRow.name);
			for (const Part& part : existingParts)
			{
				if (!mpnKey.empty() && lowered(part.mpn) == mpnKey)
				{
					bomRow.matchedPartId = part.id;
					break;
				}
			}
			if (bomRow.matchedPartId == NoPartId && !nameKey.empty())
			{
				for (const Part& part : existingParts)
				{
					if (lowered(part.mpn) == nameKey || lowered(part.name) == nameKey)
					{
						bomRow.matchedPartId = part.id;
						break;
					}
				}
			}

			// The whole original row, header-keyed, so an unresolved line can still be read back
			// months later even if the mapping was wrong.
			QJsonObject json;
			for (size_t column = 0; column < table.headers.size() && column < row.size(); ++column)
			{
				json.insert(QString::fromStdString(table.headers[column]),
					QString::fromStdString(row[column]));
			}
			bomRow.rawJson = QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();

			out.push_back(bomRow);
		}
		return out;
	}

}
