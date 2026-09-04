#pragma once

#include "UnitTest.h"
#include "kicad/PartManager_KicadEditTracker.h"
#include "kicad/PartManager_KicadLibraryGenerator.h"
#include "kicad/PartManager_KicadLibTable.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include "filestore/PartManager_FileStore.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "controllers/PartManager_KicadController.h"
#include <QDir>
#include <QFile>
#include <filesystem>
#include <fstream>
#include <sstream>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// §5a's library generation. The case the whole feature stands on is the last one: a symbol the
// user fixed by hand in KiCad must survive regeneration byte for byte. If that fails, generated
// libraries become disposable output and nobody can trust them with a manual correction.
class TST_KicadLibrary : public UnitTest::Test
{
	TEST_CLASS(TST_KicadLibrary)
public:
	TST_KicadLibrary()
		: Test("TST_KicadLibrary")
	{
		ADD_TEST(TST_KicadLibrary::splittingSurvivesBracketsInsideStrings);
		ADD_TEST(TST_KicadLibrary::everyExtendedBaseIsEmbedded);
		ADD_TEST(TST_KicadLibrary::libraryNamesAndTablesAreKicadSafe);
		ADD_TEST(TST_KicadLibrary::editStateComparesAgainstTheBaselineNotACandidate);
		ADD_TEST(TST_KicadLibrary::mergingIntoKicadsTableKeepsWhatIsAlreadyThere);
		ADD_TEST(TST_KicadLibrary::kicadsOwnSettingsFolderIsFound);
		ADD_TEST(TST_KicadLibrary::pinningKeepsWhatTheUserPinned);
		ADD_TEST(TST_KicadLibrary::padsBecomeTheDerivedSymbolsPins);
		ADD_TEST(TST_KicadLibrary::padNumbersSortInAnOrderThatSurvivesBgas);
		ADD_TEST(TST_KicadLibrary::aFootprintWithNoUsablePadsDerivesNothing);
		ADD_TEST(TST_KicadLibrary::aVendorFootprintDerivesOnePinPerPad);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_KicadLibrary::handEditedSymbolsSurviveRegeneration);
		ADD_TEST(TST_KicadLibrary::footprintReferenceUsesTheLibraryNickname);
		ADD_TEST(TST_KicadLibrary::partsWithoutKicadFilesAreSkippedNotInvented);
		ADD_TEST(TST_KicadLibrary::aChildCategoryInheritsRelevanceFromItsRoot);
		ADD_TEST(TST_KicadLibrary::aFootprintOnlyPartDerivesItsSymbolFromThePads);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// The part id behind a name, so a test can reach the row the generator wrote back into.
	static int partIdOf(SQLiteWrapper::SQLite& db, const std::string& name)
	{
		for (const PartManager::Part& part : PartManager::PartRepository::listParts(db))
		{
			if (part.name == name)
			{
				return part.id;
			}
		}
		return 0;
	}

	// A one-symbol library file as a vendor ships one. Attaching this is what makes a part
	// qualify for a symbol at all, now that nothing is generated from a template.
	static std::filesystem::path writeSymbolFile(const std::filesystem::path& folder,
		const std::string& symbolName)
	{
		const std::filesystem::path path = folder / (symbolName + ".kicad_sym");
		std::ofstream(path, std::ios::binary)
			<< "(kicad_symbol_lib\n\t(version 20241209)\n"
			<< "\t(symbol \"" << symbolName << "\"\n"
			<< "\t\t(property \"Reference\" \"R\"\n\t\t\t(at 0 0 0)\n\t\t)\n"
			<< "\t\t(symbol \"" << symbolName << "_0_1\"\n"
			<< "\t\t\t(rectangle\n\t\t\t\t(start -2.54 -1.016)\n\t\t\t\t(end 2.54 1.016)\n\t\t\t)\n"
			<< "\t\t)\n\t)\n)\n";
		return path;
	}

	static std::filesystem::path writeFootprintFile(const std::filesystem::path& folder,
		const std::string& name)
	{
		const std::filesystem::path path = folder / (name + ".kicad_mod");
		std::ofstream(path, std::ios::binary)
			<< "(footprint \"" << name << "\" (version 20240108) (layer \"F.Cu\"))\n";
		return path;
	}

	// The same, with pads — which is what makes a symbol derivable from it.
	static std::filesystem::path writePaddedFootprintFile(const std::filesystem::path& folder,
		const std::string& name, int padCount)
	{
		const std::filesystem::path path = folder / (name + ".kicad_mod");
		std::ofstream out(path, std::ios::binary);
		out << "(footprint \"" << name << "\" (version 20240108) (layer \"F.Cu\")\n";
		for (int pad = 1; pad <= padCount; ++pad)
		{
			out << "  (pad \"" << pad << "\" smd roundrect (at " << pad << " 0) (size 0.6 0.7)"
				<< " (layers \"F.Cu\" \"F.Paste\" \"F.Mask\"))\n";
		}
		out << ")\n";
		return path;
	}
#endif

	// How many times `needle` appears in `text` — a pin count read off the file the writer
	// produced, which is the only place the pin count is observable.
	static size_t countOf(const std::string& text, const std::string& needle)
	{
		size_t count = 0;
		for (size_t at = text.find(needle); at != std::string::npos;
			at = text.find(needle, at + needle.size()))
		{
			++count;
		}
		return count;
	}

	static std::string readFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream buffer;
		buffer << in.rdbuf();
		return buffer.str();
	}

	// Tests

	TEST_FUNCTION(splittingSurvivesBracketsInsideStrings)
	{
		TEST_START;

		// A naive paren count breaks on the first property value containing a bracket, and
		// KiCad's own libraries are full of them (footprint filters, descriptions). Splitting
		// wrongly means a symbol gets truncated and the rewritten library is corrupt.
		const std::string library =
			"(kicad_symbol_lib\n"
			"\t(version 20241209)\n"
			"\t(symbol \"A\"\n"
			"\t\t(property \"Description\" \"a resistor (SMD) with ) brackets\"\n\t\t)\n"
			"\t\t(symbol \"A_0_1\"\n\t\t\t(rectangle)\n\t\t)\n"
			"\t)\n"
			"\t(symbol \"B\"\n"
			"\t\t(property \"Value\" \"B\"\n\t\t)\n"
			"\t)\n"
			")\n";

		const std::vector<std::string> blocks = PartManager::KicadSymbolWriter::splitSymbols(library);
		// Two top-level symbols. The nested "A_0_1" body lives inside A and must not be reported
		// as a third.
		TEST_COMPARE(blocks.size(), static_cast<size_t>(2));
		TEST_COMPARE(PartManager::KicadSymbolWriter::symbolNameOf(blocks[0]), std::string("A"));
		TEST_COMPARE(PartManager::KicadSymbolWriter::symbolNameOf(blocks[1]), std::string("B"));
		TEST_ASSERT_M(blocks[0].find("A_0_1") != std::string::npos,
			"the nested body must stay inside its parent block");
		TEST_ASSERT_M(blocks[0].find("with ) brackets") != std::string::npos,
			"a bracket inside a string must not end the block early");

		// A truncated file stops rather than emitting a half block that would be written back
		// out as if it were valid.
		TEST_COMPARE(PartManager::KicadSymbolWriter::splitSymbols(
			"(kicad_symbol_lib (symbol \"A\" (property").size(), static_cast<size_t>(0));

		// An escaped quote inside a name must not truncate it.
		TEST_COMPARE(PartManager::KicadSymbolWriter::symbolNameOf("(symbol \"A\\\"B\" (x))"),
			std::string("A\"B"));
	}

	TEST_FUNCTION(everyExtendedBaseIsEmbedded)
	{
		TEST_START;

		// A library of freshly generated symbols embeds NO base symbols. KiCad lists every symbol
		// in a library in its chooser and has no way to hide one, so an embedded PM_R showed up
		// beside the real parts in every category - reported from KiCad on 2026-09-03.
		const std::string empty = PartManager::KicadSymbolWriter::library({});
		TEST_ASSERT_M(empty.find("(symbol \"PM_") == std::string::npos,
			"base symbols must not be embedded when nothing extends them: " + empty);

		// A symbol that still extends one - preserved from an older run, or hand-edited in KiCad -
		// keeps its parent, or the library is unreadable. Only the parent it names.
		const std::string legacy = PartManager::KicadSymbolWriter::library(
			{ "\t(symbol \"OLD\"\n\t\t(extends \"PM_R\")\n\t)\n" });
		TEST_ASSERT_M(legacy.find("(symbol \"PM_R\"") != std::string::npos,
			"a preserved (extends \"PM_R\") must keep its parent: " + legacy);
		TEST_ASSERT_M(legacy.find("(symbol \"PM_C\"") == std::string::npos,
			"and only the parent it names: " + legacy);

		// Every base is still generated, because that is what a legacy symbol resolves against
		// and what bodyForBase() stamps out. The mapping table and the embedded list are in two
		// different functions, which is exactly how they drift apart.
		const std::string allBases = PartManager::KicadSymbolWriter::library(
			{ "\t(symbol \"X\"\n\t\t(extends \"PM_R\")\n\t\t(extends \"PM_C\")\n"
			  "\t\t(extends \"PM_L\")\n\t\t(extends \"PM_D\")\n\t\t(extends \"PM_LED\")\n"
			  "\t\t(extends \"PM_Q\")\n\t\t(extends \"PM_Generic\")\n\t)\n" });
		for (const char* typeName : { "Resistor", "Ceramic Capacitor", "Inductor", "Diode", "LED",
			"MOSFET", "Transistor", "Some Type Nobody Mapped" })
		{
			const std::string base = PartManager::KicadSymbolWriter::baseSymbolForType(typeName);
			TEST_ASSERT_M(allBases.find("(symbol \"" + base + "\"") != std::string::npos,
				std::string(typeName) + " maps to base '" + base + "' which is not embedded");
		}

		// An unmapped type gets the generic box rather than a confident guess with a wrong pinout.
		TEST_COMPARE(PartManager::KicadSymbolWriter::baseSymbolForType("Some Type Nobody Mapped"),
			std::string(PartManager::KicadSymbolWriter::GenericBaseSymbol));
		TEST_COMPARE(PartManager::KicadSymbolWriter::referenceForBase("PM_R"), std::string("R"));
		TEST_COMPARE(PartManager::KicadSymbolWriter::referenceForBase("PM_LED"), std::string("D"));

		PartManager::KicadSymbolSpec spec;
		spec.name = "RC0603-4K7";
		spec.baseSymbol = "PM_R";
		spec.reference = "R";
		spec.value = "RC0603FR-074K7L";
		spec.partId = 42;
		spec.mouserPartNumber = "603-RC0603FR-074K7L";
		const std::string block = PartManager::KicadSymbolWriter::symbolBlock(spec);
		// The body is stamped in rather than inherited, so the symbol stands alone and its
		// parent does not have to exist beside it in the chooser.
		TEST_ASSERT_M(block.find("(extends") == std::string::npos, block);
		TEST_ASSERT_M(block.find("(symbol \"RC0603-4K7_0_1\"") != std::string::npos,
			"the unit body must be named after the part, or the symbol draws nothing: " + block);
		TEST_ASSERT_M(block.find("(pin passive") != std::string::npos
			|| block.find("(pin ") != std::string::npos, "a symbol with no pins is unusable: " + block);
		TEST_ASSERT(block.find("\"PM_PartID\" \"42\"") != std::string::npos);
		TEST_ASSERT(block.find("\"Mouser P/N\" \"603-RC0603FR-074K7L\"") != std::string::npos);
		// An empty property is noise in KiCad's field editor and is left out entirely.
		TEST_ASSERT_M(block.find("\"Description\" \"\"") == std::string::npos,
			"empty properties must not be written");

		// A quote in a part name would end the s-expression string early and corrupt the library.
		PartManager::KicadSymbolSpec quoted;
		quoted.name = "PART\"X";
		const std::string quotedBlock = PartManager::KicadSymbolWriter::symbolBlock(quoted);
		TEST_ASSERT(quotedBlock.find("(symbol \"PART\\\"X\"") != std::string::npos);
		// A colon separates library from symbol everywhere KiCad references one.
		TEST_COMPARE(PartManager::KicadSymbolWriter::sanitizeSymbolName("A:B"), std::string("A_B"));
		TEST_COMPARE(PartManager::KicadSymbolWriter::sanitizeSymbolName(""), std::string("Unnamed"));
	}

	TEST_FUNCTION(libraryNamesAndTablesAreKicadSafe)
	{
		TEST_START;

		// A nickname with a space makes the lib-table entry ambiguous; one with a colon splits in
		// two wherever it is referenced.
		TEST_COMPARE(PartManager::KicadLibraryGenerator::libraryNameFor("Power Regulators"),
			std::string("Power_Regulators"));
		TEST_COMPARE(PartManager::KicadLibraryGenerator::libraryNameFor("A:B"), std::string("A_B"));
		TEST_COMPARE(PartManager::KicadLibraryGenerator::libraryNameFor(""),
			std::string("PartManager"));

		const std::string table = PartManager::KicadLibraryGenerator::symLibTable(
			{ "Resistors", "Capacitors" }, "PM_LIBS");
		TEST_ASSERT(table.find("(sym_lib_table") == 0);
		// The env-var path is what makes the generated table portable between machines — an
		// absolute path would break the moment the database folder moved (§5a).
		TEST_ASSERT(table.find("${PM_LIBS}/symbols/Resistors.kicad_sym") != std::string::npos);
		TEST_ASSERT(table.find("${PM_LIBS}/symbols/Capacitors.kicad_sym") != std::string::npos);

		const std::string fpTable = PartManager::KicadLibraryGenerator::fpLibTable(
			{ "Resistors" }, "PM_LIBS");
		TEST_ASSERT(fpTable.find("${PM_LIBS}/footprints/Resistors.pretty") != std::string::npos);
	}

	TEST_FUNCTION(editStateComparesAgainstTheBaselineNotACandidate)
	{
		TEST_START;

		const std::string written = "(symbol \"A\" (property \"Value\" \"1k\"))";
		const std::string baseline = PartManager::KicadEditTracker::hashContent(written);

		// Never generated: write it.
		TEST_ASSERT(PartManager::KicadEditTracker::stateOf("", "", false)
			== PartManager::KicadItemState::New);
		// On disk is exactly what we wrote, even though a fresh generation would now produce
		// something different (the part's value changed). That is the ordinary case and must not
		// be mistaken for a hand edit.
		TEST_ASSERT(PartManager::KicadEditTracker::stateOf(baseline, written, true)
			== PartManager::KicadItemState::Unchanged);
		// Someone changed it in KiCad.
		TEST_ASSERT(PartManager::KicadEditTracker::stateOf(baseline,
			"(symbol \"A\" (property \"Value\" \"2k\"))", true)
			== PartManager::KicadItemState::EditedExternally);
		// We wrote it, it is gone. Writing it again is right; treating it as an edit would
		// freeze it out forever.
		TEST_ASSERT(PartManager::KicadEditTracker::stateOf(baseline, "", true)
			== PartManager::KicadItemState::Missing);
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	TEST_FUNCTION(handEditedSymbolsSurviveRegeneration)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_KicadLibrary";
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		SQLiteWrapper::SQLite db((folder / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::KicadEditTracker::createSchema(db);

		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		type.kicadRelevant = true;
		type.kicadCategory = "Resistors";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		const std::string libs = (folder / "kicad_libs").string();
		const std::string filestore = (folder / "filestore").string();
		PartManager::FileStore store(filestore);

		// Both parts carry their own `.kicad_sym`: nothing is generated from a template any more,
		// so an attachment is what makes a part appear in a library at all.
		for (const char* name : { "R-4K7", "R-10K" })
		{
			PartManager::Part part;
			part.partTypeId = typeId;
			part.name = name;
			part.mpn = name;
			const int partId = PartManager::PartRepository::insertPart(db, part);
			std::string error;
			TEST_ASSERT_M(store.attachFile(db, partId, PartManager::PartFileRole::KicadSymbol,
				writeSymbolFile(folder, name).string(), &error) != 0, error);
		}

		// A type that is KiCad-relevant but has no category: its parts go nowhere and must be
		// counted, not silently dropped.
		PartManager::PartType orphan;
		orphan.name = "Widget";
		orphan.domain = "generic";
		orphan.kicadRelevant = true;
		const int orphanId = PartManager::PartTypeRepository::insertType(db, orphan);
		PartManager::Part lonely;
		lonely.partTypeId = orphanId;
		lonely.name = "WIDGET-1";
		PartManager::PartRepository::insertPart(db, lonely);

		PartManager::KicadGenerationResult first =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT_M(first.ok, "first generation failed: " + first.errorMessage);
		TEST_COMPARE(first.symbolsGenerated, 2);
		TEST_COMPARE(first.librariesWritten, 1);
		TEST_COMPARE(first.symbolsPreserved, 0);
		TEST_COMPARE(first.skippedForNoCategory.size(), static_cast<size_t>(1));
		TEST_COMPARE(first.skippedForNoCategory[0], std::string("WIDGET-1"));

		const std::filesystem::path library =
			std::filesystem::path(libs) / "symbols" / "Resistors.kicad_sym";
		TEST_ASSERT_M(std::filesystem::exists(library), "the library was not written");
		TEST_ASSERT(std::filesystem::exists(std::filesystem::path(libs) / "partmanager-sym-lib-table"));

		// Regenerating with nothing changed must not report anything as edited — comparing
		// on-disk against a *fresh candidate* rather than the baseline would flag everything.
		PartManager::KicadGenerationResult second =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(second.symbolsPreserved, 0);
		TEST_COMPARE(second.symbolsGenerated, 2);

		// Now the user fixes a pin in KiCad. Simulated by rewriting one symbol's block.
		std::string text = readFile(library);
		const std::string marker = "\"R-4K7\"";
		const size_t at = text.find(marker);
		TEST_ASSERT_M(at != std::string::npos, "R-4K7 is not in the generated library");
		text.insert(text.find('\n', at) + 1, "\t\t(property \"HandEdited\" \"yes\" (at 0 0 0))\n");
		std::ofstream(library, std::ios::binary | std::ios::trunc) << text;

		PartManager::KicadGenerationResult third =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT(third.ok);
		TEST_COMPARE(third.symbolsPreserved, 1);
		TEST_COMPARE(third.symbolsGenerated, 1);
		TEST_COMPARE(third.preserved.size(), static_cast<size_t>(1));
		TEST_COMPARE(third.preserved[0].partName, std::string("R-4K7"));

		// The whole point: the edit is still there, and the untouched symbol was regenerated
		// beside it.
		const std::string after = readFile(library);
		TEST_ASSERT_M(after.find("HandEdited") != std::string::npos,
			"the hand edit was overwritten - generated libraries cannot be trusted");
		TEST_ASSERT(after.find("\"R-10K\"") != std::string::npos);

		// §5c: the edit did not merely survive — it was written into the part's own
		// `part_file(role='kicad_symbol')`, which is the copy that outlives kicad_libs/.
		TEST_COMPARE(third.symbolsSyncedBack, 1);
		PartManager::PartFile attached;
		TEST_ASSERT_M(PartManager::FileStore::roleFile(db, partIdOf(db, "R-4K7"),
			PartManager::PartFileRole::KicadSymbol, attached),
			"the KiCad edit was not written back into the part");
		const std::string storedSymbol =
			readFile(std::filesystem::path(filestore) / attached.relativePath);
		TEST_ASSERT_M(storedSymbol.find("HandEdited") != std::string::npos,
			"the part's own .kicad_sym does not carry the edit");
		// Stored as a whole one-symbol library, so KiCad opens it directly and the generator can
		// splice it straight back in.
		TEST_ASSERT(storedSymbol.find("kicad_symbol_lib") != std::string::npos);

		// Having been adopted, it stops being reported: it is now what PartManager itself would
		// write, so there is nothing left for the user to decide. The edit is still on disk.
		PartManager::KicadGenerationResult fourth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fourth.symbolsPreserved, 0);
		TEST_COMPARE(fourth.symbolsSyncedBack, 0);
		TEST_COMPARE(fourth.symbolsFromAttachment, 2);
		TEST_ASSERT_M(readFile(library).find("HandEdited") != std::string::npos,
			"the adopted edit must be regenerated from the attachment, not dropped");

		// And it is stable: a fifth run neither re-reports nor rewrites anything.
		PartManager::KicadGenerationResult fifth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fifth.symbolsPreserved, 0);
		TEST_COMPARE(fifth.symbolsSyncedBack, 0);
		TEST_ASSERT_M(readFile(library).find("HandEdited") != std::string::npos,
			"the adopted edit must not decay over repeated runs");

		// Removing the attachment leaves the part with no KiCad files at all, so it stops being
		// generated: no template stands in for it, and the symbol an earlier run left behind goes
		// with it rather than lingering as an entry no part points at.
		TEST_ASSERT(store.detachFile(db, attached.id));
		PartManager::KicadGenerationResult sixth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(sixth.symbolsGenerated, 1);
		TEST_COMPARE(sixth.staleItemsRemoved, 1);
		TEST_COMPARE(sixth.skippedForNoKicadFiles.size(), static_cast<size_t>(1));
		TEST_COMPARE(sixth.skippedForNoKicadFiles[0], std::string("R-4K7"));
		TEST_ASSERT_M(readFile(library).find("\"R-4K7\"") == std::string::npos,
			"a part that stopped qualifying must not keep its symbol in the library");
		TEST_ASSERT_M(readFile(library).find("\"R-10K\"") != std::string::npos,
			"and must not take the rest of the library with it");
	}

	// A symbol's `Footprint` property is resolved by KiCad through the fp-lib-table, where the
	// library is called `PartManager_Resistors` — the `Resistors.pretty` folder name never
	// reaches KiCad at all. Writing the folder name produces a reference that looks correct in
	// the symbol's properties and silently resolves to nothing, which is what shipped the first
	// time the nickname was prefixed.
	TEST_FUNCTION(footprintReferenceUsesTheLibraryNickname)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_KicadFootprintRef";
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		SQLiteWrapper::SQLite db((folder / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::KicadEditTracker::createSchema(db);

		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		type.kicadRelevant = true;
		type.kicadCategory = "Resistors";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = "R-4K7";
		part.mpn = "R-4K7";
		const int partId = PartManager::PartRepository::insertPart(db, part);

		// A footprint attachment is what makes the generator write the property at all, and a
		// symbol attachment is what makes there be a symbol to write it on.
		const std::string filestore = (folder / "filestore").string();
		PartManager::FileStore store(filestore);
		std::string error;
		TEST_ASSERT_M(store.attachFile(db, partId, PartManager::PartFileRole::KicadFootprint,
			writeFootprintFile(folder, "R_0603").string(), &error) != 0, error);
		TEST_ASSERT_M(store.attachFile(db, partId, PartManager::PartFileRole::KicadSymbol,
			writeSymbolFile(folder, "R-4K7").string(), &error) != 0, error);

		const std::string libs = (folder / "kicad_libs").string();
		const PartManager::KicadGenerationResult result =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT_M(result.ok, result.errorMessage);
		TEST_COMPARE(result.footprintsCopied, 1);

		const std::string library = readFile(
			std::filesystem::path(libs) / "symbols" / "Resistors.kicad_sym");
		TEST_ASSERT_M(library.find("\"PartManager_Resistors:R-4K7\"") != std::string::npos,
			"the Footprint property must name the library as KiCad knows it: " + library);
		// The bare folder name is exactly the reference KiCad cannot resolve.
		TEST_ASSERT_M(library.find("\"Resistors:R-4K7\"") == std::string::npos, library);

		db.close();
		std::filesystem::remove_all(folder);
	}

	// §5a: nothing is invented. A placeholder symbol is worse than an absent part — it places
	// silently in a schematic and is wrong on the board — so a part with no KiCad files attached
	// produces nothing, and the artifacts an earlier run left for it do not outlive it.
	TEST_FUNCTION(partsWithoutKicadFilesAreSkippedNotInvented)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_KicadSkip";
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		SQLiteWrapper::SQLite db((folder / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::KicadEditTracker::createSchema(db);

		PartManager::PartType type;
		type.name = "Resistor";
		type.domain = "electronic";
		type.kicadRelevant = true;
		type.kicadCategory = "Resistors";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		const std::string libs = (folder / "kicad_libs").string();
		const std::string filestore = (folder / "filestore").string();
		PartManager::FileStore store(filestore);
		std::string error;

		int ids[3] = { 0, 0, 0 };
		const char* names[3] = { "NEITHER", "SYM-ONLY", "FP-ONLY" };
		for (int i = 0; i < 3; ++i)
		{
			PartManager::Part part;
			part.partTypeId = typeId;
			part.name = names[i];
			ids[i] = PartManager::PartRepository::insertPart(db, part);
		}
		const int symbolFileId = store.attachFile(db, ids[1], PartManager::PartFileRole::KicadSymbol,
			writeSymbolFile(folder, "SYM-ONLY").string(), &error);
		TEST_ASSERT_M(symbolFileId != 0, error);
		const int footprintFileId = store.attachFile(db, ids[2],
			PartManager::PartFileRole::KicadFootprint,
			writeFootprintFile(folder, "FP-ONLY").string(), &error);
		TEST_ASSERT_M(footprintFileId != 0, error);

		const PartManager::KicadGenerationResult first =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT_M(first.ok, first.errorMessage);

		// Neither file: nothing at all, and named so the user knows it was deliberate.
		TEST_COMPARE(first.skippedForNoKicadFiles.size(), static_cast<size_t>(1));
		TEST_COMPARE(first.skippedForNoKicadFiles[0], std::string("NEITHER"));
		// A symbol without a footprint is normal and useful, so it is still generated.
		TEST_COMPARE(first.symbolsGenerated, 1);
		TEST_COMPARE(first.symbolsFromAttachment, 1);
		// A footprint without a symbol is also normal: the footprint is written and no symbol is
		// invented for it — but the missing symbol is reported, not swallowed.
		TEST_COMPARE(first.footprintsCopied, 1);
		TEST_COMPARE(first.skippedForNoSymbol.size(), static_cast<size_t>(1));
		TEST_COMPARE(first.skippedForNoSymbol[0], std::string("FP-ONLY"));

		const std::filesystem::path library =
			std::filesystem::path(libs) / "symbols" / "Resistors.kicad_sym";
		const std::string text = readFile(library);
		TEST_ASSERT_M(text.find("\"SYM-ONLY\"") != std::string::npos, text);
		TEST_ASSERT_M(text.find("NEITHER") == std::string::npos,
			"a part with no KiCad files must not get a placeholder symbol: " + text);
		TEST_ASSERT_M(text.find("\"FP-ONLY\"") == std::string::npos,
			"a footprint must not drag an invented symbol in with it: " + text);
		const std::filesystem::path pretty =
			std::filesystem::path(libs) / "footprints" / "Resistors.pretty";
		TEST_ASSERT(std::filesystem::exists(pretty / "FP-ONLY.kicad_mod"));
		TEST_ASSERT_M(!std::filesystem::exists(pretty / "SYM-ONLY.kicad_mod"),
			"no footprint was attached, so none may be written");

		// Two consecutive runs must agree, or every regeneration reports something new to act on.
		const PartManager::KicadGenerationResult second =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(second.symbolsGenerated, first.symbolsGenerated);
		TEST_COMPARE(second.footprintsCopied, first.footprintsCopied);
		TEST_COMPARE(second.staleItemsRemoved, 0);
		TEST_COMPARE(second.symbolsPreserved, 0);
		TEST_COMPARE(second.skippedForNoKicadFiles.size(), static_cast<size_t>(1));
		TEST_COMPARE(readFile(library), text);

		// A part that stops qualifying: the artifact goes and the tracking row with it, or the next
		// run compares a file nobody will ever regenerate against a hash and reports it forever.
		TEST_ASSERT(store.detachFile(db, symbolFileId));
		TEST_ASSERT(store.detachFile(db, footprintFileId));
		const PartManager::KicadGenerationResult third =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(third.symbolsGenerated, 0);
		TEST_COMPARE(third.footprintsCopied, 0);
		TEST_COMPARE(third.staleItemsRemoved, 2);
		TEST_COMPARE(third.skippedForNoKicadFiles.size(), static_cast<size_t>(3));
		TEST_ASSERT_M(!std::filesystem::exists(pretty / "FP-ONLY.kicad_mod"),
			"the footprint of a part that stopped qualifying must not be left on disk");
		TEST_ASSERT_M(!std::filesystem::exists(library),
			"a library that lost its last symbol must go, not sit there empty");
		TEST_COMPARE(PartManager::KicadEditTracker::allItems(db).size(), static_cast<size_t>(0));

		// And idempotent: the second removal run has nothing left to remove and says so.
		const PartManager::KicadGenerationResult fourth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fourth.staleItemsRemoved, 0);
		TEST_COMPARE(fourth.skippedForNoKicadFiles.size(), static_cast<size_t>(3));

		// A hand-edited artifact is never deleted, even when its part is gone: it is the user's own
		// work, so it stays on disk, stays tracked, and is surfaced as stale instead.
		TEST_ASSERT_M(store.attachFile(db, ids[1], PartManager::PartFileRole::KicadSymbol,
			writeSymbolFile(folder, "SYM-ONLY").string(), &error) != 0, error);
		TEST_ASSERT(PartManager::KicadLibraryGenerator::generate(db, libs, filestore).ok);
		std::string edited = readFile(library);
		const size_t at = edited.find("\"SYM-ONLY\"");
		TEST_ASSERT_M(at != std::string::npos, edited);
		edited.insert(edited.find('\n', at) + 1, "\t\t(property \"HandEdited\" \"yes\" (at 0 0 0))\n");
		std::ofstream(library, std::ios::binary | std::ios::trunc) << edited;

		PartManager::PartFile attached;
		TEST_ASSERT(PartManager::FileStore::roleFile(db, ids[1],
			PartManager::PartFileRole::KicadSymbol, attached));
		TEST_ASSERT(store.detachFile(db, attached.id));
		const PartManager::KicadGenerationResult fifth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fifth.staleItemsRemoved, 0);
		TEST_COMPARE(fifth.preserved.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(fifth.preserved[0].stale,
			"an orphaned hand edit has to be marked stale, or the dialog offers the wrong action");
		TEST_ASSERT_M(readFile(library).find("HandEdited") != std::string::npos,
			"a hand-edited symbol must never be deleted, even when nothing generates it any more");
		// Stable: it is reported again rather than quietly decaying, and force-regenerating is
		// what finally removes it.
		const PartManager::KicadGenerationResult sixth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(sixth.preserved.size(), static_cast<size_t>(1));
		const PartManager::KicadGenerationResult seventh =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore,
				{ "Resistors.kicad_sym:SYM-ONLY" });
		TEST_COMPARE(seventh.staleItemsRemoved, 1);
		TEST_COMPARE(seventh.preserved.size(), static_cast<size_t>(0));
		TEST_ASSERT_M(!std::filesystem::exists(library), "forcing must remove the orphan");

		db.close();
		std::filesystem::remove_all(folder);
	}

	// A child category (e.g. "Neopixel 5050 WS2812B" filed under "LED") must export without its
	// own kicad_relevant checkbox ticked: the tree is flattened per root category, so relevance and
	// category both inherit from the nearest flagged ancestor.
	TEST_FUNCTION(aChildCategoryInheritsRelevanceFromItsRoot)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_KicadChildCategory";
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		SQLiteWrapper::SQLite db((folder / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::KicadEditTracker::createSchema(db);

		PartManager::PartType root;
		root.name = "LED";
		root.domain = "electronic";
		root.kicadRelevant = true;
		root.kicadCategory = "LEDs";
		const int rootId = PartManager::PartTypeRepository::insertType(db, root);

		// The child never sets kicadRelevant or kicadCategory itself — both must come from the root.
		PartManager::PartType child;
		child.name = "Neopixel 5050 WS2812B";
		child.domain = "electronic";
		child.parentTypeId = rootId;
		const int childId = PartManager::PartTypeRepository::insertType(db, child);

		PartManager::Part part;
		part.partTypeId = childId;
		part.name = "WS2812B-5050";
		const int partId = PartManager::PartRepository::insertPart(db, part);

		const std::string filestore = (folder / "filestore").string();
		PartManager::FileStore store(filestore);
		std::string error;
		TEST_ASSERT_M(store.attachFile(db, partId, PartManager::PartFileRole::KicadSymbol,
			writeSymbolFile(folder, "WS2812B-5050").string(), &error) != 0, error);

		const std::string libs = (folder / "kicad_libs").string();
		const PartManager::KicadGenerationResult result =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT_M(result.ok, result.errorMessage);
		TEST_COMPARE(result.symbolsGenerated, 1);
		TEST_ASSERT_M(result.skippedForNoCategory.empty(),
			"the child must resolve the root's category, not be treated as uncategorized");

		const std::string library = readFile(
			std::filesystem::path(libs) / "symbols" / "LEDs.kicad_sym");
		TEST_ASSERT_M(library.find("\"WS2812B-5050\"") != std::string::npos,
			"a part filed under a child category must still land in its root's library: " + library);

		db.close();
		std::filesystem::remove_all(folder);
	}

	// M5: footprint-only parts. One whose pads carry numbers now gets a symbol derived from them —
	// its pins are real copper, so it places correctly — and one whose pads carry none still gets
	// nothing at all, and is still named in the result.
	TEST_FUNCTION(aFootprintOnlyPartDerivesItsSymbolFromThePads)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_KicadDerive";
		std::filesystem::remove_all(folder);
		std::filesystem::create_directories(folder);

		SQLiteWrapper::SQLite db((folder / "test.db").string());
		db.open();
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::KicadEditTracker::createSchema(db);

		PartManager::PartType type;
		type.name = "IC";
		type.domain = "electronic";
		type.kicadRelevant = true;
		type.kicadCategory = "ICs";
		const int typeId = PartManager::PartTypeRepository::insertType(db, type);

		PartManager::Part padded;
		padded.partTypeId = typeId;
		padded.name = "PADDED";
		const int paddedId = PartManager::PartRepository::insertPart(db, padded);
		PartManager::Part bare;
		bare.partTypeId = typeId;
		bare.name = "NO-PADS";
		const int bareId = PartManager::PartRepository::insertPart(db, bare);

		const std::string libs = (folder / "kicad_libs").string();
		const std::string filestore = (folder / "filestore").string();
		PartManager::FileStore store(filestore);
		std::string error;
		const int paddedFileId = store.attachFile(db, paddedId,
			PartManager::PartFileRole::KicadFootprint,
			writePaddedFootprintFile(folder, "PADDED", 6).string(), &error);
		TEST_ASSERT_M(paddedFileId != 0, error);
		TEST_ASSERT_M(store.attachFile(db, bareId, PartManager::PartFileRole::KicadFootprint,
			writeFootprintFile(folder, "NO-PADS").string(), &error) != 0, error);

		const PartManager::KicadGenerationResult first =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_ASSERT_M(first.ok, first.errorMessage);
		TEST_COMPARE(first.symbolsDerivedFromFootprint, 1);
		TEST_COMPARE(first.symbolsFromAttachment, 0);
		TEST_COMPARE(first.symbolsGenerated, 1);
		TEST_COMPARE(first.footprintsCopied, 2);
		// The one with nothing to derive from is reported, not swallowed.
		TEST_COMPARE(first.skippedForNoSymbol.size(), static_cast<size_t>(1));
		TEST_COMPARE(first.skippedForNoSymbol[0], std::string("NO-PADS"));

		const std::filesystem::path library =
			std::filesystem::path(libs) / "symbols" / "ICs.kicad_sym";
		const std::string text = readFile(library);
		TEST_ASSERT_M(text.find("\"PADDED\"") != std::string::npos, text);
		TEST_ASSERT_M(text.find("\"NO-PADS\"") == std::string::npos,
			"a footprint with no numbered pads must not drag an invented symbol in with it: " + text);
		TEST_COMPARE(countOf(text, "(pin "), static_cast<size_t>(6));
		TEST_ASSERT_M(text.find("(number \"6\"") != std::string::npos, text);
		// The footprint the pins came from, spelled the way KiCad resolves it.
		TEST_ASSERT_M(text.find("\"PartManager_ICs:PADDED\"") != std::string::npos, text);
		TEST_ASSERT_M(text.find(PartManager::KicadSymbolWriter::DerivedMarker) != std::string::npos,
			"a derived symbol has to say so, or it reads as one somebody drew: " + text);

		// Byte-identical twice over, or §5a reads the second run's own output as a hand edit.
		const PartManager::KicadGenerationResult second =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(second.symbolsDerivedFromFootprint, 1);
		TEST_COMPARE(second.symbolsPreserved, 0);
		TEST_COMPARE(second.staleItemsRemoved, 0);
		TEST_COMPARE(readFile(library), text);

		// And it is swept like any other generated artifact when the part stops qualifying.
		TEST_ASSERT(store.detachFile(db, paddedFileId));
		const PartManager::KicadGenerationResult third =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(third.symbolsDerivedFromFootprint, 0);
		TEST_COMPARE(third.staleItemsRemoved, 2);
		TEST_ASSERT_M(readFile(library).find("\"PADDED\"") == std::string::npos,
			"a derived symbol whose footprint was detached must go with it");

		db.close();
		std::filesystem::remove_all(folder);
	}

#endif

	// §5a: a part with a footprint and no symbol gets one derived from its pads. The pins are the
	// footprint's own pad numbers, which is the whole difference from the placeholder this
	// replaced — that one invented a pin count and was wrong on the board.
	TEST_FUNCTION(padsBecomeTheDerivedSymbolsPins)
	{
		TEST_START;
		using Writer = PartManager::KicadSymbolWriter;

		// Deliberately out of order, with a pad number that appears twice (a split ground pad is
		// one pin, not two) and a mounting hole that carries no number at all.
		const std::string footprint =
			"(footprint \"SOT-23\" (version 20240108) (layer \"F.Cu\")\n"
			"  (pad \"1\" smd roundrect (at -0.95 -1) (size 0.6 0.7) (layers \"F.Cu\"))\n"
			"  (pad \"3\" smd roundrect (at 0 1) (size 0.6 0.7) (layers \"F.Cu\")\n"
			"    (pinfunction \"GATE\") (pintype \"input\"))\n"
			"  (pad \"2\" smd roundrect (at 0.95 -1) (size 0.6 0.7) (layers \"F.Cu\"))\n"
			"  (pad \"2\" smd roundrect (at 1.5 -1) (size 0.6 0.7) (layers \"F.Cu\"))\n"
			"  (pad \"\" np_thru_hole circle (at 0 3) (size 1 1) (drill 1) (layers \"*.Cu\"))\n"
			")\n";

		const std::vector<PartManager::KicadDerivedPin> pins = Writer::pinsFromFootprint(footprint);
		TEST_COMPARE(pins.size(), static_cast<size_t>(3));
		TEST_COMPARE(pins[0].number, std::string("1"));
		TEST_COMPARE(pins[1].number, std::string("2"));
		TEST_COMPARE(pins[2].number, std::string("3"));
		// A library footprint normally carries no names at all; this one does on a single pad.
		TEST_ASSERT_M(pins[0].name.empty(), "a pad with no (pinfunction ...) has no name to offer");
		TEST_COMPARE(pins[2].name, std::string("GATE"));
		TEST_COMPARE(pins[2].type, std::string("input"));

		PartManager::KicadSymbolSpec spec;
		spec.name = "SOT23-PART";
		spec.reference = "U";
		spec.footprint = "PartManager_ICs:SOT23-PART";
		const std::string block = Writer::derivedSymbolBlock(spec, pins);

		TEST_COMPARE(countOf(block, "(pin "), static_cast<size_t>(3));
		TEST_ASSERT_M(block.find("(name \"GATE\"") != std::string::npos, block);
		TEST_ASSERT_M(block.find("(pin input line") != std::string::npos, block);
		// No (pintype ...) on the pad, so the pin is passive: the type KiCad's ERC is quietest
		// about, and never guessed from the number or the name.
		TEST_ASSERT_M(countOf(block, "(pin passive line") == static_cast<size_t>(2), block);
		TEST_ASSERT_M(countOf(block, "(name \"~\"") == static_cast<size_t>(2), block);
		// The Footprint property is the footprint the pins came out of — not a guess.
		TEST_ASSERT_M(block.find("\"PartManager_ICs:SOT23-PART\"") != std::string::npos, block);
		// Marked, so nobody mistakes it for a symbol someone drew.
		TEST_ASSERT_M(block.find(PartManager::KicadSymbolWriter::DerivedMarker) != std::string::npos,
			block);
		TEST_ASSERT_M(block.find("PartManager-derived") != std::string::npos, block);
		// Pins down the left then up the right, every one on KiCad's 2.54 mm grid — a pin off the
		// grid cannot be wired to, which makes the symbol useless in exactly the quiet way this
		// change exists to avoid.
		TEST_ASSERT_M(block.find("(at -7.62 2.54 0)") != std::string::npos, block);
		TEST_ASSERT_M(block.find("(at -7.62 0.00 0)") != std::string::npos, block);
		TEST_ASSERT_M(block.find("(at 7.62 2.54 180)") != std::string::npos, block);
		// Two runs of the same input are the same bytes, or every regeneration would report the
		// symbol as hand-edited (§5a).
		TEST_COMPARE(Writer::derivedSymbolBlock(spec, Writer::pinsFromFootprint(footprint)), block);
	}

	// A mixed set of pad numbers still has one definite order. "10" must not sort before "2", and
	// a BGA's "A1"/"B12" must not break the rule that handles the numeric ones.
	TEST_FUNCTION(padNumbersSortInAnOrderThatSurvivesBgas)
	{
		TEST_START;

		std::string footprint = "(footprint \"MIXED\" (layer \"F.Cu\")\n";
		const char* numbers[7] = { "A10", "10", "MH", "A2", "2", "B1", "A1" };
		for (const char* number : numbers)
		{
			footprint += std::string("  (pad \"") + number
				+ "\" smd rect (at 0 0) (size 1 1) (layers \"F.Cu\"))\n";
		}
		footprint += ")\n";

		const std::vector<PartManager::KicadDerivedPin> pins =
			PartManager::KicadSymbolWriter::pinsFromFootprint(footprint);
		TEST_COMPARE(pins.size(), static_cast<size_t>(7));
		const char* expected[7] = { "2", "10", "A1", "A2", "A10", "B1", "MH" };
		for (size_t i = 0; i < pins.size(); ++i)
		{
			TEST_ASSERT_M(pins[i].number == expected[i],
				"pad " + std::to_string(i) + " sorted to " + pins[i].number + ", expected "
					+ expected[i]);
		}
	}

	// "Otherwise don't": a footprint that carries nothing usable derives no symbol at all. The
	// user asked for exactly this — a derived symbol is only honest because its pins are pads.
	TEST_FUNCTION(aFootprintWithNoUsablePadsDerivesNothing)
	{
		TEST_START;
		using Writer = PartManager::KicadSymbolWriter;

		// Two mounting holes and a paste island: copper, and not one pin between them.
		const std::string mechanical =
			"(footprint \"BRACKET\" (version 20240108) (layer \"F.Cu\")\n"
			"  (pad \"\" np_thru_hole circle (at -2 0) (size 3 3) (drill 3) (layers \"*.Cu\"))\n"
			"  (pad \"\" np_thru_hole circle (at 2 0) (size 3 3) (drill 3) (layers \"*.Cu\"))\n"
			"  (fp_line (start -3 -3) (end 3 -3) (layer \"F.SilkS\") (width 0.12))\n"
			")\n";

		PartManager::KicadSymbolSpec spec;
		spec.name = "BRACKET";
		TEST_ASSERT(Writer::pinsFromFootprint(mechanical).empty());
		TEST_ASSERT_M(Writer::derivedSymbolBlock(spec,
			Writer::pinsFromFootprint(mechanical)).empty(),
			"a footprint with no numbered pads must derive nothing, not a box with no pins");
		// And the same for a file that is not a footprint at all.
		TEST_ASSERT(Writer::pinsFromFootprint("not a footprint").empty());
	}

	// The real thing, not only invented input: a vendor footprint out of the user's own library.
	// Every one of them is the legacy `(module ...)` form with *unquoted* pad numbers, which is
	// the spelling an invented test file would never have produced.
	TEST_FUNCTION(aVendorFootprintDerivesOnePinPerPad)
	{
		TEST_START;

		// CAY16-221J8LF, an eight-resistor network — copied verbatim from
		// KicadFresh/filestore, trimmed to its header and its pads.
		std::string footprint =
			"(module \"CAY16221J8LF\" (layer F.Cu)\n"
			"  (descr \"CAY16-221J8LF-1\")\n"
			"  (tags \"Resistor Network\")\n"
			"  (attr smd)\n";
		const double xs[16] = { -1.750, -1.250, -0.750, -0.250, 0.250, 0.750, 1.250, 1.750,
			1.750, 1.250, 0.750, 0.250, -0.250, -0.750, -1.250, -1.750 };
		for (int pad = 1; pad <= 16; ++pad)
		{
			footprint += "  (pad " + std::to_string(pad) + " smd rect (at "
				+ std::to_string(xs[pad - 1]) + " " + (pad <= 8 ? "0.725" : "-0.725")
				+ " 0) (size 0.325 0.650) (layers F.Cu F.Paste F.Mask))\n";
		}
		footprint += ")\n";

		const std::vector<PartManager::KicadDerivedPin> pins =
			PartManager::KicadSymbolWriter::pinsFromFootprint(footprint);
		TEST_COMPARE(pins.size(), static_cast<size_t>(16));
		TEST_COMPARE(pins[0].number, std::string("1"));
		// The one that a lexical sort gets wrong.
		TEST_COMPARE(pins[9].number, std::string("10"));
		TEST_COMPARE(pins[15].number, std::string("16"));

		PartManager::KicadSymbolSpec spec;
		spec.name = "CAY16-221J8LF";
		const std::string block = PartManager::KicadSymbolWriter::derivedSymbolBlock(spec, pins);
		TEST_COMPARE(countOf(block, "(pin "), static_cast<size_t>(16));
		// Eight down the left, eight up the right: pin 1 top left, pin 16 top right, which is how
		// anyone reading a schematic expects a 16-pin part to be drawn.
		TEST_ASSERT_M(countOf(block, "(at -7.62 ") == static_cast<size_t>(8), block);
		TEST_ASSERT_M(countOf(block, "(at 7.62 ") == static_cast<size_t>(8), block);
		TEST_ASSERT_M(block.find("(at -7.62 10.16 0)") != std::string::npos, block);
		TEST_ASSERT_M(block.find("(at 7.62 10.16 180)") != std::string::npos, block);
	}

	// Installing into KiCad's own sym-lib-table. The file belongs to the user's whole KiCad
	// install, so the property that matters is not "our rows are there" but "nothing else moved".
	TEST_FUNCTION(mergingIntoKicadsTableKeepsWhatIsAlreadyThere)
	{
		TEST_START;
		using Table = PartManager::KicadLibTable;

		// A table as KiCad writes one, with a library of the user's own in it.
		const std::string existing =
			"(sym_lib_table\n"
			"  (version 7)\n"
			"  (lib (name \"MyParts\")(type \"KiCad\")(uri \"${KIPRJMOD}/MyParts.kicad_sym\")"
			"(options \"\")(descr \"my own\"))\n"
			")\n";

		const std::vector<PartManager::KicadLibEntry> entries =
			Table::symbolEntries({ "Resistors", "ICs" }, "PARTMANAGER_KICAD_LIBS");
		const std::string merged = Table::merge(existing, "sym_lib_table", entries);

		TEST_ASSERT_M(merged.find("(name \"MyParts\")") != std::string::npos,
			"the user's own library must survive: " + merged);
		TEST_ASSERT_M(merged.find("(descr \"my own\")") != std::string::npos,
			"and survive with its own description: " + merged);
		TEST_ASSERT_M(merged.find("${PARTMANAGER_KICAD_LIBS}/symbols/Resistors.kicad_sym")
			!= std::string::npos, merged);
		TEST_ASSERT_M(merged.find("${PARTMANAGER_KICAD_LIBS}/symbols/ICs.kicad_sym")
			!= std::string::npos, merged);

		// The nickname is prefixed and the path is not: KiCad's chooser is one flat alphabetical
		// list of every library on the machine, so an unprefixed "Resistors" is unfindable among
		// KiCad's own - but renaming the file would strand every edit-tracker baseline.
		TEST_ASSERT_M(merged.find("(name \"PartManager_Resistors\")") != std::string::npos, merged);
		TEST_COMPARE(Table::nicknameFor("Resistors"), std::string("PartManager_Resistors"));
		// Idempotent, so a name that already carries the prefix is not doubled.
		TEST_COMPARE(Table::nicknameFor("PartManager_Resistors"),
			std::string("PartManager_Resistors"));

		// Idempotent: installing twice must not accumulate rows, or KiCad reports duplicate
		// nicknames and refuses the table.
		TEST_COMPARE(Table::merge(merged, "sym_lib_table", entries), merged);

		// A library that no longer exists goes. A table pointing at a deleted library makes
		// KiCad complain on every launch, which reads as PartManager having broken something.
		const std::string fewer = Table::merge(merged, "sym_lib_table",
			Table::symbolEntries({ "Resistors" }, "PARTMANAGER_KICAD_LIBS"));
		TEST_ASSERT_M(fewer.find("ICs") == std::string::npos, fewer);
		TEST_ASSERT_M(fewer.find("MyParts") != std::string::npos, fewer);

		// A user library that happens to share a nickname is NOT ours and is not taken over —
		// the marker decides, not the name.
		const std::string clash =
			"(sym_lib_table\n  (version 7)\n"
			"  (lib (name \"PartManager_Resistors\")(type \"KiCad\")(uri \"/somewhere/mine.kicad_sym\")"
			"(options \"\")(descr \"mine\"))\n)\n";
		TEST_ASSERT_M(Table::merge(clash, "sym_lib_table", entries).find("/somewhere/mine.kicad_sym")
			!= std::string::npos, "a same-named library of the user's must not be replaced");

		// No table yet, and a file that is not one at all, both come back as a valid table
		// holding exactly our rows — a missing install and a wrong path are not failures the
		// user has to diagnose from inside KiCad.
		const std::string fresh = Table::merge("", "fp_lib_table",
			Table::footprintEntries({ "Resistors" }, "PARTMANAGER_KICAD_LIBS"));
		TEST_ASSERT_M(fresh.find("(fp_lib_table") == 0, fresh);
		TEST_ASSERT_M(fresh.find("/footprints/Resistors.pretty") != std::string::npos, fresh);
		TEST_ASSERT_M(Table::merge("hello", "fp_lib_table", {}).find("(fp_lib_table") == 0,
			"an unreadable file is rebuilt, not appended to");
	}

	// Where KiCad keeps its settings. Environment-dependent by nature, so it asserts what it can
	// prove anywhere and only checks the contents when a KiCad is actually installed.
	//
	// The trap this pins: QStandardPaths::GenericConfigLocation is AppData/**Local** on Windows
	// and KiCad writes to AppData/**Roaming**, so the obvious implementation finds nothing on the
	// one platform this ships on.
	TEST_FUNCTION(kicadsOwnSettingsFolderIsFound)
	{
		TEST_START;

		const QStringList dirs = PartManager::KicadController::kicadConfigDirs();
		for (const QString& dir : dirs)
		{
			TEST_ASSERT_M(QDir(dir).exists(), ("reported a folder that is not there: " + dir)
				.toStdString());
			const bool looksLikeKicad =
				QFile::exists(QDir(dir).absoluteFilePath("kicad_common.json"))
				|| QFile::exists(QDir(dir).absoluteFilePath("sym-lib-table"))
				|| QFile::exists(QDir(dir).absoluteFilePath("kicad_common"));
			TEST_ASSERT_M(looksLikeKicad, ("not a KiCad settings folder: " + dir).toStdString());
		}

		// On a machine with KiCad installed the answer must not be empty — an empty list there
		// means the install button silently falls back to "pick a folder yourself" forever.
		const QString appData = qEnvironmentVariable("APPDATA");
		if (!appData.isEmpty() && QDir(QDir(appData).absoluteFilePath("kicad")).exists())
		{
			TEST_ASSERT_M(!dirs.isEmpty(),
				"KiCad is installed here, so its settings folder must be found");
		}
		else
		{
			TEST_MESSAGE("no KiCad on this machine - only the shape of the answer was checked");
		}
	}

	// KiCad's "favourite" libraries (`session.pinned_symbol_libs` in kicad_common.json, measured
	// against a real KiCad 9 install). The list is shared with whatever the user pinned by hand,
	// so the interesting cases are all about not trampling those.
	TEST_FUNCTION(pinningKeepsWhatTheUserPinned)
	{
		TEST_START;
		using Controller = PartManager::KicadController;

		const QStringList ours{ "PartManager_Resistors", "PartManager_ICs" };

		// The user's own pins keep their place, ours are appended.
		const QStringList merged = Controller::mergePinned(QStringList{ "Device", "Connector" }, ours);
		TEST_COMPARE(merged.size(), 4);
		TEST_COMPARE(merged.at(0), QString("Device"));
		TEST_COMPARE(merged.at(1), QString("Connector"));
		TEST_ASSERT_M(merged.contains("PartManager_Resistors"), merged.join(",").toStdString());

		// Idempotent - installing twice must not pin the same library twice.
		TEST_COMPARE(Controller::mergePinned(merged, ours), merged);

		// A PartManager pin whose library is gone is dropped: it would sit at the top of the
		// chooser pointing at nothing.
		const QStringList fewer = Controller::mergePinned(merged, QStringList{ "PartManager_Resistors" });
		TEST_ASSERT_M(!fewer.contains("PartManager_ICs"), fewer.join(",").toStdString());
		TEST_ASSERT_M(fewer.contains("Device"), fewer.join(",").toStdString());

		// A stale pin of the *user's* is theirs, not ours to clean up.
		const QStringList untouched = Controller::mergePinned(QStringList{ "SomethingOld" }, ours);
		TEST_ASSERT_M(untouched.contains("SomethingOld"), untouched.join(",").toStdString());
	}

};

TEST_INSTANTIATE(TST_KicadLibrary);
