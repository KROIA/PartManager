#pragma once

#include "UnitTest.h"
#include "search/PartManager_SearchQuery.h"
#include "search/PartManager_SearchEngine.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include <cmath>
#include <filesystem>
#include <memory>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

class TST_SearchEngine : public UnitTest::Test
{
	TEST_CLASS(TST_SearchEngine)
public:
	TST_SearchEngine()
		: Test("TST_SearchEngine")
	{
		// Grammar tests need no database at all — that is the point of SearchQuery being pure.
		ADD_TEST(TST_SearchEngine::parseTermForms);
		ADD_TEST(TST_SearchEngine::parseMalformedQueriesFailCleanly);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_SearchEngine::numericComparisonsAgainstAttrColumns);
		ADD_TEST(TST_SearchEngine::freeTextTagsAndScope);
		ADD_TEST(TST_SearchEngine::malformedOrUnknownAttributeYieldsNothing);
#endif
	}

private:

	// Float compare with the same relative window the engine's `=` uses (§2a).
	static bool nearly(double a, double b)
	{
		const double tolerance = std::fabs(b) * 0.005;
		return std::fabs(a - b) <= (tolerance > 1e-15 ? tolerance : 1e-15);
	}

	// Tests — parsing (no database)
	TEST_FUNCTION(parseTermForms)
	{
		TEST_START;

		PartManager::SearchQuery query = PartManager::SearchQuery::parse("  ");
		TEST_ASSERT_M(query.ok, "whitespace must parse");
		TEST_ASSERT_M(query.isEmpty(), "whitespace must produce no terms");

		query = PartManager::SearchQuery::parse("YAGEO resistance>1k voltage<=25V tag:SMD");
		TEST_ASSERT(query.ok);
		TEST_COMPARE(query.textTerms.size(), static_cast<size_t>(1));
		TEST_COMPARE(query.textTerms[0], std::string("yageo"));   // lowercased for substring matching
		TEST_COMPARE(query.tagNames.size(), static_cast<size_t>(1));
		TEST_COMPARE(query.tagNames[0], std::string("smd"));
		TEST_COMPARE(query.attributeTerms.size(), static_cast<size_t>(2));

		TEST_COMPARE(query.attributeTerms[0].key, std::string("resistance"));
		TEST_ASSERT(query.attributeTerms[0].op == PartManager::SearchCompareOp::Greater);
		TEST_COMPARE(query.attributeTerms[0].value, 1000.0);      // via units/ValueParser
		TEST_ASSERT_M(query.attributeTerms[0].unit.empty(), "no unit was typed on '1k'");

		TEST_COMPARE(query.attributeTerms[1].key, std::string("voltage"));
		TEST_ASSERT(query.attributeTerms[1].op == PartManager::SearchCompareOp::LessEqual);
		TEST_COMPARE(query.attributeTerms[1].value, 25.0);
		TEST_COMPARE(query.attributeTerms[1].unit, std::string("V"));

		// §2a value forms all reach the same base-SI number, and ',' is a decimal point.
		TEST_ASSERT(nearly(PartManager::SearchQuery::parse("capacitance=100n").attributeTerms[0].value, 1e-7));
		TEST_ASSERT(nearly(PartManager::SearchQuery::parse("resistance=4k7").attributeTerms[0].value, 4700.0));
		TEST_ASSERT(nearly(PartManager::SearchQuery::parse("resistance=4,7k").attributeTerms[0].value, 4700.0));

		// Quotes keep one term together; the remaining operators and key casing.
		query = PartManager::SearchQuery::parse("\"power supply\" tag:\"Do not use\" Resistance!=0");
		TEST_ASSERT(query.ok);
		TEST_COMPARE(query.textTerms.size(), static_cast<size_t>(1));
		TEST_COMPARE(query.textTerms[0], std::string("power supply"));
		TEST_COMPARE(query.tagNames[0], std::string("do not use"));
		TEST_COMPARE(query.attributeTerms[0].key, std::string("resistance"));
		TEST_ASSERT(query.attributeTerms[0].op == PartManager::SearchCompareOp::NotEqual);

		query = PartManager::SearchQuery::parse("resistance<1M resistance>=100");
		TEST_ASSERT(query.attributeTerms[0].op == PartManager::SearchCompareOp::Less);
		TEST_COMPARE(query.attributeTerms[0].value, 1e6);
		TEST_ASSERT(query.attributeTerms[1].op == PartManager::SearchCompareOp::GreaterEqual);
	}

	TEST_FUNCTION(parseMalformedQueriesFailCleanly)
	{
		TEST_START;

		// Every one of these must come back as ok == false with a message, never throw.
		const std::vector<std::string> broken = {
			"resistance>",              // no value
			"resistance>abc",           // value the §2a parser cannot read
			">1k",                      // no key
			"resistance>1k2k",          // two SI prefixes
			"my resistance>1.2.3",      // two decimal points
			"tag:",                     // empty tag name
			"re$istance>1k",            // key that is not an identifier -> never reaches a column name
		};
		for (const std::string& text : broken)
		{
			PartManager::SearchQuery query = PartManager::SearchQuery::parse(text);
			TEST_ASSERT_M(!query.ok, "'" + text + "' must not parse");
			TEST_ASSERT_M(!query.error.empty(), "'" + text + "' must report a reason");
			TEST_ASSERT_M(query.isEmpty(), "'" + text + "' must not leave half-parsed terms behind");
		}
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Two resistors and one capacitor, each with its searchable attr_* column filled, plus tags.
	struct Fixture
	{
		std::unique_ptr<SQLiteWrapper::SQLite> db;
		int resistorTypeId = 0;
		int capacitorTypeId = 0;
		int r10kId = 0;
		int r220Id = 0;
		int c100nId = 0;
		int smdTagId = 0;
	};

	static void addAttribute(SQLiteWrapper::SQLite& db, int typeId, const std::string& key,
		const std::string& unit)
	{
		PartManager::PartTypeAttribute attribute;
		attribute.partTypeId = typeId;
		attribute.key = key;
		attribute.label = key;
		attribute.unit = unit;
		attribute.datatype = PartManager::AttributeDataType::Dimension;
		attribute.searchable = true;
		PartManager::PartTypeRepository::insertAttribute(db, attribute);
	}

	static Fixture makeFixture(const std::string& name)
	{
		Fixture fixture;
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		fixture.db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		fixture.db->open();
		SQLiteWrapper::SQLite& db = *fixture.db;
		PartManager::PartTypeRepository::createSchema(db);
		PartManager::PartRepository::createSchema(db);
		PartManager::TagRepository::createSchema(db);

		PartManager::PartType resistor;
		resistor.name = "Resistor";
		resistor.domain = "electronic";
		fixture.resistorTypeId = PartManager::PartTypeRepository::insertType(db, resistor);
		addAttribute(db, fixture.resistorTypeId, "resistance", "\xCE\xA9");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		fixture.capacitorTypeId = PartManager::PartTypeRepository::insertType(db, capacitor);
		addAttribute(db, fixture.capacitorTypeId, "capacitance", "F");
		addAttribute(db, fixture.capacitorTypeId, "voltage", "V");

		PartManager::Part r10k;
		r10k.partTypeId = fixture.resistorTypeId;
		r10k.name = "10k 0603";
		r10k.manufacturer = "Yageo";
		r10k.mpn = "RC0603FR-0710KL";
		r10k.attributes = "{\"resistance\":{\"value\":10000,\"unit\":\"\xCE\xA9\"}}";
		fixture.r10kId = PartManager::PartRepository::insertPart(db, r10k);

		PartManager::Part r220;
		r220.partTypeId = fixture.resistorTypeId;
		r220.name = "220R 0805";
		r220.manufacturer = "Vishay";
		r220.description = "LED series resistor";
		r220.attributes = "{\"resistance\":{\"value\":220,\"unit\":\"\xCE\xA9\"}}";
		fixture.r220Id = PartManager::PartRepository::insertPart(db, r220);

		PartManager::Part c100n;
		c100n.partTypeId = fixture.capacitorTypeId;
		c100n.name = "100n X7R";
		c100n.manufacturer = "Murata";
		c100n.attributes = "{\"capacitance\":{\"value\":1e-07,\"unit\":\"F\"},"
			"\"voltage\":{\"value\":50,\"unit\":\"V\"}}";
		fixture.c100nId = PartManager::PartRepository::insertPart(db, c100n);

		PartManager::Tag smd;
		smd.name = "SMD";
		smd.color = "#1E88E5";
		fixture.smdTagId = PartManager::TagRepository::insertTag(db, smd);
		PartManager::TagRepository::addPartTag(db, fixture.r10kId, fixture.smdTagId);
		PartManager::TagRepository::addPartTag(db, fixture.c100nId, fixture.smdTagId);
		return fixture;
	}

	static bool containsPart(const std::vector<PartManager::Part>& parts, int partId)
	{
		for (const PartManager::Part& part : parts)
		{
			if (part.id == partId)
			{
				return true;
			}
		}
		return false;
	}

	// Tests — against a real database
	TEST_FUNCTION(numericComparisonsAgainstAttrColumns)
	{
		TEST_START;

		Fixture fixture = makeFixture("PartManager_TST_SearchEngine_numeric.db");
		SQLiteWrapper::SQLite& db = *fixture.db;

		std::vector<PartManager::Part> hits = PartManager::SearchEngine::search(db, "resistance>1k");
		TEST_COMPARE(hits.size(), static_cast<size_t>(1));
		TEST_COMPARE(hits[0].id, fixture.r10kId);

		hits = PartManager::SearchEngine::search(db, "resistance<1k");
		TEST_COMPARE(hits.size(), static_cast<size_t>(1));
		TEST_COMPARE(hits[0].id, fixture.r220Id);

		// Equality goes through the §2a tolerance, and "10k" is the same number as "10000".
		TEST_COMPARE(PartManager::SearchEngine::search(db, "resistance=10k").size(), static_cast<size_t>(1));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "resistance=10000").size(), static_cast<size_t>(1));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "resistance!=10k").size(), static_cast<size_t>(1));

		// A sub-micro value must survive the round trip into attr_capacitance and back out again.
		hits = PartManager::SearchEngine::search(db, "capacitance=100n");
		TEST_COMPARE(hits.size(), static_cast<size_t>(1));
		TEST_COMPARE(hits[0].id, fixture.c100nId);
		TEST_COMPARE(PartManager::SearchEngine::search(db, "capacitance>1u").size(), static_cast<size_t>(0));

		// Terms AND together, and a part without the column never matches an attribute term.
		TEST_COMPARE(PartManager::SearchEngine::search(db, "capacitance=100n voltage<=25V").size(),
			static_cast<size_t>(0));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "capacitance=100n voltage>=25V").size(),
			static_cast<size_t>(1));
		TEST_ASSERT_M(!containsPart(PartManager::SearchEngine::search(db, "voltage>=0"), fixture.r10kId),
			"a resistor has no voltage attribute, so it must not match a voltage term");
	}

	TEST_FUNCTION(freeTextTagsAndScope)
	{
		TEST_START;

		Fixture fixture = makeFixture("PartManager_TST_SearchEngine_text.db");
		SQLiteWrapper::SQLite& db = *fixture.db;

		// Empty query = unfiltered list, scoped or not.
		TEST_COMPARE(PartManager::SearchEngine::search(db, "").size(), static_cast<size_t>(3));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "", fixture.resistorTypeId).size(),
			static_cast<size_t>(2));

		// Free text is case-insensitive and covers name/mpn/manufacturer/description.
		TEST_COMPARE(PartManager::SearchEngine::search(db, "yageo")[0].id, fixture.r10kId);
		TEST_COMPARE(PartManager::SearchEngine::search(db, "RC0603")[0].id, fixture.r10kId);
		TEST_COMPARE(PartManager::SearchEngine::search(db, "series resistor")[0].id, fixture.r220Id);
		TEST_COMPARE(PartManager::SearchEngine::search(db, "X7R")[0].id, fixture.c100nId);
		TEST_COMPARE(PartManager::SearchEngine::search(db, "nonexistent").size(), static_cast<size_t>(0));

		// tag: filters across types, and ANDs with the rest.
		std::vector<PartManager::Part> tagged = PartManager::SearchEngine::search(db, "tag:smd");
		TEST_COMPARE(tagged.size(), static_cast<size_t>(2));
		TEST_ASSERT(containsPart(tagged, fixture.r10kId) && containsPart(tagged, fixture.c100nId));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "tag:SMD resistance>1k").size(),
			static_cast<size_t>(1));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "tag:SMD", fixture.capacitorTypeId).size(),
			static_cast<size_t>(1));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "tag:THT").size(), static_cast<size_t>(0));

		// Scope restricts a query that would otherwise match across types.
		TEST_COMPARE(PartManager::SearchEngine::search(db, "0603", fixture.capacitorTypeId).size(),
			static_cast<size_t>(0));
	}

	TEST_FUNCTION(malformedOrUnknownAttributeYieldsNothing)
	{
		TEST_START;

		Fixture fixture = makeFixture("PartManager_TST_SearchEngine_malformed.db");
		SQLiteWrapper::SQLite& db = *fixture.db;

		// Malformed input must be an empty result, not an exception and not "everything".
		TEST_COMPARE(PartManager::SearchEngine::search(db, "resistance>").size(), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "resistance>abc").size(), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "tag:").size(), static_cast<size_t>(0));

		// An attribute nobody declared has no attr_* column — must be empty, not a SQL error.
		TEST_COMPARE(PartManager::SearchEngine::search(db, "inductance>1u").size(), static_cast<size_t>(0));

		// A quoted SQL fragment is data, never syntax (the terms are bound parameters).
		TEST_COMPARE(PartManager::SearchEngine::search(db, "\"' OR 1=1 --\"").size(), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "tag:\"x'; DROP TABLE part; --\"").size(),
			static_cast<size_t>(0));
		TEST_COMPARE(PartManager::SearchEngine::search(db, "").size(), static_cast<size_t>(3));
	}
#endif

};

TEST_INSTANTIATE(TST_SearchEngine);
