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
		ADD_TEST(TST_FileStore::theBytesDecideTheExtension);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_FileStore::anAttachedFileIsNamedAfterItsPart);
		ADD_TEST(TST_FileStore::attachingCopiesAndKeepsNoPathToTheOriginal);
		ADD_TEST(TST_FileStore::attachAndDetachKeepsRowsAndFilesInSync);
		ADD_TEST(TST_FileStore::theSweepFindsOnlyWhatNothingPointsAt);
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

	// Mouser serves every product photo as WebP no matter what the URL's extension claims, so a
	// photo downloaded from ".../hd/WL-SMCW.JPG" is stored as a .jpg that nothing can open until
	// it is renamed by hand. The bytes decide the name.
	TEST_FUNCTION(theBytesDecideTheExtension)
	{
		TEST_START;

		// The exact first twelve bytes measured from www.mouser.ch on 2026-09-02.
		const std::string webp("RIFF\xfa\x5a\x00\x00WEBPVP8 ", 16);
		TEST_COMPARE(PartManager::FileStore::sniffExtension(webp), std::string(".webp"));
		TEST_COMPARE(
			PartManager::FileStore::correctedFilename("WL-SMCW.JPG", "image/webp", webp),
			std::string("WL-SMCW.webp"));

		// A RIFF container that is not WebP must not be claimed as one.
		TEST_COMPARE(PartManager::FileStore::sniffExtension(std::string("RIFF\x00\x00\x00\x00WAVEfmt ", 16)),
			std::string());

		TEST_COMPARE(PartManager::FileStore::sniffExtension("%PDF-1.4"), std::string(".pdf"));
		TEST_COMPARE(PartManager::FileStore::sniffExtension(std::string("\x89PNG\r\n\x1a\n", 8)),
			std::string(".png"));
		TEST_COMPARE(PartManager::FileStore::sniffExtension("\xFF\xD8\xFF\xE0 JFIF"), std::string(".jpg"));

		// A correct name is left exactly as it is — including .jpeg, which is .jpg under another
		// spelling and must not be churned into one.
		TEST_COMPARE(PartManager::FileStore::correctedFilename("photo.png", "image/png",
			std::string("\x89PNG\r\n\x1a\n", 8)), std::string("photo.png"));
		TEST_COMPARE(PartManager::FileStore::correctedFilename("photo.jpeg", "image/jpeg",
			"\xFF\xD8\xFF\xE0 JFIF"), std::string("photo.jpeg"));

		// A datasheet link that really does answer with a PDF keeps its name.
		TEST_COMPARE(PartManager::FileStore::correctedFilename("ds.pdf", "application/pdf", "%PDF-1.7"),
			std::string("ds.pdf"));

		// An unrecognised format must be left alone rather than guessed at: renaming a good file
		// into an unopenable one is worse than the wrong extension we were handed.
		TEST_COMPARE(PartManager::FileStore::correctedFilename("model.stp", "application/step",
			"ISO-10303-21;"), std::string("model.stp"));
		// A ZIP-based format the caller already named correctly must not become ".zip" — that
		// would lose what the file is.
		TEST_COMPARE(PartManager::FileStore::correctedFilename("lib.kicad_sym", "",
			std::string("PK\x03\x04zzzz", 8)), std::string("lib.kicad_sym"));
		// SVG has no signature, so the content type is what identifies it.
		TEST_COMPARE(PartManager::FileStore::correctedFilename("sym.png", "image/svg+xml", "<svg/>"),
			std::string("sym.svg"));
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Attaching must COPY, and must leave nothing pointing at where the file came from. A store
	// that linked would break the moment a datasheet was attached from a USB stick, a Downloads
	// folder that gets cleared, or a path that changes when the database is moved to another
	// machine — and it would break silently, months later.
	TEST_FUNCTION(anAttachedFileIsNamedAfterItsPart)
	{
		TEST_START;

		// The pure half first: the name is what a PDF viewer's title bar shows, so it has to be
		// the part, and it still has to be a legal filename after a name full of Ω and slashes.
		TEST_COMPARE(PartManager::FileStore::readableFileName("DMG1013T-7",
			PartManager::PartFileRole::Datasheet, "ac4e1b041f3eaa15", ".pdf"),
			std::string("DMG1013T-7_datasheet_ac4e1b04.pdf"));
		TEST_COMPARE(PartManager::FileStore::readableFileName("4.7 k\xCE\xA9 / 0603",
			PartManager::PartFileRole::Image, "0123456789abcdef", ".jpg"),
			std::string("4.7_k_0603_image_01234567.jpg"));
		// A name this filter strips to nothing still has to produce a file.
		TEST_ASSERT_M(PartManager::FileStore::readableFileName("///",
			PartManager::PartFileRole::Other, "0123456789abcdef", ".bin")
			== std::string("part_other_01234567.bin"), "an unnameable part must still get a name");

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_names");
		const std::filesystem::path source = writeTempFile(work, "0900766b81864d6b.pdf",
			"%PDF-1.4 as mouser serves it");

		SQLiteWrapper::SQLite db((work / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::Part part;
		part.name = "DMG1013T-7";
		part.id = PartManager::PartRepository::insertPart(db, part);
		TEST_ASSERT(part.id != 0);

		PartManager::FileStore store((work / "filestore").string());
		std::string error;
		TEST_ASSERT_M(store.replaceRoleFile(db, part.id, PartManager::PartFileRole::Datasheet,
			source.string(), &error) != 0, "attach failed: " + error);

		PartManager::PartFile row;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, part.id,
			PartManager::PartFileRole::Datasheet, row));
		TEST_ASSERT_M(row.relativePath.find("DMG1013T-7_datasheet_") != std::string::npos,
			"the stored file is still named after its hash: " + row.relativePath);
		// The rename has to move the file, not just the row — a path nothing is at is worse than
		// a cryptic one.
		TEST_ASSERT_M(!store.absolutePath(row.relativePath).empty(),
			"the row points at a file that is not there: " + row.relativePath);
		TEST_COMPARE(readFile(store.absolutePath(row.relativePath)),
			std::string("%PDF-1.4 as mouser serves it"));
	}

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
		// Readable names cost the cross-part dedup that content addressing used to give: each
		// part's copy is named after that part, so the same PDF on two parts is two files. A few
		// hundred kB against a filename a PDF viewer can show — deliberate, not a regression.
		TEST_ASSERT_M(rowsB.front().relativePath != rowsA.front().relativePath,
			"both parts got the same file, so one of them is named after the other");

		const std::string pathA = rowsA.front().relativePath;
		TEST_ASSERT_M(!store.absolutePath(pathA).empty(), "the stored file must exist");

		// Ref-counting is still what decides whether the bytes go, so it is still pinned — with
		// a second row pointed at the same path by hand, which is the shape a shared file has.
		PartManager::PartFile shared = rowsA.front();
		shared.id = 0;
		shared.partId = partBId;
		shared.role = PartManager::toString(PartManager::PartFileRole::Other);
		const int sharedId = PartManager::PartRepository::insertFile(db, shared);
		TEST_ASSERT(sharedId != 0);

		// First detach: row gone, file stays because the other row still references it.
		TEST_ASSERT_M(store.detachFile(db, fileA), "detachFile failed");
		TEST_COMPARE(PartManager::PartRepository::listFiles(db, partAId).size(), static_cast<size_t>(0));
		TEST_ASSERT_M(!store.absolutePath(pathA).empty(), "the file is still referenced and must survive");

		// Last detach: row and file both gone, nothing orphaned.
		TEST_ASSERT_M(store.detachFile(db, sharedId), "detachFile failed");
		TEST_ASSERT_M(store.absolutePath(pathA).empty(), "the last reference must remove the file");

		TEST_ASSERT_M(store.detachFile(db, fileB), "detachFile failed");
		TEST_COMPARE(PartManager::PartRepository::listFiles(db, partBId).size(), static_cast<size_t>(0));

		TEST_ASSERT_M(!store.detachFile(db, fileB), "detaching an unknown id must fail");
	}

	// §12a: deleting a part clears its rows but cannot touch its files, because persistence is
	// not allowed to depend on FileStore. The bytes therefore stay behind, and this is the sweep
	// that finds them. Getting it wrong the other way — reporting a *referenced* file — would
	// delete a live attachment, so both directions are pinned here.
	TEST_FUNCTION(theSweepFindsOnlyWhatNothingPointsAt)
	{
		TEST_START;

		std::filesystem::path work = freshFolder("PartManager_TST_FileStore_sweep");

		SQLiteWrapper::SQLite db((work / "partmanager.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);

		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = "Kept";
		const int partId = PartManager::PartRepository::insertPart(db, part);

		PartManager::FileStore store((work / "filestore").string());
		std::string error;
		const std::filesystem::path kept = writeTempFile(work, "kept.pdf", "a referenced datasheet");
		TEST_ASSERT_M(store.attachFile(db, partId, PartManager::PartFileRole::Datasheet,
			kept.string(), &error) != 0, error);

		// Imported but never given a row — exactly the state deletePart() leaves behind.
		const std::filesystem::path dropped = writeTempFile(work, "dropped.pdf", "an orphan");
		const PartManager::FileStoreResult orphan = store.importFile(dropped.string());
		TEST_ASSERT_M(orphan.ok, orphan.errorMessage);

		PartManager::FileStoreOrphans found = store.findOrphans(db);
		TEST_COMPARE(found.relativePaths.size(), static_cast<size_t>(1));
		TEST_COMPARE(found.relativePaths.front(), orphan.relativePath);
		TEST_ASSERT_M(found.totalBytes > 0, "an orphan's size must be reported, not left at zero");

		// The mesh cache is derived data no part_file row ever points at. Sweeping it by the
		// same rule would delete the whole cache, so it must be invisible here.
		const std::filesystem::path cache = work / "filestore" / "meshcache";
		std::error_code ignored;
		std::filesystem::create_directories(cache, ignored);
		writeTempFile(cache, "part-abc123.pmmesh", "cached mesh");
		writeTempFile(cache, "part-abc123.stl", "left over from the old naming");
		TEST_COMPARE(store.findOrphans(db).relativePaths.size(), static_cast<size_t>(1));

		// The legacy `.stl` entries go; the current `.pmmesh` one stays.
		TEST_COMPARE(store.countStaleMeshCache(), 1);
		TEST_COMPARE(store.removeStaleMeshCache(), 1);
		TEST_COMPARE(store.countStaleMeshCache(), 0);
		TEST_ASSERT_M(std::filesystem::exists(cache / "part-abc123.pmmesh"),
			"a current mesh-cache entry must not be swept");

		TEST_COMPARE(store.removeOrphans(db), 1);
		TEST_COMPARE(store.findOrphans(db).relativePaths.size(), static_cast<size_t>(0));
		// The referenced file is still there — the whole point of scanning before deleting.
		TEST_ASSERT_M(!store.absolutePath(
			PartManager::PartRepository::listFiles(db, partId).front().relativePath).empty(),
			"a file a part_file row points at must survive the sweep");
	}
#endif

};

TEST_INSTANTIATE(TST_FileStore);
