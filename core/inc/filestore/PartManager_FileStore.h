// @file PartManager_FileStore.h
// @brief Content-addressed file storage under a database folder's `filestore/` (§1, §3).
//
// Stores file *content* only; the metadata half lives in `part_file` rows
// (PartRepository::insertFile()/listFiles()/deleteFile()). Layout is exactly
// what §1 specifies: `filestore/<hash[0:2]>/<hash>.<ext>` — identical content
// hashes to the same name, so two parts attaching the same PDF share one file
// on disk and each still get their own `part_file` row.
//
// Deletion is therefore reference-counted, not a plain unlink: detachFile()
// removes the row and only removes the file once no other row points at it.
//
// The manual "attach a local file" path (importFile/attachFile) is the primary
// one — Mouser returns an empty DataSheetUrl for most real parts (§6), so
// downloadFile() is a bonus, not the main road. Everything except
// downloadFile() is plain C++/std::filesystem and works in the non-Qt build;
// downloadFile() needs QtCore/QtNetwork (never QtWidgets, §12a) and fails
// cleanly with a message when Qt is absent.
// @see docs/design/ARCHITECTURE.md §1, §3, §6, §12a
// @see PartManager_PartFile.h, PartManager_PartRepository.h, PartManager_DatabaseHandle.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_PartFile.h"
#include "domain/PartManager_PartFileRole.h"
#include <string>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	// Outcome of one import/download. On success the fields map 1:1 onto the
	// matching `part_file` columns, so a caller can hand it straight to insertFile().
	struct PART_MANAGER_API FileStoreResult
	{
		bool ok = false;
		std::string errorMessage;
		std::string relativePath;      // e.g. "3f/3fa1c2d4e5f60718.pdf", relative to the filestore root
		std::string contentHash;
		int sizeBytes = 0;
		std::string mimeType;
		std::string originalFilename;
	};

	// One HTTP GET's answer, held in memory. Not every download belongs in the store: the Mouser
	// search dialog shows a row thumbnail per result, and writing 25 throwaway pictures into a
	// content-addressed store for a search the user may abandon is a leak, not a cache.
	struct PART_MANAGER_API DownloadedBytes
	{
		bool ok = false;
		std::string errorMessage;
		std::string contentType;
		std::string bytes;
		int status = 0;
	};

	// Content-addressed store rooted at one database folder's filestore/ path.
	class PART_MANAGER_API FileStore
	{
	public:
		// `filestorePath` is DatabaseHandle::filestorePath(). The folder is created on first write.
		explicit FileStore(const std::string& filestorePath);

		// The filestore root this store was constructed with.
		const std::string& rootPath() const;

		// Content hash used for the stored file name, as lowercase hex.
		// ponytail: FNV-1a 64 rather than SHA-256 — no crypto dependency and no hand-rolled
		// SHA in core/. Ceiling: 64 bits is fine for dedup but not collision-proof, so every
		// store/reuse verifies the bytes and falls back to a `_<n>` suffix on a real collision;
		// swap in QCryptographicHash::Sha256 (Qt build) if the hash ever needs to be a trusted
		// integrity check rather than a name.
		static std::string hashBytes(const std::string& bytes);

		// Copies a file into the store. Deduplicates: importing identical content twice
		// yields the same relativePath and writes nothing the second time.
		// Fails (ok == false) if the source cannot be read.
		FileStoreResult importFile(const std::string& sourcePath);
		// Same, for content already in memory (used by downloadFile()).
		FileStoreResult importBytes(const std::string& bytes, const std::string& originalFilename);

		// Absolute path of a stored file. Empty string if it is not (or no longer) on disk.
		std::string absolutePath(const std::string& relativePath) const;

		// True when a response body is a web page rather than the file that was asked for.
		//
		// **A blocked download arrives as a successful one.** Mouser's CDN answers a request it
		// dislikes with HTTP 200, `Content-Type: text/html` and an "Access Denied" page. Status
		// and length checks therefore both pass, and without this the block page is stored under
		// the requested name: a `.pdf` that is HTML, a `.jpg` that will not decode. Worse than a
		// failure, because the part then looks complete.
		//
		// downloadFile() now shapes its request so Mouser answers properly (see there), so this
		// is the backstop for the next CDN rather than the everyday path.
		//
		// Content-Type first, then a sniff of the body, so a CDN that mislabels its block page as
		// octet-stream is caught too. Pure string work, so it is testable without a network.
		static bool looksLikeBlockPage(const std::string& contentType, const std::string& body);

		// The filename `bytes` should actually be stored under.
		//
		// **A URL's extension is not a promise.** Mouser serves every product photo as WebP —
		// `.../hd/WL-SMCW.JPG` answers `Content-Type: image/webp` with `RIFF....WEBP` bytes —
		// so storing it as the requested `.jpg` produces a file nothing will open until it is
		// renamed by hand. Same trap for a `.pdf` link that redirects to an HTML landing page.
		//
		// The bytes decide, because they are the only party that cannot be lying: a magic-number
		// sniff first, Content-Type only as a fallback for formats with no signature. An
		// unrecognised format leaves the name alone rather than guessing — being wrong here
		// renames a perfectly good file into an unopenable one.
		static std::string correctedFilename(const std::string& filename,
			const std::string& contentType, const std::string& bytes);

		// The file extension (with dot, lowercase) `bytes` look like, empty when unrecognised.
		static std::string sniffExtension(const std::string& bytes);

		// GETs `url` into memory. The transport half of downloadFile() on its own — same WinHTTP
		// -then-Qt cascade and the same browser-shaped headers, which is the only reason Mouser
		// answers at all (see the comment in the .cpp). No block-page check and no extension
		// correction: those belong to storing a file, and a caller that only wants pixels does its
		// own judging. Blocking, so a UI caller runs it on a worker thread.
		static DownloadedBytes downloadBytes(const std::string& url, int timeoutMs = 15000);

		// Downloads `url` into the store. `originalFilename` may be empty — the URL's last
		// path segment is used then. Requires the Qt build; fails cleanly otherwise.
		// A response that looksLikeBlockPage() fails rather than being stored, unless the URL
		// asked for a `.htm`/`.html` in the first place.
		FileStoreResult downloadFile(const std::string& url, const std::string& originalFilename = std::string());

		// Per-download timeout in milliseconds. Default 15000.
		void setTimeoutMs(int timeoutMs);
		int timeoutMs() const;

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Import + insert the matching `part_file` row in one step, so neither half can be
		// forgotten. Returns the new part_file id, 0 on failure (reason in outError if given).
		int attachFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
			const std::string& sourcePath, std::string* outError = nullptr);

		// Removes the `part_file` row and, only when no other row still references the same
		// relativePath, the stored file. Never leaves an orphan row or an orphan file.
		bool detachFile(SQLiteWrapper::SQLite& db, int fileId);

		// **Single-slot roles.** A part carries at most one datasheet, one KiCad symbol, one
		// footprint, one 3D model and one image. That rule lives here rather than in each caller,
		// because the part editor and §5a's library generator both write these slots and a second
		// row would make roleFile() a coin flip between two files.
		//
		// Newest wins if an older version ever left two behind. False when the part has none.
		static bool roleFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
			PartFile& outFile);

		// Import + insert + drop whatever occupied the slot, in that order — a failed import
		// leaves the old file in place rather than losing both. Returns the new part_file id,
		// 0 on failure with the reason in outError.
		int replaceRoleFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
			const std::string& sourcePath, std::string* outError = nullptr);
		// Same, for content already in memory — what a ZIP entry and a KiCad edit both arrive as.
		int replaceRoleFileBytes(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
			const std::string& bytes, const std::string& originalFilename,
			std::string* outError = nullptr);
		// Same, for content the store already holds — what downloadFile() hands back. Skips the
		// import, so the bytes are not carried around a second time just to be deduplicated.
		int adoptStoredFile(SQLiteWrapper::SQLite& db, int partId, PartFileRole role,
			const FileStoreResult& stored, std::string* outError = nullptr);
#endif

	private:
		std::string m_rootPath;
		int m_timeoutMs = 15000;
	};

}
