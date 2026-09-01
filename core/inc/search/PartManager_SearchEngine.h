// @file PartManager_SearchEngine.h
// @brief Runs a parsed SearchQuery against `part` (§7a), incl. the `attr_*` fast-filter columns.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as the repositories in core/persistence. Everything the
// user typed is bound as a SQL parameter; the only piece that reaches the
// statement as text is the `attr_<key>` column name, which SQL cannot
// parameterise and which SearchQuery::parse() has already restricted to
// identifier characters.
//
// Term semantics (all ANDed, see PartManager_SearchQuery.h for the grammar):
//   - free text  -> case-insensitive substring over name/mpn/manufacturer/description
//   - key op val -> compared against `attr_<key>`; a part whose column is NULL or whose
//                   type has no such attribute never matches, including for `!=`
//   - tag:name   -> the part carries a tag of that name (case-insensitive, exact)
// `=` and `!=` use the §2a ±0.5% relative tolerance, because a REAL column that
// round-tripped through an SI prefix is rarely bit-identical to the typed number.
//
// A query that failed to parse, or that names an attribute no `attr_*` column
// exists for, yields an empty result rather than an error — the search box is a
// filter, and "nothing matches" is the honest answer to both.
// @see docs/design/ARCHITECTURE.md §2a, §2d, §7a
// @see PartManager_SearchQuery.h, PartManager_PartRepository.h, PartManager_TagRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "search/PartManager_SearchQuery.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API SearchEngine
	{
		SearchEngine() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Matching parts, ordered by id. partTypeId 0 = search across all types (§7a tree filter);
		// any other value scopes to that one type (§7a table filter). An empty query matches every
		// part in scope, exactly like an empty filter box.
		static std::vector<Part> search(SQLiteWrapper::SQLite& db, const SearchQuery& query, int partTypeId = 0);

		// Parses `queryText` and searches in one call. Malformed text -> empty result.
		static std::vector<Part> search(SQLiteWrapper::SQLite& db, const std::string& queryText, int partTypeId = 0);

		// Ids only — for the §7a per-category match counts, where the rows themselves are never shown.
		static std::vector<int> searchIds(SQLiteWrapper::SQLite& db, const SearchQuery& query, int partTypeId = 0);
#endif

	};

}
