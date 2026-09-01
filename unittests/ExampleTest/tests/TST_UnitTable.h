#pragma once

#include "UnitTest.h"
#include "units/PartManager_UnitTable.h"

class TST_UnitTable : public UnitTest::Test
{
	TEST_CLASS(TST_UnitTable)
public:
	TST_UnitTable()
		: Test("TST_UnitTable")
	{
		ADD_TEST(TST_UnitTable::dropdownMatchesSpec);
		ADD_TEST(TST_UnitTable::prefixLookupIsCaseSensitive);
		ADD_TEST(TST_UnitTable::unitSuffixLongestMatchWins);
	}

private:

	TEST_FUNCTION(dropdownMatchesSpec)
	{
		TEST_START;

		const std::vector<std::string>& units = PartManager::UnitTable::units();
		// The §2a table: 11 entries, "(no unit)" first.
		TEST_COMPARE(units.size(), static_cast<size_t>(11));
		TEST_ASSERT_M(units[0].empty(), "\"(no unit)\" must be the first dropdown entry");
		TEST_COMPARE(units[1], std::string(PartManager::UnitTable::OhmSymbol));
		TEST_COMPARE(units[1], std::string("\xCE\xA9"));   // U+03A9, not the U+2126 OHM SIGN

		TEST_ASSERT(PartManager::UnitTable::isValidUnit(""));
		TEST_ASSERT(PartManager::UnitTable::isValidUnit("Hz"));
		TEST_ASSERT(PartManager::UnitTable::isValidUnit("mm"));
		TEST_ASSERT_M(!PartManager::UnitTable::isValidUnit("kg"), "kg is not on the §2a dropdown");
		TEST_ASSERT_M(!PartManager::UnitTable::isValidUnit("hz"), "isValidUnit is exact, not case-folded");

		TEST_ASSERT(PartManager::UnitTable::unitsEqual("hz", "Hz"));
		TEST_ASSERT(PartManager::UnitTable::unitsEqual("f", "F"));
		TEST_ASSERT(!PartManager::UnitTable::unitsEqual("F", "H"));
		// ASCII-only folding must leave multi-byte symbols alone.
		TEST_ASSERT(PartManager::UnitTable::unitsEqual("\xCE\xA9", "\xCE\xA9"));
		TEST_ASSERT(!PartManager::UnitTable::unitsEqual("\xCE\xA9", "\xC2\xB5"));
	}

	TEST_FUNCTION(prefixLookupIsCaseSensitive)
	{
		TEST_START;

		double multiplier = 0.0;
		size_t length = 0;

		TEST_ASSERT(PartManager::UnitTable::prefixAt("m", 0, multiplier, length));
		TEST_COMPARE(multiplier, 1e-3);
		TEST_COMPARE(length, static_cast<size_t>(1));

		// The 10^9 trap: 'm' is milli, 'M' is mega — never case-folded.
		TEST_ASSERT(PartManager::UnitTable::prefixAt("M", 0, multiplier, length));
		TEST_COMPARE(multiplier, 1e6);

		// 'u' and µ are the same prefix; µ is two bytes in UTF-8.
		TEST_ASSERT(PartManager::UnitTable::prefixAt("u", 0, multiplier, length));
		TEST_COMPARE(multiplier, 1e-6);
		TEST_COMPARE(length, static_cast<size_t>(1));
		TEST_ASSERT(PartManager::UnitTable::prefixAt("\xC2\xB5", 0, multiplier, length));
		TEST_COMPARE(multiplier, 1e-6);
		TEST_COMPARE(length, static_cast<size_t>(2));

		// Offset lookup, as the parser uses it.
		TEST_ASSERT(PartManager::UnitTable::prefixAt("4k7", 1, multiplier, length));
		TEST_COMPARE(multiplier, 1e3);

		TEST_ASSERT(!PartManager::UnitTable::prefixAt("x", 0, multiplier, length));
		TEST_ASSERT_M(!PartManager::UnitTable::prefixAt("K", 0, multiplier, length), "only lowercase k is kilo");
		TEST_ASSERT_M(!PartManager::UnitTable::prefixAt("N", 0, multiplier, length), "only lowercase n is nano");

		// Display side: exponent -> canonical prefix.
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(0), std::string(""));
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(3), std::string("k"));
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(-3), std::string("m"));
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(6), std::string("M"));
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(-6), std::string(PartManager::UnitTable::MicroSign));
		TEST_COMPARE(PartManager::UnitTable::prefixForExponent(15), std::string(""));
	}

	TEST_FUNCTION(unitSuffixLongestMatchWins)
	{
		TEST_START;

		std::string unit;
		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("1Hz", unit), static_cast<size_t>(2));
		TEST_COMPARE(unit, std::string("Hz"));

		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("100mm", unit), static_cast<size_t>(2));
		TEST_COMPARE(unit, std::string("mm"));

		// Case-insensitive on the unit, and the canonical spelling comes back out.
		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("100uf", unit), static_cast<size_t>(1));
		TEST_COMPARE(unit, std::string("F"));

		// Ω is two bytes.
		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("1k\xCE\xA9", unit), static_cast<size_t>(2));
		TEST_COMPARE(unit, std::string(PartManager::UnitTable::OhmSymbol));

		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("100k", unit), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::UnitTable::matchUnitSuffix("4.7", unit), static_cast<size_t>(0));
	}
};

TEST_INSTANTIATE(TST_UnitTable);
