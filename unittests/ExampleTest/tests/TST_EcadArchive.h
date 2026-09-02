#pragma once

#include "UnitTest.h"
#include "import/PartManager_EcadArchive.h"

#include <QByteArray>
#include <QString>
#include <private/qzipwriter_p.h>
#include <filesystem>
#include <vector>

// Picking the KiCad files out of a vendor ECAD download (§5c).
//
// The listing below is the real one from `LIB_74HC4051PW-Q100,11(5).zip` (Component Search
// Engine, 69 entries), trimmed of the tools that add nothing to the argument. It is the fixture
// because two of its properties would have been guessed wrong: the 3D model sits outside the
// KiCad folder, and `.lib` appears twice in the archive — once as KiCad 5 and once as CADSTAR.
class TST_EcadArchive : public UnitTest::Test
{
	TEST_CLASS(TST_EcadArchive)
public:
	TST_EcadArchive()
		: Test("TST_EcadArchive")
	{
		ADD_TEST(TST_EcadArchive::realVendorArchiveYieldsTheKicadFiles);
		ADD_TEST(TST_EcadArchive::legacyOnlyArchiveSaysSoInsteadOfLookingBroken);
		ADD_TEST(TST_EcadArchive::choiceIsStableWhenSeveralCandidatesExist);
		ADD_TEST(TST_EcadArchive::readsAnActualZip);
	}

private:

	static std::vector<std::string> realListing()
	{
		return {
			"74HC4051PW-Q100,11/74HC4051PW-Q100,11.epw",
			"74HC4051PW-Q100,11/part_info.txt",
			"74HC4051PW-Q100,11/How_To_Use_Models.pdf",
			"license.txt",
			"74HC4051PW-Q100,11/Altium/74HC4051PW-Q100,11.epw",
			"74HC4051PW-Q100,11/OrCAD_Allegro16/74HC4051PW-Q100,11.edf",
			"74HC4051PW-Q100,11/EAGLE/74HC4051PW-Q100_11.lbr",
			// The trap: a `.lib` that is not KiCad's.
			"74HC4051PW-Q100,11/CADSTAR/74HC4051PW-Q100_11.lib",
			"74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.lib",
			"74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.dcm",
			"74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.kicad_sym",
			"74HC4051PW-Q100,11/KiCad/SOP65P640X110-16N.kicad_mod",
			"74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.mod",
			"74HC4051PW-Q100,11/PADS/74HC4051PW-Q100_11.c",
			// The other trap: the model is not under KiCad/.
			"74HC4051PW-Q100,11/3D/74HC4051PW-Q100,11.stp",
			"version.bin",
		};
	}

	TEST_FUNCTION(realVendorArchiveYieldsTheKicadFiles)
	{
		TEST_START;

		const PartManager::EcadArchiveContents contents =
			PartManager::EcadArchive::classify(realListing());

		TEST_ASSERT(contents.ok);
		TEST_COMPARE(contents.symbolEntry,
			std::string("74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.kicad_sym"));
		TEST_COMPARE(contents.footprintEntry,
			std::string("74HC4051PW-Q100,11/KiCad/SOP65P640X110-16N.kicad_mod"));
		// Outside the KiCad folder, which is exactly why the model is not looked for there.
		TEST_COMPARE(contents.modelEntry,
			std::string("74HC4051PW-Q100,11/3D/74HC4051PW-Q100,11.stp"));
		TEST_ASSERT(contents.hasAnything());
		// The archive has modern files, so the legacy ones are simply not news.
		TEST_ASSERT_M(!contents.legacyKicadOnly,
			"a KiCad 5 file alongside a .kicad_sym must not be reported as legacy-only");
		TEST_ASSERT_M(contents.ignoredEntries > 0, "the other tools' files must be counted");
	}

	TEST_FUNCTION(legacyOnlyArchiveSaysSoInsteadOfLookingBroken)
	{
		TEST_START;

		// An older download: KiCad support present, but only in the format PartManager does not
		// write. Taking nothing is correct; taking nothing *silently* looks like a bug.
		const PartManager::EcadArchiveContents contents = PartManager::EcadArchive::classify({
			"PART/KiCad/PART.lib",
			"PART/KiCad/PART.dcm",
			"PART/KiCad/PART.mod",
			"PART/CADSTAR/PART.lib",
			"PART/EAGLE/PART.lbr",
		});

		TEST_ASSERT(contents.ok);
		TEST_ASSERT_M(contents.symbolEntry.empty(), "a KiCad 5 .lib is not a symbol we can use");
		TEST_ASSERT_M(contents.footprintEntry.empty(), "a KiCad 5 .mod is not a footprint we can use");
		TEST_ASSERT_M(contents.legacyKicadOnly, "the user has to be told the archive is KiCad 5 only");
		TEST_ASSERT_M(!contents.hasAnything(), "nothing to attach");
	}

	TEST_FUNCTION(choiceIsStableWhenSeveralCandidatesExist)
	{
		TEST_START;

		// Listing order must not decide, or re-downloading the same part could attach a
		// different footprint. Under KiCad/ wins over not; then the shallower path.
		const PartManager::EcadArchiveContents first = PartManager::EcadArchive::classify({
			"P/Other/deep/nested/A.kicad_mod",
			"P/KiCad/B.kicad_mod",
		});
		const PartManager::EcadArchiveContents reversed = PartManager::EcadArchive::classify({
			"P/KiCad/B.kicad_mod",
			"P/Other/deep/nested/A.kicad_mod",
		});
		TEST_COMPARE(first.footprintEntry, std::string("P/KiCad/B.kicad_mod"));
		TEST_COMPARE(reversed.footprintEntry, first.footprintEntry);

		// Directory entries and the macOS resource fork are not candidates.
		const PartManager::EcadArchiveContents noise = PartManager::EcadArchive::classify({
			"P/KiCad/",
			"__MACOSX/P/KiCad/._X.kicad_sym",
			"P/KiCad/X.kicad_sym",
		});
		TEST_COMPARE(noise.symbolEntry, std::string("P/KiCad/X.kicad_sym"));

		// A folder called kicad only counts as a folder — a file named "KiCad" does not.
		TEST_ASSERT(PartManager::EcadArchive::isUnderKicadFolder("a/KiCad/b.kicad_sym"));
		TEST_ASSERT(PartManager::EcadArchive::isUnderKicadFolder("a/kicad/b.kicad_sym"));
		TEST_ASSERT(!PartManager::EcadArchive::isUnderKicadFolder("a/KiCad"));
		TEST_ASSERT(!PartManager::EcadArchive::isUnderKicadFolder("a/KiCadish/b.lib"));
	}

	// The half classify() cannot cover: that the archive is really opened and the bytes come out.
	// The fixture is built here rather than shipped, so the test needs no binary in the repo.
	TEST_FUNCTION(readsAnActualZip)
	{
		TEST_START;

		const std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_EcadArchive";
		std::error_code ec;
		std::filesystem::remove_all(folder, ec);
		std::filesystem::create_directories(folder, ec);
		const std::filesystem::path zipPath = folder / "LIB_74HC4051PW-Q100,11(5).zip";

		const QByteArray symbol = "(kicad_symbol_lib (version 20241209)\n\t(symbol \"74HC4051PW\"\n\t)\n)\n";
		const QByteArray footprint = "(footprint \"SOP65P640X110-16N\"\n)\n";
		const QByteArray model = QByteArray("ISO-10303-21;\nHEADER;\n");
		{
			QZipWriter writer(QString::fromStdString(zipPath.string()));
			TEST_ASSERT_M(writer.isWritable(), "could not create the fixture archive");
			// Same shape as the real download, other tools included.
			writer.addFile(QStringLiteral("74HC4051PW-Q100,11/CADSTAR/74HC4051PW-Q100_11.lib"),
				QByteArray("not kicad"));
			writer.addFile(QStringLiteral("74HC4051PW-Q100,11/KiCad/74HC4051PW-Q100_11.kicad_sym"), symbol);
			writer.addFile(QStringLiteral("74HC4051PW-Q100,11/KiCad/SOP65P640X110-16N.kicad_mod"), footprint);
			writer.addFile(QStringLiteral("74HC4051PW-Q100,11/3D/74HC4051PW-Q100,11.stp"), model);
			writer.close();
		}

		const PartManager::EcadArchivePayload payload =
			PartManager::EcadArchive::read(zipPath.string());
		TEST_ASSERT_M(payload.contents.ok, "read failed: " + payload.contents.errorMessage);
		TEST_COMPARE(payload.symbolBytes, std::string(symbol.constData(), symbol.size()));
		TEST_COMPARE(payload.footprintBytes, std::string(footprint.constData(), footprint.size()));
		TEST_COMPARE(payload.modelBytes, std::string(model.constData(), model.size()));
		// The stored file keeps the vendor's own name, not the archive path.
		TEST_COMPARE(payload.symbolName, std::string("74HC4051PW-Q100_11.kicad_sym"));
		TEST_COMPARE(payload.footprintName, std::string("SOP65P640X110-16N.kicad_mod"));
		TEST_COMPARE(payload.modelName, std::string("74HC4051PW-Q100,11.stp"));

		// A file that is not an archive fails cleanly rather than producing an empty import.
		const std::filesystem::path notAZip = folder / "notes.txt";
		std::ofstream(notAZip) << "hello";
		const PartManager::EcadArchivePayload bad = PartManager::EcadArchive::read(notAZip.string());
		TEST_ASSERT_M(!bad.contents.hasAnything(), "a non-archive must yield nothing");
	}
};

TEST_INSTANTIATE(TST_EcadArchive);
