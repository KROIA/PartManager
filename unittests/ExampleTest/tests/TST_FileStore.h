#pragma once

#include "UnitTest.h"
#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>
#include <fstream>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// No network anywhere in here: FileStore::downloadFile() is exercised only through its
// empty-URL rejection, which returns before any request is made.
class TST_FileStore : public UnitTest::Test
{
	TEST_CLASS(TST_FileStore)
public:
	TST_FileStore()
		: Test("TST_FileStore")
	{
		ADD_TEST(TST_FileStore::importReadBackAndDedup);
		ADD_TEST(TST_FileStore::missingSourceFileFails);
		ADD_TEST(TST_FileStore::emptyUrlFailsWithoutNetwork);
		ADD_TEST(TST_FileStore::aBlockPageIsNotAFile);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_FileStore::attachingCopiesAndKeepsNoPathToTheOriginal);
		ADD_TEST(TST_FileStore::attachAndDetachKeepsRowsAndFilesInSync);
#endif
	}

private:

	static std::filesystem::path freshFolder(const char* name)
	{
		std::filesystem::path folder = std::filesystem::temp_directory_path() / name;
		std::error_code error;
		std::filesystem::remove_all(folder, error);
		std::filesystem::create_directories(folder, error);
		return folder;
	}

	static std::filesystem::path writeTempFile(const std::filesystem::path& folder,
		const std::string& name, const std::string& content)
	{
		std::filesystem::path path = folder / name;
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		stream.write(content.data(), static_cast<std::streamsize>(content.size()));
		stream.close();
		return path;
	}

	static std::string readFile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
	}

	// Tests
	TEST_FUNCTION(importReadBackAndDedup)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore");
		std::filesystem::path source = writeTempFile(work, "datasheet.pdf", "%PDF-1.4 fake datasheet bytes");
		PartManager::FileStore store((work / "filestore").string());

		PartManager::FileStoreResult stored = store.importFile(source.string());
		TEST_ASSERT_M(stored.ok, stored.errorMessage);
		TEST_COMPARE(stored.originalFilename, std::string("datasheet.pdf"));
		TEST_COMPARE(stored.mimeType, std::string("application/pdf"));
		TEST_COMPARE(stored.sizeBytes, 29);
		// filestore/<hash[0:2]>/<hash>.pdf (§1)
		TEST_COMPARE(stored.relativePath, stored.contentHash.substr(0, 2) + "/" + stored.contentHash + ".pdf");

		std::string absolute = store.absolutePath(stored.relativePath);
		TEST_ASSERT_M(!absolute.empty(), "stored file must be retrievable");
		TEST_COMPARE(readFile(absolute), readFile(source.string()));

		// Same content from a differently named second file: one file on disk, same relative path.
		std::filesystem::path duplicate = writeTempFile(work, "same-content.pdf", "%PDF-1.4 fake datasheet bytes");
		PartManager::FileStoreResult again = store.importFile(duplicate.string());
		TEST_ASSERT_M(again.ok, again.errorMessage);
		TEST_COMPARE(again.relativePath, stored.relativePath);

		size_t fileCount = 0;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(store.rootPath()))
		{
			if (entry.is_regular_file())
			{
				++fileCount;
			}
		}
		TEST_COMPARE(fileCount, static_cast<size_t>(1));

		// Different content must not land on the same path.
		std::filesystem::path other = writeTempFile(work, "other.pdf", "%PDF-1.4 a different datasheet");
		PartManager::FileStoreResult otherStored = store.importFile(other.string());
		TEST_ASSERT_M(otherStored.ok, otherStored.errorMessage);
		TEST_ASSERT_M(otherStored.relativePath != stored.relativePath, "different content must get its own path");
	}

	TEST_FUNCTION(missingSourceFileFails)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_missing");
		PartManager::FileStore store((work / "filestore").string());

		PartManager::FileStoreResult result = store.importFile((work / "does_not_exist.pdf").string());
		TEST_ASSERT_M(!result.ok, "importing a missing file must fail");
		TEST_ASSERT_M(!result.errorMessage.empty(), "a failure must carry a message");
		TEST_ASSERT_M(result.relativePath.empty(), "a failed import must not claim a path");
		TEST_ASSERT_M(store.absolutePath("de/deadbeefdeadbeef.pdf").empty(), "unknown paths resolve to nothing");
	}

	TEST_FUNCTION(emptyUrlFailsWithoutNetwork)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_url");
		PartManager::FileStore store((work / "filestore").string());

		// Mouser leaves DataSheetUrl empty for most real parts (§6) — that path must fail politely.
		PartManager::FileStoreResult result = store.downloadFile("");
		TEST_ASSERT_M(!result.ok, "an empty URL cannot produce a file");
		TEST_ASSERT_M(!result.errorMessage.empty(), "a failure must carry a message");
	}

	// The one that costs real data if it regresses: Mouser's CDN refuses automated downloads
	// with HTTP 200 + text/html + an "Access Denied" page, so status and length checks both pass
	// and the block page gets stored as the part's datasheet or photo.
	TEST_FUNCTION(aBlockPageIsNotAFile)
	{
		TEST_START;

		// The exact shape measured against www.mouser.com on 2026-09-02.
		const std::string accessDenied =
			"<!DOCTYPE html>\n <html lang=\"en\">\n <head><title>Access Denied</title></head>";
		TEST_ASSERT_M(PartManager::FileStore::looksLikeBlockPage("text/html", accessDenied),
			"the content type alone must be enough");
		TEST_ASSERT_M(PartManager::FileStore::looksLikeBlockPage("text/html; charset=utf-8", ""),
			"a charset parameter must not defeat the check");
		TEST_ASSERT_M(PartManager::FileStore::looksLikeBlockPage("TEXT/HTML", ""),
			"the content type is case-insensitive");
		// A CDN that mislabels its block page still gets caught, which is why the body is sniffed.
		TEST_ASSERT_M(PartManager::FileStore::looksLikeBlockPage("application/octet-stream", accessDenied),
			"a mislabelled block page must be sniffed out of the body");
		TEST_ASSERT_M(PartManager::FileStore::looksLikeBlockPage("", "\n\n  <HTML><body>nope</body>"),
			"leading whitespace and upper case must not hide it");

		// Real files must not trip it, or every download breaks instead of the blocked ones.
		TEST_ASSERT_M(!PartManager::FileStore::looksLikeBlockPage("application/pdf", "%PDF-1.4 ..."),
			"a real PDF is not a block page");
		TEST_ASSERT_M(!PartManager::FileStore::looksLikeBlockPage("image/jpeg", "\xFF\xD8\xFF\xE0 JFIF"),
			"a real JPEG is not a block page");
		TEST_ASSERT_M(!PartManager::FileStore::looksLikeBlockPage("", ""),
			"an empty response is handled as empty, not as HTML");
		// "text/plain" starts with "text/" but is not html — the prefix must be the whole type.
		TEST_ASSERT_M(!PartManager::FileStore::looksLikeBlockPage("text/plain", "solid cube"),
			"text/plain is a legitimate download");
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Attaching must COPY, and must leave nothing pointing at where the file came from. A store
	// that linked would break the moment a datasheet was attached from a USB stick, a Downloads
	// folder that gets cleared, or a path that changes when the database is moved to another
	// machine — and it would break silently, months later.
	TEST_FUNCTION(attachingCopiesAndKeepsNoPathToTheOriginal)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_copy");
		const std::filesystem::path source = work / "somewhere else" / "LM358.pdf";
		std::filesystem::create_directories(source.parent_path());
		const std::string contents = "%PDF-1.4 the only copy";
		std::ofstream(source, std::ios::binary) << contents;

		SQLiteWrapper::SQLite db((work / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::Part part;
		part.name = "LM358";
		part.id = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT(part.id != 0);

		PartManager::FileStore store((work / "filestore").string());
		std::string error;
		const int fileId = store.replaceRoleFile(db, part.id, PartManager::PartFileRole::Datasheet,
			source.string(), &error);
		TEST_ASSERT_M(fileId != 0, "attach failed: " + error);

		PartManager::PartFile row;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, part.id,
			PartManager::PartFileRole::Datasheet, row));

		// Nothing recorded may contain the source path or its folder.
		TEST_ASSERT_M(row.relativePath.find("somewhere else") == std::string::npos,
			"the stored path leaks where the file came from: " + row.relativePath);
		TEST_ASSERT_M(row.relativePath.find(':') == std::string::npos,
			"the stored path is absolute: " + row.relativePath);
		// Only the base name survives, and only as a display label.
		TEST_COMPARE(row.originalFilename, std::string("LM358.pdf"));

		// The proof: destroy the original and the whole folder it lived in.
		std::filesystem::remove_all(source.parent_path());
		TEST_ASSERT(!std::filesystem::exists(source));

		const std::string stored = store.absolutePath(row.relativePath);
		TEST_ASSERT_M(!stored.empty(), "the copy vanished with the original - it was a link");
		{
			// Scoped: Windows refuses to delete a file that is still open, so a stream left open
			// here would make the replace-removes-the-old-copy check below fail for a reason that
			// has nothing to do with the store.
			std::ifstream in(stored, std::ios::binary);
			std::ostringstream buffer;
			buffer << in.rdbuf();
			TEST_COMPARE(buffer.str(), contents);
		}

		// Replacing a slot swaps the row rather than adding one, and the old content goes when
		// nothing else references it — the rule §5c's generator relies on when it syncs back.
		const std::filesystem::path second = work / "revB.pdf";
		std::ofstream(second, std::ios::binary) << "%PDF-1.4 revision B";
		const int replaced = store.replaceRoleFile(db, part.id,
			PartManager::PartFileRole::Datasheet, second.string(), &error);
		TEST_ASSERT_M(replaced != 0 && replaced != fileId, "replacing must write its own row");
		TEST_COMPARE(PartManager::PartRepository::listFiles(db, part.id).size(),
			static_cast<size_t>(1));
		TEST_ASSERT_M(!std::filesystem::exists(stored), "the replaced copy must not stay behind");
	}

	TEST_FUNCTION(attachAndDetachKeepsRowsAndFilesInSync)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_db");
		std::filesystem::path source = writeTempFile(work, "shared.pdf", "shared datasheet content");

		SQLiteWrapper::SQLite db((work / "partmanager.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part partA;
		partA.partTypeId = typeId;
		partA.name = "Part A";
		int partAId = PartManager::PartRepository::insertPart(db, partA);
		PartManager::Part partB;
		partB.partTypeId = typeId;
		partB.name = "Part B";
		int partBId = PartManager::PartRepository::insertPart(db, partB);

		PartManager::FileStore store((work / "filestore").string());
		std::string error;
		int fileA = store.attachFile(db, partAId, PartManager::PartFileRole::Datasheet, source.string(), &error);
		TEST_ASSERT_M(fileA != 0, error);
		// Both parts attach the same PDF: two rows, one file.
		int fileB = store.attachFile(db, partBId, PartManager::PartFileRole::Datasheet, source.string(), &error);
		TEST_ASSERT_M(fileB != 0, error);

		std::vector<PartManager::PartFile> rowsA = PartManager::PartRepository::listFiles(db, partAId);
		TEST_COMPARE(rowsA.size(), static_cast<size_t>(1));
		TEST_COMPARE(rowsA.front().role, std::string("datasheet"));
		std::vector<PartManager::PartFile> rowsB = PartManager::PartRepository::listFiles(db, partBId);
		TEST_COMPARE(rowsB.size(), static_cast<size_t>(1));
		TEST_COMPARE(rowsB.front().relativePath, rowsA.front().relativePath);

		const std::string sharedPath = rowsA.front().relativePath;
		TEST_ASSERT_M(!store.absolutePath(sharedPath).empty(), "the shared file must exist");

		// First detach: row gone, file stays because the other part still references it.
		TEST_ASSERT_M(store.detachFile(db, fileA), "detachFile failed");
		TEST_COMPARE(PartManager::PartRepository::listFiles(db, partAId).size(), static_cast<size_t>(0));
		TEST_ASSERT_M(!store.absolutePath(sharedPath).empty(), "the file is still referenced and must survive");

		// Last detach: row and file both gone, nothing orphaned.
		TEST_ASSERT_M(store.detachFile(db, fileB), "detachFile failed");
		TEST_COMPARE(PartManager::PartRepository::listFiles(db, partBId).size(), static_cast<size_t>(0));
		TEST_ASSERT_M(store.absolutePath(sharedPath).empty(), "the last reference must remove the file");

		TEST_ASSERT_M(!store.detachFile(db, fileB), "detaching an unknown id must fail");
	}
#endif

};

TEST_INSTANTIATE(TST_FileStore);
