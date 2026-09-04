#include "search/PartManager_SearchEngine.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cmath>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	namespace
	{
		std::string toLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}
	}

	std::vector<std::string> SearchEngine::keywordLines(const std::string& keywordList)
	{
		std::vector<std::string> words;
		size_t start = 0;
		while (start <= keywordList.size())
		{
			const size_t end = keywordList.find('\n', start);
			std::string line = keywordList.substr(start,
				(end == std::string::npos ? keywordList.size() : end) - start);
			// A '\r' from a list pasted in from somewhere else is whitespace at the end of the
			// line, so trimming both sides takes care of it along with stray spaces.
			const size_t first = line.find_first_not_of(" \t\r");
			const size_t last = line.find_last_not_of(" \t\r");
			if (first != std::string::npos)
			{
				line = line.substr(first, last - first + 1);
				if (std::find(words.begin(), words.end(), line) == words.end())
				{
					words.push_back(line);
				}
			}
			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1;
		}
		return words;
	}

	std::vector<std::string> SearchEngine::keywordsMatching(const std::string& keywordList,
		const std::string& term)
	{
		std::vector<std::string> matches;
		if (term.empty())
		{
			return matches;
		}
		const std::string needle = toLower(term);
		for (const std::string& word : keywordLines(keywordList))
		{
			if (toLower(word).compare(0, needle.size(), needle) == 0)
			{
				matches.push_back(word);
			}
		}
		return matches;
	}

	bool SearchEngine::keywordListMatches(const std::string& keywordList, const std::string& term)
	{
		return !keywordsMatching(keywordList, term).empty();
	}

	namespace
	{
		// `words` minus everything in `removeList`, compared whole and case-insensitively — the
		// unticked boxes. Whole-word and not prefix, unlike the search itself: the boxes are drawn
		// from the very list being filtered, so there is an exact word behind every one of them.
		void removeKeywords(std::vector<std::string>& words, const std::string& removeList)
		{
			for (const std::string& excluded : SearchEngine::keywordLines(removeList))
			{
				const std::string lowered = toLower(excluded);
				words.erase(std::remove_if(words.begin(), words.end(),
					[&lowered](const std::string& word) { return toLower(word) == lowered; }),
					words.end());
			}
		}

		std::string joinKeywords(const std::vector<std::string>& words)
		{
			std::string joined;
			for (const std::string& word : words)
			{
				joined += joined.empty() ? word : "\n" + word;
			}
			return joined;
		}
	}

	std::string SearchEngine::inheritedKeywords(const std::vector<PartType>& types, int typeId)
	{
		// The chain is walked child -> root because that is the direction the links point, but the
		// words have to be accumulated root -> child: a type's exclusions apply to what it inherited
		// from above, and its own words are added after them, so an ancestor's word it drops is gone
		// while a word of its own by the same name is not.
		std::vector<const PartType*> chain;
		std::vector<int> visited;
		int current = typeId;
		while (current != NoParentType
			&& std::find(visited.begin(), visited.end(), current) == visited.end())
		{
			visited.push_back(current);
			const PartType* found = nullptr;
			for (const PartType& type : types)
			{
				if (type.id == current)
				{
					found = &type;
				}
			}
			if (found == nullptr)
			{
				break;
			}
			chain.push_back(found);
			current = found->parentTypeId;
		}

		std::vector<std::string> words;
		// Root first, so "everything a Capacitor answers to, then what a Ceramic one adds" reads
		// the way the inheritance does. The order only shows in the editors' checkbox lists.
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			removeKeywords(words, (*it)->excludedKeywords);
			for (const std::string& word : keywordLines((*it)->searchKeywords))
			{
				if (std::find(words.begin(), words.end(), word) == words.end())
				{
					words.push_back(word);
				}
			}
		}
		return joinKeywords(words);
	}

	std::string SearchEngine::effectiveKeywords(const std::vector<PartType>& types, const Part& part)
	{
		std::vector<std::string> words = keywordLines(inheritedKeywords(types, part.partTypeId));
		removeKeywords(words, part.excludedKeywords);
		for (const std::string& word : keywordLines(part.searchKeywords))
		{
			if (std::find(words.begin(), words.end(), word) == words.end())
			{
				words.push_back(word);
			}
		}
		return joinKeywords(words);
	}

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
		if (!query.tagGroups.empty() && (!db.tableExists("part_tag") || !db.tableExists("tag")))
		{
			return {};   // tag schema was never created in this database, so no part can carry a tag
		}

		// Per type, the inherited search words that answer to `term` — so a part inherits its
		// category's keywords without them ever being copied onto the part. The words and not just
		// a yes/no, because a part may have unticked some of them and the SQL has to ask which.
		struct TypeKeywordHit { int typeId; std::vector<std::string> words; };
		auto typesMatchingKeyword = [](SQLiteWrapper::SQLite& database, const std::string& term)
		{
			std::vector<TypeKeywordHit> hits;
			const std::vector<PartType> types = PartTypeRepository::listTypes(database);
			for (const PartType& type : types)
			{
				std::vector<std::string> words = SearchEngine::keywordsMatching(
					SearchEngine::inheritedKeywords(types, type.id), term);
				if (!words.empty())
				{
					hits.push_back({ type.id, std::move(words) });
				}
			}
			return hits;
		};

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
				" OR instr(lower(ifnull(description,'')),?)>0"
				// The stored attribute values, as the JSON they live in. Searching the raw text
				// means the keys match too ("dielectric" finds every part that has one), which is
				// more useful than not and is what a user typing into one box expects.
				" OR instr(lower(ifnull(attributes,'')),?)>0"
				// This part's own search words. char(10)|| on both sides makes it a per-line
				// *prefix* match rather than a substring one: "r" finds the keyword "R" and "Res"
				// but not the "r" inside "Ferrite", which is the difference between a keyword list
				// that narrows a search and one that matches everything.
				" OR instr(char(10)||lower(ifnull(search_keywords,'')),char(10)||?)>0"
				// Its tags, by name — the same names the `tag:` term matches exactly, reachable
				// here without the prefix for a user who does not know the grammar.
				" OR EXISTS (SELECT 1 FROM part_tag pt JOIN tag t ON t.id=pt.tag_id"
				" WHERE pt.part_id=part.id AND instr(lower(t.name),?)>0)";
			for (int i = 0; i < 7; ++i)
			{
				binds.push_back(BoundValue::fromText(term));
			}

			// ...and the search words the part inherits from its category and that category's
			// ancestors (§2b). Resolved here rather than in SQL: the chain is a walk, SQL would
			// need a recursive CTE that has to be guarded against a cycle in `parent_type_id`,
			// and PartTypeRepository already has a walk that is guarded. There are eighteen types
			// in a stock database, so the list this produces is short.
			// One clause per matching type rather than a single `part_type_id IN (...)`, because a
			// part can untick individual inherited words: the type only lends its match to a part
			// that still keeps at least one of the words that matched. `excluded_keywords` is
			// compared whole, newline-fenced on both sides, since the boxes were drawn from this
			// very list and there is an exact word behind every tick.
			for (const TypeKeywordHit& hit : typesMatchingKeyword(db, term))
			{
				sql += " OR (part_type_id=? AND (";
				binds.push_back(BoundValue::fromNumber(hit.typeId));
				for (size_t i = 0; i < hit.words.size(); ++i)
				{
					sql += (i == 0 ? "" : " OR ");
					sql += "instr(char(10)||lower(ifnull(excluded_keywords,''))||char(10),?)=0";
					binds.push_back(BoundValue::fromText("\n" + toLower(hit.words[i]) + "\n"));
				}
				sql += "))";
			}
			sql += ")";
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

		// One EXISTS per group, so the groups AND. Inside a group the names are an IN list, which
		// is the OR: a part carrying any one of them satisfies that group.
		for (const std::vector<std::string>& group : query.tagGroups)
		{
			sql += " AND EXISTS (SELECT 1 FROM part_tag pt JOIN tag t ON t.id=pt.tag_id"
				" WHERE pt.part_id=part.id AND lower(t.name) IN (";
			for (size_t i = 0; i < group.size(); ++i)
			{
				sql += (i == 0 ? "?" : ",?");
				binds.push_back(BoundValue::fromText(group[i]));
			}
			sql += "))";
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
