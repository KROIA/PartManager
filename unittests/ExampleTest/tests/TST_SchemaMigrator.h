#pragma once

#include "UnitTest.h"
#include "database/PartManager_SchemaMigrator.h"
#include "domain/PartManager_TypeIcon.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_SchemaMigrator : public UnitTest::Test
{
	TEST_CLASS(TST_SchemaMigrator)
public:
	TST_SchemaMigrator()
		: Test("TST_SchemaMigrator")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_SchemaMigrator::equalVersionOpensNormally);
		ADD_TEST(TST_SchemaMigrator::lowerVersionMigrates);
		ADD_TEST(TST_SchemaMigrator::higherVersionRefuses);
		ADD_TEST(TST_SchemaMigrator::migratingAnOldDatabaseFillsTheNewTables);
		ADD_TEST(TST_SchemaMigrator::migratingToV13AddsTheIconColumnsAndChangesNoIcon);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Tests
	TEST_FUNCTION(equalVersionOpensNormally)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, PartManager::CurrentSchemaVersion, "0.0.0", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::equal);
		TEST_ASSERT(error.empty());
	}

	// A migration has to leave the database in the state the current version expects, not merely
	// with the right tables. Tag categories (v8) are the case that caught this: creating the
	// tables and leaving them empty means an existing database shows no families at all, and the
	// whole feature is invisible on every database that predates it — which is all of them.
	TEST_FUNCTION(migratingAnOldDatabaseFillsTheNewTables)
	{
		TEST_START;

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "PartManager_TST_Migrator_v8.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();

		// A database as it looked before categories existed: the tag schema of v2, carrying the
		// six tags the old seed wrote.
		TEST_ASSERT(db.execute(
			"CREATE TABLE tag (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,"
			" color TEXT NOT NULL, sort_order INTEGER NOT NULL DEFAULT 0);"));
		for (const char* name : { "SMD", "THT", "Favourite", "Obsolete", "Do not use",
			"Needs datasheet" })
		{
			db.executeWithParams("INSERT INTO tag (name,color,sort_order) VALUES (?,?,0);",
				{ name, "#1E88E5" });
		}

		std::string error;
		const PartManager::SchemaCompatibility result =
			PartManager::SchemaMigrator::migrate(db, 7, "0.0.0", error);
		TEST_ASSERT(result == PartManager::SchemaCompatibility::migrated);
		TEST_ASSERT(error.empty());

		// The families exist...
		const std::vector<PartManager::TagCategory> categories =
			PartManager::TagRepository::listCategories(db);
		TEST_ASSERT_M(!categories.empty(),
			"migrating to v8 must create the tag families, not just the table");

		// ...and the tags that were already there joined them rather than being duplicated.
		int smdCount = 0;
		int loose = 0;
		for (const PartManager::Tag& tag : PartManager::TagRepository::listTags(db))
		{
			smdCount += (tag.name == "SMD") ? 1 : 0;
			loose += (tag.categoryId == PartManager::NoTagCategoryId) ? 1 : 0;
		}
		TEST_ASSERT_M(smdCount == 1, "the existing tag is adopted, not duplicated");
		TEST_ASSERT_M(loose == 1, "only Favourite stays outside a family");

		db.close();
		std::filesystem::remove(path);
	}

	// The other half of the same rule, on the step that deliberately writes *nothing*: v13 adds
	// `icon_glyph`/`icon_colour` and leaves every existing row unset, because unset is what
	// TypeIconStyle::resolve() derives from the name — which is exactly what these categories
	// already drew. This pins both halves: the columns are really there (CREATE TABLE IF NOT
	// EXISTS would not have added them), and not one category changed its appearance.
	TEST_FUNCTION(migratingToV13AddsTheIconColumnsAndChangesNoIcon)
	{
		TEST_START;

		const std::filesystem::path path =
			std::filesystem::temp_directory_path() / "PartManager_TST_Migrator_v12.db";
		std::filesystem::remove(path);
		SQLiteWrapper::SQLite db(path.string());
		db.open();

		// `part_type` exactly as v12 left it — the two icon columns are the whole difference.
		// `part` is here because ensureLateAddedColumns() also reaches for its keyword columns.
		TEST_ASSERT(db.execute(
			"CREATE TABLE part_type (id INTEGER PRIMARY KEY, name TEXT NOT NULL,"
			" domain TEXT NOT NULL, kicad_relevant INTEGER NOT NULL DEFAULT 0,"
			" kicad_category TEXT, parent_type_id INTEGER REFERENCES part_type(id),"
			" description TEXT, search_keywords TEXT, excluded_keywords TEXT,"
			" name_template TEXT);"));
		TEST_ASSERT(db.execute(
			"CREATE TABLE part (id INTEGER PRIMARY KEY, part_type_id INTEGER, name TEXT,"
			" search_keywords TEXT, excluded_keywords TEXT);"));
		// One category whose name classifies, one that falls through to Generic — the two sides
		// of forType(), so a fallback that broke for either would show up here.
		for (const char* name : { "Resistor", "Wibble Frobnicator" })
		{
			TEST_ASSERT(db.executeWithParams(
				"INSERT INTO part_type (name,domain,parent_type_id) VALUES (?,'electronic',0);",
				{ name }));
		}

		std::string error;
		const PartManager::SchemaCompatibility result =
			PartManager::SchemaMigrator::migrate(db, 12, "0.0.0", error);
		TEST_ASSERT(result == PartManager::SchemaCompatibility::migrated);
		TEST_ASSERT(error.empty());

		bool hasGlyph = false;
		bool hasColour = false;
		for (const std::vector<std::string>& row : db.fetchAll("PRAGMA table_info(part_type);"))
		{
			hasGlyph = hasGlyph || (row.size() > 1 && row[1] == "icon_glyph");
			hasColour = hasColour || (row.size() > 1 && row[1] == "icon_colour");
		}
		TEST_ASSERT_M(hasGlyph, "v13 must ALTER icon_glyph into an existing part_type");
		TEST_ASSERT_M(hasColour, "...and icon_colour with it");

		const std::vector<PartManager::PartType> types = PartManager::PartTypeRepository::listTypes(db);
		TEST_ASSERT_M(types.size() == 2, "the migrated table still reads back both categories");
		for (const PartManager::PartType& type : types)
		{
			TEST_ASSERT_M(type.iconGlyph.empty() && type.iconColour == 0u,
				"v13 writes nothing into the new columns: " + type.name);
			const PartManager::TypeIcon derived = PartManager::TypeIconStyle::forType(type.name);
			const PartManager::TypeIcon resolved =
				PartManager::TypeIconStyle::resolve(type.name, type.iconGlyph, type.iconColour);
			TEST_ASSERT_M(resolved.glyph == derived.glyph && resolved.colour == derived.colour,
				"a migrated category draws exactly what it drew before: " + type.name);
		}

		// ...and the database is usable at v13, not merely wider: a category written with an icon
		// comes back carrying it.
		PartManager::PartType fresh;
		fresh.name = "Varistor";
		fresh.domain = "electronic";
		fresh.iconGlyph = "Fuse";
		fresh.iconColour = 0xE15759;
		const int newId = PartManager::PartTypeRepository::insertType(db, fresh);
		TEST_ASSERT_M(newId != PartManager::NoParentType, "insert into the migrated table works");
		PartManager::PartType read;
		TEST_ASSERT(PartManager::PartTypeRepository::findType(db, newId, read));
		TEST_ASSERT_M(read.iconGlyph == "Fuse", "the new column round-trips");
		TEST_ASSERT_M(read.iconColour == 0xE15759u, "and so does the colour");

		db.close();
		std::filesystem::remove(path);
	}

	TEST_FUNCTION(lowerVersionMigrates)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, 0, "0.0.0", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::migrated);
	}

	TEST_FUNCTION(higherVersionRefuses)
	{
		TEST_START;

		SQLiteWrapper::SQLite db;
		std::string error;
		PartManager::SchemaCompatibility result = PartManager::SchemaMigrator::migrate(
			db, PartManager::CurrentSchemaVersion + 1, "9.9.9", error);

		TEST_ASSERT(result == PartManager::SchemaCompatibility::refused);
		TEST_ASSERT_M(!error.empty(), "refusal must set an error message");
	}
#endif

};

TEST_INSTANTIATE(TST_SchemaMigrator);
