// @file PartManager_PartFile.h
// @brief Plain data class for a `part_file` row (§3) — one file attached to a part.
//
// Zero Qt, zero SQL, zero business logic. SQLite holds this metadata only —
// the actual file content lives content-addressed under `filestore/`
// (relativePath is relative to DatabaseHandle::filestorePath()).
// @see docs/design/ARCHITECTURE.md §3
// @see PartManager_Part.h
// @see PartManager_PartFileRole.h
#pragma once

#include "PartManager_global.h"
#include "PartManager_PartFileRole.h"
#include <string>

namespace PartManager
{

	// One `part_file` row — a file attached to a part, metadata only.
	struct PART_MANAGER_API PartFile
	{
		int id = 0;                    // SQLite primary key; 0 = not yet inserted
		int partId = 0;                 // FK -> Part::id
		std::string role;              // TEXT column; use PartFileRole / toString() / partFileRoleFromString() (PartManager_PartFileRole.h), not raw literals
		std::string relativePath;      // under filestore/, content-addressed
		std::string contentHash;       // hash of the file content; identical files across parts share storage
		int sizeBytes = 0;              // file size in bytes
		std::string mimeType;          // e.g. "application/pdf"
		std::string originalFilename;  // name as it was when attached, for display/download
		std::string addedAt;           // ISO-8601, DB-assigned on insert
	};

}

