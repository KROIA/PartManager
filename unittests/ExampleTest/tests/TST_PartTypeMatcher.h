#pragma once

#include "UnitTest.h"
#include "domain/PartManager_PartTypeMatcher.h"

// Picking the part type a vendor category belongs to (§2, §6). Pure: no database, no widgets, no
// network — the types are a hand-built vector, which is also the point of the rewrite. The old
// route looked MouserSearchService::suggestedTypeName()'s hardcoded answer up as combo text and so
// could only ever find a built-in template under its original name; these fixtures deliberately
// include a type nobody seeded ("Neopixel 5050 WS2812B") to prove the new one does not care.
//
// The case that matters most is the last one: nothing matching must come back with nothing, not
// with a plausible guess. A wrong category silently attaches the wrong attribute template.
class TST_PartTypeMatcher : public UnitTest::Test
{
	TEST_CLASS(TST_PartTypeMatcher)
public:
	TST_PartTypeMatcher()
		: Test("TST_PartTypeMatcher")
	{
		ADD_TEST(TST_PartTypeMatcher::anExactNameWins);
		ADD_TEST(TST_PartTypeMatcher::aNameInsideTheCategoryIsAWholeWord);
		ADD_TEST(TST_PartTypeMatcher::aChildBeatsTheTypeItInheritsFrom);
		ADD_TEST(TST_PartTypeMatcher::synonymsCoverTheObviousCases);
		ADD_TEST(TST_PartTypeMatcher::nothingMatchingMatchesNothing);
	}

private:

	// A small database's worth of types, including a user-made one and a child template.
	static std::vector<PartManager::PartType> types()
	{
		const auto type = [](int id, const char* name, int parent)
		{
			PartManager::PartType partType;
			partType.id = id;
			partType.name = name;
			partType.parentTypeId = parent;
			return partType;
		};
		return {
			type(1, "Resistor", PartManager::NoParentType),
			type(2, "Capacitor", PartManager::NoParentType),
			type(3, "Ceramic Capacitor", 2),
			type(4, "MOSFET", PartManager::NoParentType),
			type(5, "Transistor", PartManager::NoParentType),
			type(6, "LED", PartManager::NoParentType),
			type(7, "IC", PartManager::NoParentType),
			// Nobody seeded this one and suggestedTypeName() has never heard of it.
			type(8, "Neopixel 5050 WS2812B", 6),
		};
	}

	TEST_FUNCTION(anExactNameWins)
	{
		TEST_START;

		// What MouserSearchService already worked out, when it worked something out.
		const PartManager::TypeMatch bySuggestion =
			PartManager::matchPartType(types(), "Resistors - Chip SMD", "", "Resistor");
		TEST_ASSERT(bySuggestion.confident);
		TEST_COMPARE(bySuggestion.typeId, 1);
		TEST_COMPARE(bySuggestion.matchedOn, std::string("suggested-type-name"));

		// The category alone, when it happens to be the type's name. Case and punctuation are
		// folded away on both sides, so "mosfet" finds "MOSFET".
		const PartManager::TypeMatch byCategory =
			PartManager::matchPartType(types(), "mosfet", "", "");
		TEST_ASSERT(byCategory.confident);
		TEST_COMPARE(byCategory.typeId, 4);
		TEST_COMPARE(byCategory.matchedOn, std::string("category-exact"));

		// Two letters is too short to be found *inside* anything, but it is still a name.
		const PartManager::TypeMatch shortName =
			PartManager::matchPartType(types(), "IC", "", "");
		TEST_COMPARE(shortName.typeId, 7);
	}

	TEST_FUNCTION(aNameInsideTheCategoryIsAWholeWord)
	{
		TEST_START;

		// Mouser's own category strings, which name the type but are not equal to it. The plural
		// is folded, so "Resistors" finds "Resistor".
		const PartManager::TypeMatch inCategory = PartManager::matchPartType(types(),
			"Thick Film Resistors", "4.7K OHM 1% 0603", "");
		TEST_ASSERT(inCategory.confident);
		TEST_COMPARE(inCategory.typeId, 1);
		TEST_COMPARE(inCategory.matchedOn, std::string("category-word"));

		// The description is the fallback, and it really is one: a category that names the type
		// outranks a description that does.
		const PartManager::TypeMatch inDescription = PartManager::matchPartType(types(),
			"Interface - Specialized", "Precision resistor network, 1%", "");
		TEST_ASSERT(inDescription.confident);
		TEST_COMPARE(inDescription.typeId, 1);
		TEST_COMPARE(inDescription.matchedOn, std::string("description-word"));

		// The user's own type, which no hardcoded table knows and the old findText() route could
		// never have reached.
		const PartManager::TypeMatch userMade = PartManager::matchPartType(types(),
			"Neopixel 5050 WS2812B LEDs", "", "");
		TEST_ASSERT(userMade.confident);
		TEST_COMPARE(userMade.typeId, 8);

		// Whole words, not substrings: "LED" must not be found inside "Sealed".
		const PartManager::TypeMatch notASubstring = PartManager::matchPartType(types(),
			"Sealed Lead Acid Batteries", "", "");
		TEST_ASSERT_M(!notASubstring.confident,
			"a type name found inside a longer word is a coincidence, not a match");
	}

	TEST_FUNCTION(aChildBeatsTheTypeItInheritsFrom)
	{
		TEST_START;

		// Both names are in the category. The child carries the more specific attribute template,
		// so it is the one the part should be filed under.
		const PartManager::TypeMatch specific = PartManager::matchPartType(types(),
			"Capacitors - Ceramic Capacitor MLCC SMD", "", "");
		TEST_ASSERT(specific.confident);
		TEST_COMPARE(specific.typeId, 3);

		// And the parent still wins when only the parent is named — being specific is not a
		// licence to guess.
		const PartManager::TypeMatch general = PartManager::matchPartType(types(),
			"Aluminium Electrolytic Capacitor", "", "");
		TEST_COMPARE(general.typeId, 2);
	}

	TEST_FUNCTION(synonymsCoverTheObviousCases)
	{
		TEST_START;

		// What a datasheet calls an LED when it is being formal.
		const PartManager::TypeMatch led = PartManager::matchPartType(types(),
			"Standard Light Emitting Diode", "", "");
		TEST_ASSERT(led.confident);
		TEST_COMPARE(led.typeId, 6);
		TEST_COMPARE(led.matchedOn, std::string("synonym-category"));

		// "FET" for a MOSFET, plural folded.
		const PartManager::TypeMatch fet = PartManager::matchPartType(types(),
			"Power Management", "N-channel FETs, 60V", "");
		TEST_ASSERT(fet.confident);
		TEST_COMPARE(fet.typeId, 4);

		// "BJT" for a plain transistor.
		const PartManager::TypeMatch bjt = PartManager::matchPartType(types(),
			"BJT Arrays", "", "");
		TEST_ASSERT(bjt.confident);
		TEST_COMPARE(bjt.typeId, 5);

		// A real name outranks a synonym: the category says MOSFET outright, so the fact that
		// "transistor" is a synonym of nothing here does not drag it to the general template.
		const PartManager::TypeMatch both = PartManager::matchPartType(types(),
			"MOSFETs", "Transistor, 60V, N-channel", "");
		TEST_COMPARE(both.typeId, 4);
	}

	TEST_FUNCTION(nothingMatchingMatchesNothing)
	{
		TEST_START;

		// The rule the whole file exists for: a wrong category is worse than no category, because
		// it silently gives the part the wrong attribute template.
		const PartManager::TypeMatch none = PartManager::matchPartType(types(),
			"Rectangular Connectors - Headers, Male Pins", "2.54mm pitch, 40-pin", "");
		TEST_ASSERT_M(!none.confident, "a category with no matching type must not be guessed at");
		TEST_COMPARE(none.typeId, 0);
		TEST_ASSERT(none.matchedOn.empty());

		// Nothing to go on at all, and an empty type list, are both the same answer.
		const PartManager::TypeMatch empty = PartManager::matchPartType(types(), "", "", "");
		TEST_COMPARE(empty.typeId, 0);
		TEST_ASSERT(!empty.confident);
		const PartManager::TypeMatch noTypes = PartManager::matchPartType(
			std::vector<PartManager::PartType>(), "Resistors", "", "Resistor");
		TEST_COMPARE(noTypes.typeId, 0);
		TEST_ASSERT(!noTypes.confident);
	}
};

TEST_INSTANTIATE(TST_PartTypeMatcher);
