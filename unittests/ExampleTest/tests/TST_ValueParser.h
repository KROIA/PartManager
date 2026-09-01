#pragma once

#include "UnitTest.h"
#include "units/PartManager_ValueParser.h"
#include "units/PartManager_UnitTable.h"
#include <cmath>

class TST_ValueParser : public UnitTest::Test
{
	TEST_CLASS(TST_ValueParser)
public:
	TST_ValueParser()
		: Test("TST_ValueParser")
	{
		ADD_TEST(TST_ValueParser::bareNumberUsesFieldUnit);
		ADD_TEST(TST_ValueParser::attachedPrefix);
		ADD_TEST(TST_ValueParser::milliVersusMega);
		ADD_TEST(TST_ValueParser::engineeringShorthand);
		ADD_TEST(TST_ValueParser::separators);
		ADD_TEST(TST_ValueParser::explicitUnitSuffix);
		ADD_TEST(TST_ValueParser::searchBarOverload);
		ADD_TEST(TST_ValueParser::invalidInput);
		ADD_TEST(TST_ValueParser::formatting);
		ADD_TEST(TST_ValueParser::roundTrip);
	}

private:

	static const char* ohm() { return PartManager::UnitTable::OhmSymbol; }

	// Relative comparison — these are doubles scaled by powers of ten, exact equality is not the point.
	static bool almostEqual(double a, double b)
	{
		return std::fabs(a - b) <= 1e-9 * std::fmax(std::fabs(a), std::fabs(b)) + 1e-18;
	}

	// Parses in a field declared with `unit` and asserts the base-SI result.
	static bool parsesTo(const std::string& text, const std::string& unit, double expected)
	{
		PartManager::ValueParseResult r = PartManager::ValueParser::parse(text, unit);
		return r.ok && almostEqual(r.value, expected);
	}

	static bool fails(const std::string& text, const std::string& unit)
	{
		PartManager::ValueParseResult r = PartManager::ValueParser::parse(text, unit);
		// A failed parse must leave a harmless zero behind, never a half-parsed number.
		return !r.ok && r.value == 0.0;
	}

	// Tests
	TEST_FUNCTION(bareNumberUsesFieldUnit)
	{
		TEST_START;

		// §2a: a Resistance field with no suffix means Ω, so "100" is exactly 100 Ω.
		TEST_ASSERT(parsesTo("100", ohm(), 100.0));
		TEST_ASSERT(parsesTo("0", ohm(), 0.0));
		TEST_ASSERT(parsesTo("4.7", "F", 4.7));
		TEST_ASSERT(parsesTo("-5", "V", -5.0));
		TEST_ASSERT(parsesTo("  100  ", ohm(), 100.0));
		// "(no unit)" fields take plain numbers.
		TEST_ASSERT(parsesTo("8", "", 8.0));

		PartManager::ValueParseResult r = PartManager::ValueParser::parse("100", ohm());
		TEST_ASSERT(r.ok);
		TEST_ASSERT_M(!r.hasPrefix, "a bare number has no prefix");
		TEST_ASSERT_M(!r.hasExplicitUnit, "no unit was typed");
		TEST_COMPARE(r.mantissa, 100.0);
		// The field's own unit is what the result carries, for the {value, unit} JSON.
		TEST_COMPARE(r.unit, std::string(ohm()));
	}

	TEST_FUNCTION(attachedPrefix)
	{
		TEST_START;

		TEST_ASSERT(parsesTo("1k", ohm(), 1000.0));
		TEST_ASSERT(parsesTo("4.7u", "F", 0.0000047));
		TEST_ASSERT(parsesTo("10n", "F", 1e-8));
		TEST_ASSERT(parsesTo("4.7p", "F", 4.7e-12));
		TEST_ASSERT(parsesTo("1G", "Hz", 1e9));

		// µ (U+00B5, two bytes in UTF-8) is the same prefix as 'u'.
		TEST_ASSERT(parsesTo("4.7\xC2\xB5", "F", 0.0000047));

		PartManager::ValueParseResult r = PartManager::ValueParser::parse("4.7u", "F");
		TEST_ASSERT(r.ok);
		TEST_ASSERT(r.hasPrefix);
		TEST_COMPARE(r.prefix, std::string("u"));
		// §2a keeps what the user typed for round-tripping: mantissa 4.7 with prefix "u".
		TEST_COMPARE(r.mantissa, 4.7);
	}

	TEST_FUNCTION(milliVersusMega)
	{
		TEST_START;

		// The 10^9 trap. Prefix letters are case-sensitive even though unit suffixes are not.
		TEST_ASSERT(parsesTo("1m", ohm(), 0.001));
		TEST_ASSERT(parsesTo("1M", ohm(), 1000000.0));

		PartManager::ValueParseResult milli = PartManager::ValueParser::parse("2.2m", ohm());
		PartManager::ValueParseResult mega = PartManager::ValueParser::parse("2.2M", ohm());
		TEST_ASSERT(milli.ok && mega.ok);
		TEST_ASSERT_M(almostEqual(mega.value / milli.value, 1e9), "'M' must be exactly 10^9 times 'm'");

		// Same trap through the unit-suffix path: the suffix folds case, the prefix does not.
		TEST_ASSERT(parsesTo("1mF", "F", 0.001));
		TEST_ASSERT(parsesTo("1Mf", "F", 1000000.0));
	}

	TEST_FUNCTION(engineeringShorthand)
	{
		TEST_START;

		// Prefix letter replaces the decimal point.
		TEST_ASSERT(parsesTo("4k7", ohm(), 4700.0));
		TEST_ASSERT(parsesTo("2m2", ohm(), 0.0022));
		TEST_ASSERT(parsesTo("1k0", ohm(), 1000.0));
		TEST_ASSERT(parsesTo("100n5", "F", 1.005e-7));
		TEST_ASSERT(parsesTo("2M2", "Hz", 2200000.0));

		// All three §2a forms resolve to the same stored value.
		PartManager::ValueParseResult shorthand = PartManager::ValueParser::parse("4k7", ohm());
		PartManager::ValueParseResult attached = PartManager::ValueParser::parse("4.7k", ohm());
		PartManager::ValueParseResult bare = PartManager::ValueParser::parse("4700", ohm());
		TEST_ASSERT(shorthand.ok && attached.ok && bare.ok);
		TEST_ASSERT(almostEqual(shorthand.value, attached.value));
		TEST_ASSERT(almostEqual(attached.value, bare.value));

		// ...but the human-entered form is still distinguishable for display.
		TEST_COMPARE(shorthand.mantissa, 4.7);
		TEST_COMPARE(bare.mantissa, 4700.0);
	}

	TEST_FUNCTION(separators)
	{
		TEST_START;

		TEST_ASSERT(parsesTo("100", ohm(), 100.0));
		TEST_ASSERT(parsesTo("100.5", ohm(), 100.5));
		// ',' is a decimal separator, never a grouping one.
		TEST_ASSERT(parsesTo("100,5", ohm(), 100.5));
		// "'" is the only grouping separator.
		TEST_ASSERT(parsesTo("100'000", ohm(), 100000.0));
		TEST_ASSERT(parsesTo("1'234'567", ohm(), 1234567.0));
		TEST_ASSERT(parsesTo("1'000,5", ohm(), 1000.5));
		// Grouping and prefixes coexist.
		TEST_ASSERT(parsesTo("1'000k", ohm(), 1000000.0));

		// A second decimal separator is an error however it is spelled.
		TEST_ASSERT(fails("100,5,5", ohm()));
		TEST_ASSERT(fails("1.2.3", ohm()));
		TEST_ASSERT(fails("1,2.3", ohm()));
	}

	TEST_FUNCTION(explicitUnitSuffix)
	{
		TEST_START;

		// Accepted but never required, and case-insensitive on the unit only.
		TEST_ASSERT(parsesTo(std::string("1k") + ohm(), ohm(), 1000.0));
		TEST_ASSERT(parsesTo("100uF", "F", 0.0001));
		TEST_ASSERT(parsesTo("100uf", "F", 0.0001));
		TEST_ASSERT(parsesTo("100 uF", "F", 0.0001));
		TEST_ASSERT(parsesTo("1kHz", "Hz", 1000.0));
		TEST_ASSERT(parsesTo("100mm", "mm", 100.0));
		TEST_ASSERT(parsesTo("1mH", "H", 0.001));
		TEST_ASSERT(parsesTo("5%", "%", 5.0));

		PartManager::ValueParseResult r = PartManager::ValueParser::parse("100uF", "F");
		TEST_ASSERT(r.ok);
		TEST_ASSERT(r.hasExplicitUnit);
		TEST_COMPARE(r.unit, std::string("F"));

		// A suffix that contradicts the field's declared unit is rejected, not silently stored.
		TEST_ASSERT_M(fails("100uF", ohm()), "farads must not be accepted into a resistance field");
		TEST_ASSERT(fails(std::string("1k") + ohm(), "F"));
		TEST_ASSERT_M(fails("5V", ""), "a \"(no unit)\" field takes no unit suffix");
	}

	TEST_FUNCTION(searchBarOverload)
	{
		TEST_START;

		// No declared unit to check against: any suffix is legal, and the caller reads it back out
		// to decide which attr_* columns the §2a pass-1 search should compare.
		PartManager::ValueParseResult withUnit = PartManager::ValueParser::parse("100uF");
		TEST_ASSERT(withUnit.ok);
		TEST_ASSERT(almostEqual(withUnit.value, 0.0001));
		TEST_ASSERT(withUnit.hasExplicitUnit);
		TEST_COMPARE(withUnit.unit, std::string("F"));

		// Bare SI-prefixed magnitude: no unit restriction (§2a "100k checks every dimensioned column").
		PartManager::ValueParseResult bareScaled = PartManager::ValueParser::parse("100k");
		TEST_ASSERT(bareScaled.ok);
		TEST_ASSERT(almostEqual(bareScaled.value, 100000.0));
		TEST_ASSERT(!bareScaled.hasExplicitUnit);
		TEST_ASSERT(bareScaled.unit.empty());

		// §2a's three spellings of the same query all land on 100000.
		TEST_ASSERT(almostEqual(PartManager::ValueParser::parse("100000").value, 100000.0));
		TEST_ASSERT(almostEqual(PartManager::ValueParser::parse("100'000").value, 100000.0));
		TEST_ASSERT(almostEqual(PartManager::ValueParser::parse("4k7").value, 4700.0));

		TEST_ASSERT(!PartManager::ValueParser::parse("resistor").ok);
	}

	TEST_FUNCTION(invalidInput)
	{
		TEST_START;

		TEST_ASSERT(fails("", ohm()));
		TEST_ASSERT(fails("   ", ohm()));
		TEST_ASSERT(fails("abc", ohm()));
		TEST_ASSERT_M(fails("1k2k", ohm()), "two prefixes is not a number");
		TEST_ASSERT(fails("--5", ohm()));
		TEST_ASSERT(fails("-", ohm()));
		TEST_ASSERT(fails(".", ohm()));
		TEST_ASSERT(fails("k", ohm()));
		TEST_ASSERT_M(fails("k7", ohm()), "the shorthand needs a whole part in front of the prefix");
		TEST_ASSERT_M(fails("4.7k5", ohm()), "the prefix already is the decimal point");
		TEST_ASSERT(fails("1e5", ohm()));
		TEST_ASSERT(fails("0x10", ohm()));
		TEST_ASSERT(fails(ohm(), ohm()));
		TEST_ASSERT_M(fails("nan", ohm()), "'n' is nano, but \"an\" is still garbage");
		TEST_ASSERT(fails("100 ohms", ohm()));
	}

	TEST_FUNCTION(formatting)
	{
		TEST_START;

		TEST_COMPARE(PartManager::ValueParser::format(4700.0, ohm()), std::string("4.7 k") + ohm());
		TEST_COMPARE(PartManager::ValueParser::format(100.0, ohm()), std::string("100 ") + ohm());
		TEST_COMPARE(PartManager::ValueParser::format(0.0000047, "F"),
			std::string("4.7 ") + PartManager::UnitTable::MicroSign + "F");
		// Engineering notation: the mantissa stays in [1, 1000), so 1e-7 F is 100 nF, not 0.1 µF.
		TEST_COMPARE(PartManager::ValueParser::format(1e-7, "F"), std::string("100 nF"));
		TEST_COMPARE(PartManager::ValueParser::format(0.0022, ohm()), std::string("2.2 m") + ohm());
		TEST_COMPARE(PartManager::ValueParser::format(2200000.0, "Hz"), std::string("2.2 MHz"));
		TEST_COMPARE(PartManager::ValueParser::format(0.0, ohm()), std::string("0 ") + ohm());
		TEST_COMPARE(PartManager::ValueParser::format(-4700.0, ohm()), std::string("-4.7 k") + ohm());

		// "(no unit)" values print bare, with the prefix still attached when there is one.
		TEST_COMPARE(PartManager::ValueParser::format(8.0), std::string("8"));
		TEST_COMPARE(PartManager::ValueParser::format(1000.0), std::string("1k"));
	}

	TEST_FUNCTION(roundTrip)
	{
		TEST_START;

		const char* inputs[] = { "100", "4k7", "4.7k", "2m2", "1M", "1m", "10n", "4.7p",
								 "1G", "100'000", "100,5", "0", "-3.3k", "1'000k" };
		for (const char* input : inputs)
		{
			PartManager::ValueParseResult first = PartManager::ValueParser::parse(input, ohm());
			TEST_ASSERT_M(first.ok, std::string("must parse: ") + input);

			const std::string formatted = PartManager::ValueParser::format(first.value, ohm());
			PartManager::ValueParseResult second = PartManager::ValueParser::parse(formatted, ohm());
			TEST_ASSERT_M(second.ok, std::string("must re-parse: ") + formatted);
			TEST_ASSERT_M(almostEqual(first.value, second.value),
				std::string("round-trip changed the value: ") + input + " -> " + formatted);
		}
	}
};

TEST_INSTANTIATE(TST_ValueParser);
