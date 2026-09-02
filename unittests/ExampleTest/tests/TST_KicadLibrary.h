#pragma once

#include "UnitTest.h"
#include "kicad/PartManager_KicadEditTracker.h"
#include "kicad/PartManager_KicadLibraryGenerator.h"
#include "kicad/PartManager_KicadSymbolWriter.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
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
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_KicadLibrary::handEditedSymbolsSurviveRegeneration);
#endif
	}

private:

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

		// KiCad resolves (extends "X") within the same file, so a base that is mapped to but not
		// embedded produces a library it reports as broken - and the mapping table and the
		// embedded list are in two different functions, which is exactly how they drift apart.
		const std::string library = PartManager::KicadSymbolWriter::library({});
		for (const char* typeName : { "Resistor", "Ceramic Capacitor", "Inductor", "Diode", "LED",
			"MOSFET", "Transistor", "Some Type Nobody Mapped" })
		{
			const std::string base = PartManager::KicadSymbolWriter::baseSymbolForType(typeName);
			TEST_ASSERT_M(library.find("(symbol \"" + base + "\"") != std::string::npos,
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
		TEST_ASSERT(block.find("(extends \"PM_R\")") != std::string::npos);
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

		for (const char* name : { "R-4K7", "R-10K" })
		{
			PartManager::Part part;
			part.partTypeId = typeId;
			part.name = name;
			part.mpn = name;
			PartManager::PartRepository::insertPart(db, part);
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

		const std::string libs = (folder / "kicad_libs").string();
		const std::string filestore = (folder / "filestore").string();

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

		// It keeps being reported until the user decides, rather than quietly becoming the new
		// baseline.
		PartManager::KicadGenerationResult fourth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fourth.symbolsPreserved, 1);

		// Force-regenerate discards the edit, which is the other half of the choice §5a gives.
		PartManager::KicadGenerationResult forced = PartManager::KicadLibraryGenerator::generate(
			db, libs, filestore, { "Resistors.kicad_sym:R-4K7" });
		TEST_COMPARE(forced.symbolsPreserved, 0);
		TEST_COMPARE(forced.symbolsGenerated, 2);
		TEST_ASSERT_M(readFile(library).find("HandEdited") == std::string::npos,
			"force-regenerate must actually discard the edit");
	}

#endif
};

TEST_INSTANTIATE(TST_KicadLibrary);
