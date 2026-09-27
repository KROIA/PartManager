// @file PartManager_Utf8Path.h
// @brief UTF-8 ⇄ std::filesystem::path. The only two conversions core/ is allowed to use.
//
// **`std::filesystem::path(someUtf8String)` is wrong on Windows.** The standard
// says a narrow string handed to `path` is in the *native narrow encoding*, and
// MSVC's is the active ANSI code page, not UTF-8. Every string in this project
// is UTF-8 — Qt hands over `QString::toStdString()`, SQLite stores and returns
// UTF-8 — so the plain constructor decodes those bytes as CP1252 and the file
// lands on disk under a different name than the one that was asked for.
//
// Measured, not assumed: a part named `1 µF 10 V X7S 0402` (UTF-8 `C2 B5` for
// the µ) produced `1 ÂµF 10 V X7S 0402.kicad_mod`, because CP1252 reads `C2` as
// `Â` and `B5` as `µ`. KiCad then cannot find the footprint the symbol names.
//
// `path::string()` is the same mistake in the other direction, and it is worse:
// the round trip happens to be lossless for anything CP1252 can spell, so the
// bug hides on a German machine and turns into `?` on a Cyrillic or CJK path.
// @see docs/design/ARCHITECTURE.md §5a
#pragma once

#include "PartManager_global.h"
#include <filesystem>
#include <string>

namespace PartManager
{

	// A path from UTF-8 bytes — use instead of `std::filesystem::path(s)`.
	PART_MANAGER_API std::filesystem::path utf8Path(const std::string& utf8);

	// The UTF-8 bytes of a path — use instead of `path.string()` for anything that is
	// stored, compared, or handed back to Qt.
	PART_MANAGER_API std::string pathToUtf8(const std::filesystem::path& path);

}
