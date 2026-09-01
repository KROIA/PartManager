// @file PartManager_PartRepository.h
// @brief CRUD for `part`/`part_file` (§2, §3), incl. writing base-SI `attr_*` fast-filter columns.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection, same style as PartTypeRepository. `insertPart()`/`updatePart()`
// look up the part's effective attributes (PartTypeRepository::effectiveAttributes())
// to find which are `searchable`, pull each one's `value` out of the
// `attributes` JSON, and write it into the matching `attr_<key>` column.
//
// ponytail: the value is trusted as already being in the attribute's declared
// base-SI unit ("100" in a Resistance field means 100 Ω) — no SI-prefix
// parsing happens here. The real entry parser (§2a: `1k`, `4k7`, `100uF`, ...)
// is a separate later module, `core/units`.
// @see docs/design/ARCHITECTURE.md §2, §2a, §3
// @see PartManager_Part.h, PartManager_PartFile.h, PartManager_PartTypeRepository.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_PartFile.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API PartRepository
	{
		PartRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates part/part_file if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// part CRUD. Insert/update also (re)write this type's searchable attr_* columns (see header note).
		// Returns the new id (0 on failure).
		static int insertPart(SQLiteWrapper::SQLite& db, const Part& part);
		static bool updatePart(SQLiteWrapper::SQLite& db, const Part& part);
		static bool deletePart(SQLiteWrapper::SQLite& db, int partId);
		// Looks up a single part by id. Returns false if not found.
		static bool findPart(SQLiteWrapper::SQLite& db, int partId, Part& outPart);
		// Lists parts, optionally restricted to one part_type_id (0 = all types).
		static std::vector<Part> listParts(SQLiteWrapper::SQLite& db, int partTypeId = 0);

		// part_file CRUD.
		static int insertFile(SQLiteWrapper::SQLite& db, const PartFile& file);
		static bool deleteFile(SQLiteWrapper::SQLite& db, int fileId);
		static std::vector<PartFile> listFiles(SQLiteWrapper::SQLite& db, int partId);
#endif

	};

}

