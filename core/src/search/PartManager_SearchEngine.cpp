#include "search/PartManager_SearchEngine.h"
#include "persistence/PartManager_PartRepository.h"
#include "PartManager_global.h"

#include <cmath>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// One bound SQL parameter. SQLiteWrapper's own executeWithParams() binds everything as text,
		// which would compare a typed number against a REAL `attr_*` column as a string, so the
		// numeric terms need real sqlite3_bind_double() and this small statement helper of their own.
		struct BoundValue
		{
			bool numeric = false;
			double number = 0.0;
			std::string text;

			static BoundValue fromNumber(double value)
			{
				BoundValue bound;
				bound.numeric = true;
				bound.number = value;
				return bound;
			}
			static BoundValue fromText(const std::string& value)
			{
				BoundValue bound;
				bound.text = value;
				return bound;
			}
		};

		// Runs a single-column SELECT of part ids with the given parameters bound. Returns an empty
		// list on any SQLite error (a missing column, a malformed statement) — never throws.
		std::vector<int> fetchIds(SQLiteWrapper::SQLite& db, const std::string& sql,
			const std::vector<BoundValue>& binds)
		{
			std::vector<int> ids;
			sqlite3_stmt* statement = nullptr;
			if (sqlite3_prepare_v2(db.getDB(), sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
			{
				return ids;
			}
			for (size_t i = 0; i < binds.size(); ++i)
			{
				const BoundValue& bound = binds[i];
				const int index = static_cast<int>(i) + 1;
				if (bound.numeric)
				{
					sqlite3_bind_double(statement, index, bound.number);
				}
				else
				{
					sqlite3_bind_text(statement, index, bound.text.c_str(),
						static_cast<int>(bound.text.size()), SQLITE_TRANSIENT);
				}
			}
			while (sqlite3_step(statement) == SQLITE_ROW)
			{
				ids.push_back(sqlite3_column_int(statement, 0));
			}
			sqlite3_finalize(statement);
			return ids;
		}

		bool attrColumnExists(SQLiteWrapper::SQLite& db, const std::string& key)
		{
			const std::string column = "attr_" + key;
			for (const std::vector<std::string>& row : db.fetchAll("PRAGMA table_info(part);"))
			{
				if (row.size() > 1 && row[1] == column)
				{
					return true;
				}
			}
			return false;
		}

		// §2a: an `attr_*` value that went through an SI prefix is rarely bit-identical to the number
		// the user types back, so equality gets a relative window. The floor keeps `= 0` meaningful.
		double equalityTolerance(double value)
		{
			const double relative = std::fabs(value) * 0.005;
			return relative > 1e-15 ? relative : 1e-15;
		}

		const char* comparisonSymbol(SearchCompareOp op)
		{
			switch (op)
			{
			case SearchCompareOp::Less:         return "<";
			case SearchCompareOp::LessEqual:    return "<=";
			case SearchCompareOp::Greater:      return ">";
			case SearchCompareOp::GreaterEqual: return ">=";
			default:                            return "=";
			}
		}
	}

	std::vector<int> SearchEngine::searchIds(SQLiteWrapper::SQLite& db, const SearchQuery& query, int partTypeId)
	{
		if (!query.ok)
		{
			return {};
		}
		if (!query.tagNames.empty() && (!db.tableExists("part_tag") || !db.tableExists("tag")))
		{
			return {};   // tag schema was never created in this database, so no part can carry a tag
		}

		std::string sql = "SELECT id FROM part WHERE 1=1";
		std::vector<BoundValue> binds;

		if (partTypeId != 0)
		{
			sql += " AND part_type_id=?";
			binds.push_back(BoundValue::fromNumber(partTypeId));
		}

		// instr() rather than LIKE: no wildcard metacharacters means nothing in the user's text
		// needs escaping, and a typed '%' searches for a literal per cent sign as anyone would expect.
		for (const std::string& term : query.textTerms)
		{
			sql += " AND (instr(lower(ifnull(name,'')),?)>0"
				" OR instr(lower(ifnull(mpn,'')),?)>0"
				" OR instr(lower(ifnull(manufacturer,'')),?)>0"
				" OR instr(lower(ifnull(description,'')),?)>0)";
			for (int i = 0; i < 4; ++i)
			{
				binds.push_back(BoundValue::fromText(term));
			}
		}

		for (const SearchAttributeTerm& term : query.attributeTerms)
		{
			if (!attrColumnExists(db, term.key))
			{
				return {};   // nothing in this database has that attribute, so nothing can match it
			}
			// Safe to concatenate: SearchQuery::parse() rejects any key that is not [A-Za-z0-9_].
			const std::string column = "attr_" + term.key;
			sql += " AND " + column + " IS NOT NULL AND ";
			if (term.op == SearchCompareOp::Equal || term.op == SearchCompareOp::NotEqual)
			{
				sql += "abs(" + column + " - ?)" +
					(term.op == SearchCompareOp::Equal ? std::string("<=?") : std::string(">?"));
				binds.push_back(BoundValue::fromNumber(term.value));
				binds.push_back(BoundValue::fromNumber(equalityTolerance(term.value)));
			}
			else
			{
				sql += column + comparisonSymbol(term.op) + "?";
				binds.push_back(BoundValue::fromNumber(term.value));
			}
		}

		for (const std::string& tagName : query.tagNames)
		{
			sql += " AND EXISTS (SELECT 1 FROM part_tag pt JOIN tag t ON t.id=pt.tag_id"
				" WHERE pt.part_id=part.id AND lower(t.name)=?)";
			binds.push_back(BoundValue::fromText(tagName));
		}

		sql += " ORDER BY id;";
		return fetchIds(db, sql, binds);
	}

	std::vector<Part> SearchEngine::search(SQLiteWrapper::SQLite& db, const SearchQuery& query, int partTypeId)
	{
		std::vector<Part> result;
		// ponytail: one findPart() per hit instead of re-selecting the whole row set here — it reuses
		// PartRepository's row mapping instead of growing a second copy of it, at the cost of N+1
		// statements. Give PartRepository a listPartsByIds() if a filter keystroke ever feels slow.
		for (int id : searchIds(db, query, partTypeId))
		{
			Part part;
			if (PartRepository::findPart(db, id, part))
			{
				result.push_back(part);
			}
		}
		return result;
	}

	std::vector<Part> SearchEngine::search(SQLiteWrapper::SQLite& db, const std::string& queryText, int partTypeId)
	{
		return search(db, SearchQuery::parse(queryText), partTypeId);
	}

#endif

}
