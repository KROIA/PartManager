#pragma once

#include "UnitTest.h"
#include "domain/PartManager_TypeIcon.h"
#include <set>
#include <string>

// The placeholder classifier. Almost every rule here is a near-miss of another one — "LED" is a
// diode by name, "ceramic capacitor" is a capacitor, and "ic" hides inside "silicone" — so the
// ordering and the word-boundary matching are the whole substance of it.
class TST_TypeIcon : public UnitTest::Test
{
	TEST_CLASS(TST_TypeIcon)
public:
	TST_TypeIcon()
		: Test("TST_TypeIcon")
	{
		ADD_TEST(TST_TypeIcon::everySeededTypeIsClassified);
		ADD_TEST(TST_TypeIcon::shortWordsDoNotMatchInsideOtherWords);
		ADD_TEST(TST_TypeIcon::unknownTypesGetAStableReadableColour);
		ADD_TEST(TST_TypeIcon::initials);
	}

private:

	static PartManager::TypeGlyph glyphOf(const std::string& name)
	{
		return PartManager::TypeIconStyle::forType(name).glyph;
	}

	// The 17 types a fresh database is seeded with. If one of these falls through to Generic the
	// user sees a lettered box where a component symbol belongs.
	TEST_FUNCTION(everySeededTypeIsClassified)
	{
		TEST_START;
		using G = PartManager::TypeGlyph;

		TEST_COMPARE(glyphOf("Resistor"), G::Resistor);
		TEST_COMPARE(glyphOf("Capacitor"), G::Capacitor);
		// Caught by the "capacitor" rule, which is the intent — a ceramic cap is still a cap.
		TEST_COMPARE(glyphOf("Ceramic Capacitor"), G::Capacitor);
		TEST_COMPARE(glyphOf("Inductor"), G::Inductor);
		TEST_COMPARE(glyphOf("Power Regulator"), G::Ic);
		TEST_COMPARE(glyphOf("Transistor"), G::Transistor);
		// "MOSFET" carries no "transistor" in it, so it needs its own spelling.
		TEST_COMPARE(glyphOf("MOSFET"), G::Transistor);
		TEST_COMPARE(glyphOf("Diode"), G::Diode);
		// ...but an LED must not be caught by the diode rule, or the two are indistinguishable.
		TEST_COMPARE(glyphOf("LED"), G::Led);
		TEST_COMPARE(glyphOf("Connector"), G::Connector);
		TEST_COMPARE(glyphOf("Crystal / Oscillator"), G::Crystal);
		TEST_COMPARE(glyphOf("Microcontroller"), G::Ic);
		TEST_COMPARE(glyphOf("Op-Amp"), G::Ic);
		TEST_COMPARE(glyphOf("Logic IC"), G::Ic);
		TEST_COMPARE(glyphOf("Switch"), G::Switch);
		TEST_COMPARE(glyphOf("Relay"), G::Relay);
		TEST_COMPARE(glyphOf("Fuse"), G::Fuse);
		TEST_COMPARE(glyphOf("Sensor"), G::Sensor);

		// Matching is case-insensitive, because a user-created type is spelled however they like.
		TEST_COMPARE(glyphOf("resistor"), G::Resistor);
		TEST_COMPARE(glyphOf("SMD RESISTOR 0603"), G::Resistor);
	}

	// The rules that would silently misfile things if matched as bare substrings.
	TEST_FUNCTION(shortWordsDoNotMatchInsideOtherWords)
	{
		TEST_START;
		using G = PartManager::TypeGlyph;

		// "ic" is inside all of these; none of them is an integrated circuit.
		TEST_COMPARE(glyphOf("Silicone Sheet"), G::Generic);
		TEST_COMPARE(glyphOf("Plastic Housing"), G::Generic);
		// "led" is inside these.
		TEST_COMPARE(glyphOf("Modelled Bracket"), G::Generic);
		TEST_COMPARE(glyphOf("Bundled Cable"), G::Generic);
		// "nut" is inside this one, and a walnut sheet is not a fastener.
		TEST_COMPARE(glyphOf("Walnut Sheet"), G::Generic);

		// The same words as whole words must still match.
		TEST_COMPARE(glyphOf("Logic IC"), G::Ic);
		TEST_COMPARE(glyphOf("IC"), G::Ic);
		TEST_COMPARE(glyphOf("RGB LED"), G::Led);
		TEST_COMPARE(glyphOf("Hex Nut"), G::Mechanical);
		TEST_COMPARE(glyphOf("M3 Screw"), G::Mechanical);

		// An unclassifiable name must not throw or pick something at random.
		TEST_COMPARE(glyphOf(""), G::Generic);
		TEST_COMPARE(glyphOf("   "), G::Generic);
	}

	TEST_FUNCTION(unknownTypesGetAStableReadableColour)
	{
		TEST_START;

		// Stable across calls: a table that reshuffled its colours between runs would be worse
		// than one with no colours at all.
		const PartManager::TypeIcon first = PartManager::TypeIconStyle::forType("Widget Frobnicator");
		const PartManager::TypeIcon again = PartManager::TypeIconStyle::forType("Widget Frobnicator");
		TEST_COMPARE(first.colour, again.colour);
		TEST_COMPARE(first.initials, std::string("WF"));

		// Different names generally land on different colours. Not guaranteed for any given
		// pair, so this checks the palette is actually being spread rather than collapsing.
		std::set<std::uint32_t> colours;
		for (const std::string name : { std::string("Alpha"), std::string("Bravo"),
			std::string("Charlie"), std::string("Delta"), std::string("Echo"),
			std::string("Foxtrot"), std::string("Golf"), std::string("Hotel") })
		{
			colours.insert(PartManager::TypeIconStyle::forType(name).colour);
		}
		TEST_ASSERT_M(colours.size() >= 4, "unknown types must spread across the palette");

		// Every classified type gets a colour; 0x000000 would mean "unset leaked through".
		for (const std::string name : { std::string("Resistor"), std::string("LED"),
			std::string("Logic IC"), std::string("M3 Screw"), std::string("Mystery") })
		{
			TEST_ASSERT_M(PartManager::TypeIconStyle::forType(name).colour != 0u,
				"every type must get a colour");
		}
	}

	TEST_FUNCTION(initials)
	{
		TEST_START;

		TEST_COMPARE(PartManager::TypeIconStyle::initialsFor("Wood Sheet"), std::string("WS"));
		// A single word gives one initial, which reads as an accident beside the two-letter
		// ones, so it takes two letters instead.
		TEST_COMPARE(PartManager::TypeIconStyle::initialsFor("Bracket"), std::string("BR"));
		TEST_COMPARE(PartManager::TypeIconStyle::initialsFor("Alpha Beta Gamma"), std::string("AB"));
		// Punctuation and separators are word breaks, not letters.
		TEST_COMPARE(PartManager::TypeIconStyle::initialsFor("Crystal / Oscillator"), std::string("CO"));
		TEST_COMPARE(PartManager::TypeIconStyle::initialsFor(""), std::string());
	}
};

TEST_INSTANTIATE(TST_TypeIcon);
