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
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_KicadLibrary::handEditedSymbolsSurviveRegeneration);
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
#endif

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
		TEST_COMPARE(fourth.symbolsFromAttachment, 1);
		TEST_ASSERT_M(readFile(library).find("HandEdited") != std::string::npos,
			"the adopted edit must be regenerated from the attachment, not dropped");

		// And it is stable: a fifth run neither re-reports nor rewrites anything.
		PartManager::KicadGenerationResult fifth =
			PartManager::KicadLibraryGenerator::generate(db, libs, filestore);
		TEST_COMPARE(fifth.symbolsPreserved, 0);
		TEST_COMPARE(fifth.symbolsSyncedBack, 0);
		TEST_ASSERT_M(readFile(library).find("HandEdited") != std::string::npos,
			"the adopted edit must not decay over repeated runs");

		// Deleting the wrong thing by hand is how a sync feature loses data, so: removing the
		// attachment is what puts the part back on the generic template. Nothing else does.
		PartManager::FileStore store(filestore);
		TEST_ASSERT(store.detachFile(db, attached.id));
		PartManager::KicadGenerationResult forced = PartManager::KicadLibraryGenerator::generate(
			db, libs, filestore, { "Resistors.kicad_sym:R-4K7" });
		TEST_COMPARE(forced.symbolsPreserved, 0);
		TEST_COMPARE(forced.symbolsGenerated, 2);
		TEST_COMPARE(forced.symbolsFromAttachment, 0);
		TEST_ASSERT_M(readFile(library).find("HandEdited") == std::string::npos,
			"with the attachment gone, force-regenerate must discard the edit");
	}

#endif

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
