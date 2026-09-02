#pragma once

#include "UnitTest.h"
#include "easyeda/PartManager_EasyEdaClient.h"
#include "easyeda/PartManager_EasyEdaConverter.h"
#include <cmath>
#include <string>

// Offline only. Every fixture below is a verbatim excerpt of what easyeda.com actually answered
// for C7950 (onsemi LM358DR2G, SOIC-8) on 2026-09-02 — the shape lines are copied byte for byte,
// so the parsing and unit rules are tested against the real format rather than an invented one.
//
// The two things most likely to be wrong here, and the reason half these asserts exist:
//   * the unit scale — EasyEDA's PCB numbers look like mils but are 10-mil units,
//   * the Y direction — a KiCad symbol measures up, a footprint measures down.
class TST_EasyEda : public UnitTest::Test
{
	TEST_CLASS(TST_EasyEda)
public:
	TST_EasyEda()
		: Test("TST_EasyEda")
	{
		ADD_TEST(TST_EasyEda::searchRowsBecomeHits);
		ADD_TEST(TST_EasyEda::onlyAnExactPartNumberIsAccepted);
		ADD_TEST(TST_EasyEda::componentPayloadIsUnpacked);
		ADD_TEST(TST_EasyEda::symbolPinsKeepTheirPlaceAndDirection);
		ADD_TEST(TST_EasyEda::footprintPadsAreTheRightSizeAndSide);
		ADD_TEST(TST_EasyEda::whatCannotBeConvertedIsReported);
		ADD_TEST(TST_EasyEda::rubbishInNothingOut);
	}

private:

	// Trimmed to four rows; the field names are the real ones.
	static std::string searchJson()
	{
		return R"({"code":200,"msg":null,"result":{"total":19,"productList":[
			{"number":"C5423","mpn":"LM358DR","manufacturer":"TI","package":"SOIC-8"},
			{"number":"C5145450","mpn":"LM358DRG","manufacturer":"HANSCHIP semiconductor","package":"SOP-8"},
			{"number":"C49208520","mpn":"MSLM358DR","manufacturer":"MSKSEMI","package":"SOP-8"},
			{"number":"C7950","mpn":"LM358DR","manufacturer":"onsemi","package":"SOIC-8"}
		]}})";
	}

	// The real head blocks, and a representative slice of each shape array.
	static std::string componentJson()
	{
		return R"({"success":true,"result":{
			"title":"LM358DR2G","lcsc":"C7950",
			"dataStr":{
				"head":{"docType":"2","x":400,"y":305,"c_para":{
					"pre":"U?","name":"LM358DR2G","package":"SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL",
					"Manufacturer":"onsemi","Manufacturer Part":"LM358DR2G","Supplier Part":"C7950"}},
				"shape":[
					"R~360~278~2~2~80~54~#880000~1~0~none~gge16~0~",
					"E~365~283~1.5~1.5~#880000~1~0~#880000~gge14~0",
					"P~show~0~1~350~290~180~gge18~0^^350~290^^M 350 290 h 10~#880000^^1~363.7~294~0~1OUT~start~~~#0000FF^^1~359.5~289~0~1~end~~~#0000FF^^0~357~290^^0~M 360 293 L 363 290 L 360 287",
					"P~show~0~8~450~290~0~gge67~0^^450~290^^M 450 290 h -10~#FF0000^^1~436.3~294~0~VCC~end~~~#FF0000^^1~440.5~289~0~8~start~~~#FF0000^^0~443~290^^0~M 440 287 L 437 290 L 440 293",
					"T~text~380~300~0~#000000~gge99~0"
				]},
			"packageDetail":{"title":"SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL","dataStr":{
				"head":{"docType":"4","x":4000,"y":3000,"c_para":{"package":"SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL"}},
				"shape":[
					"PAD~OVAL~3992.5~3010.65~2.2378~7.678~1~~1~0~3992.5 3007.9299 3992.5 3013.3701~0~gge1002~0~~Y~0~0.0000~0.2000~3992.5,3010.65",
					"PAD~OVAL~3992.5~2989.35~2.2378~7.678~1~~8~0~3992.5 2986.6299 3992.5 2992.0701~0~gge1014~0~~Y~0~0.0000~0.2000~3992.5,2989.35",
					"TRACK~1~3~~3990.1575 3005.9055 4009.8425 3005.9055~gge1146~0",
					"CIRCLE~3993~3004~0.591~1.1811~3~gge1025~0~~",
					"ARC~1~3~~M3990.1575,3002.8605 A2.8648,2.8648 0 0 0 3990.1768,2997.1406~~gge1125~0",
					"SOLIDREGION~100~~M 4006.6929 2992.3622 L 4006.6929 2991.437 Z~cutout~gge1064~~~~0",
					"SOLIDREGION~99~~M 3990.1575 3007.874 L 3990.1575 2992.126 Z~solid~gge1133~~~~0",
					"SVGNODE~{\"gId\":\"g1_outline\",\"nodeName\":\"g\"}"
				]}}
		}})";
	}

	static PartManager::EasyEdaComponent component()
	{
		return PartManager::EasyEdaClient::parseComponentResponse(componentJson());
	}

	// True when `text` contains `needle`. Reads better than the npos comparison at every call.
	static bool has(const std::string& text, const std::string& needle)
	{
		return text.find(needle) != std::string::npos;
	}

	TEST_FUNCTION(searchRowsBecomeHits)
	{
		TEST_START;
		const PartManager::EasyEdaSearchResult result =
			PartManager::EasyEdaClient::parseSearchResponse(searchJson());

		TEST_ASSERT(result.ok);
		TEST_COMPARE(result.total, 19);
		TEST_COMPARE(result.hits.size(), static_cast<size_t>(4));
		// `number`, not `productCode` — this endpoint does not send a productCode at all, and
		// reading the wrong key gives four hits with empty LCSC codes and no error.
		TEST_COMPARE(result.hits[0].lcscCode, std::string("C5423"));
		TEST_COMPARE(result.hits[0].manufacturer, std::string("TI"));
	}

	TEST_FUNCTION(onlyAnExactPartNumberIsAccepted)
	{
		TEST_START;
		using Client = PartManager::EasyEdaClient;
		const PartManager::EasyEdaSearchResult result = Client::parseSearchResponse(searchJson());

		// Two rows are exactly "LM358DR"; the manufacturer Mouser named breaks the tie.
		TEST_COMPARE(Client::bestMatch(result, "LM358DR", "onsemi"), std::string("C7950"));
		TEST_COMPARE(Client::bestMatch(result, "LM358DR", "Texas Instruments"), std::string("C5423"));
		// No manufacturer to go on: the first exact match, not a guess between them.
		TEST_COMPARE(Client::bestMatch(result, "LM358DR", ""), std::string("C5423"));

		// The whole point. "LM358DRG" and "MSLM358DR" are in the result set and are different
		// parts in different packages; accepting either would put a wrong land pattern on a board.
		TEST_COMPARE(Client::bestMatch(result, "LM358", "TI"), std::string());
		TEST_COMPARE(Client::bestMatch(result, "LM358D", "TI"), std::string());
		TEST_COMPARE(Client::bestMatch(result, "", "TI"), std::string());

		// Mouser and LCSC disagree about punctuation inside a part number, so it is ignored.
		TEST_ASSERT(Client::samePartNumber("LM358-DR", "lm358dr"));
		TEST_ASSERT(Client::samePartNumber("LT1506CR-3.3PBF", "LT1506CR33PBF"));
		TEST_ASSERT(!Client::samePartNumber("LM358DR", "LM358DRG"));
		TEST_ASSERT(!Client::samePartNumber("", ""));
	}

	TEST_FUNCTION(componentPayloadIsUnpacked)
	{
		TEST_START;
		const PartManager::EasyEdaComponent parsed = component();

		TEST_ASSERT_M(parsed.ok, parsed.errorMessage.c_str());
		TEST_COMPARE(parsed.mpn, std::string("LM358DR2G"));
		TEST_COMPARE(parsed.lcscCode, std::string("C7950"));
		TEST_COMPARE(parsed.packageName, std::string("SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL"));
		// "U?" is EasyEDA's placeholder spelling; KiCad wants the prefix without it.
		TEST_COMPARE(parsed.referencePrefix, std::string("U"));
		TEST_COMPARE(parsed.symbolOriginX, 400.0);
		TEST_COMPARE(parsed.symbolOriginY, 305.0);
		TEST_COMPARE(parsed.footprintOriginX, 4000.0);
		TEST_COMPARE(parsed.footprintOriginY, 3000.0);
		TEST_ASSERT(parsed.hasSymbol());
		TEST_ASSERT(parsed.hasFootprint());
	}

	TEST_FUNCTION(symbolPinsKeepTheirPlaceAndDirection)
	{
		TEST_START;
		int pins = 0;
		std::vector<std::string> skipped;
		const std::string block =
			PartManager::EasyEdaConverter::symbolBlock(component(), "LM358DR2G", pins, skipped);

		TEST_COMPARE(pins, 2);

		// Pin 1 sits at EasyEDA (350,290) with the origin at (400,305): x is 50 units left of it,
		// y is 15 units *above* — and above is +Y in a KiCad symbol, so the sign flips.
		TEST_ASSERT_M(has(block, "(at -12.7 3.81 0)"), "left pin position or angle is wrong");
		// Pin 8 is the mirror of it, and its angle must be the opposite: EasyEDA's rotation points
		// away from the body, KiCad's points into it.
		TEST_ASSERT_M(has(block, "(at 12.7 3.81 180)"), "right pin position or angle is wrong");
		TEST_ASSERT(has(block, "(length 2.54)"));
		TEST_ASSERT(has(block, "(number \"1\""));
		TEST_ASSERT(has(block, "(name \"1OUT\""));
		TEST_ASSERT(has(block, "(name \"VCC\""));

		// The body rectangle, same Y flip: EasyEDA y=278 is 27 units above the origin.
		TEST_ASSERT_M(has(block, "(start -10.16 6.86)"), "body rectangle start is wrong");
		TEST_ASSERT_M(has(block, "(end 10.16 -6.86)"), "body rectangle end is wrong");

		// KiCad finds a symbol's graphics and pins by the "_0_1"/"_1_1" naming convention; without
		// both sub-symbols the entry loads but draws nothing.
		TEST_ASSERT(has(block, "(symbol \"LM358DR2G_0_1\""));
		TEST_ASSERT(has(block, "(symbol \"LM358DR2G_1_1\""));
		TEST_ASSERT(has(block, "(property \"LCSC\" \"C7950\""));
	}

	TEST_FUNCTION(footprintPadsAreTheRightSizeAndSide)
	{
		TEST_START;
		int pads = 0;
		std::vector<std::string> skipped;
		const std::string text =
			PartManager::EasyEdaConverter::footprintFile(component(), "SOIC-8", pads, skipped);

		TEST_COMPARE(pads, 2);

		// The unit scale, stated as a number rather than trusted: pad 1 is 7.5 units left of and
		// 10.65 units below the origin, at 0.254 mm per unit.
		TEST_ASSERT_M(has(text, "(at -1.905 2.7051)"), "pad 1 position is wrong (unit scale?)");
		// And unlike the symbol, a footprint's Y is *not* flipped — pin 8 is above the origin in
		// EasyEDA and must stay above it here, i.e. negative.
		TEST_ASSERT_M(has(text, "(at -1.905 -2.7051)"), "pad 8 must keep its sign; Y is not flipped");
		// 2.2378 x 7.678 units is 0.568 x 1.950 mm — a plausible SOIC-8 land. A factor-of-ten
		// error would make this 0.0568 mm and the whole footprint unbuildable.
		TEST_ASSERT_M(has(text, "(size 0.5684 1.9502)"), "pad size is wrong (unit scale?)");
		TEST_ASSERT(has(text, "(pad \"1\" smd oval"));
		TEST_ASSERT(has(text, "(layers \"F.Cu\" \"F.Paste\" \"F.Mask\")"));
		// Layer 3 is the top silkscreen.
		TEST_ASSERT(has(text, "(layer \"F.SilkS\")"));
		TEST_ASSERT(has(text, "(fp_line"));
		TEST_ASSERT(has(text, "(fp_circle"));
		// KiCad's circle takes a rim point, not a radius: 0.591 units is 0.1501 mm out from
		// the centre at x = (3993-4000)*0.254 = -1.778.
		TEST_ASSERT_M(has(text, "(center -1.778 1.016)"), "circle centre is wrong");
		TEST_ASSERT_M(has(text, "(end -1.6279 1.016)"), "circle must end on its rim, not its radius");

		// The arc closes the gap in the body's left edge at x = -2.5: it is the pin-1 notch, and
		// it is cut *into* the body, so its mid point must lie to the right of both endpoints.
		// The SVG centre-side flag is what decides that, and getting it backwards produces a
		// perfectly valid arc bulging the wrong way — which no other assertion here would catch.
		TEST_ASSERT(has(text, "(fp_arc"));
		TEST_ASSERT(has(text, "(start -2.5 0.7266)"));
		TEST_ASSERT(has(text, "(end -2.4951 -0.7263)"));
		TEST_ASSERT_M(has(text, "(mid -1.81"), "the pin-1 notch must bulge into the body, not out of it");

		TEST_ASSERT(has(text, "(footprint \"SOIC-8\""));
		TEST_ASSERT(has(text, "(property \"Reference\" \"REF**\""));
	}

	TEST_FUNCTION(whatCannotBeConvertedIsReported)
	{
		TEST_START;
		const PartManager::EasyEdaConversion converted =
			PartManager::EasyEdaConverter::convert(component());

		TEST_ASSERT_M(converted.ok, converted.errorMessage.c_str());
		TEST_ASSERT(converted.hasSymbol());
		TEST_ASSERT(converted.hasFootprint());
		TEST_COMPARE(converted.pinCount, 2);
		TEST_COMPARE(converted.padCount, 2);
		TEST_COMPARE(converted.symbolName, std::string("LM358DR2G"));
		// The package name carries dots and dashes KiCad tolerates but a filename should not fight.
		TEST_COMPARE(converted.footprintName,
			std::string("SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL"));

		// A silently dropped shape is a footprint that looks complete and is not, so the two
		// SOLIDREGIONs, the SVGNODE and the symbol text must all be named.
		bool sawRegion = false;
		bool sawModel = false;
		bool sawText = false;
		for (const std::string& entry : converted.skipped)
		{
			if (has(entry, "region")) { sawRegion = true; }
			if (has(entry, "3D model")) { sawModel = true; }
			if (has(entry, "symbol text")) { sawText = true; }
		}
		TEST_ASSERT_M(sawRegion, "the two SOLIDREGIONs must be reported");
		TEST_ASSERT_M(sawModel, "the SVGNODE must be reported");
		TEST_ASSERT_M(sawText, "the schematic text must be reported");
		// Counted, not listed one by one.
		bool sawCount = false;
		for (const std::string& entry : converted.skipped)
		{
			if (has(entry, "region") && has(entry, "x2")) { sawCount = true; }
		}
		TEST_ASSERT_M(sawCount, "repeated skips should be counted");

		// The symbol comes back as a library a user can open, not a bare fragment.
		TEST_ASSERT(has(converted.symbolLibraryText, "(kicad_symbol_lib"));
	}

	TEST_FUNCTION(rubbishInNothingOut)
	{
		TEST_START;
		using Client = PartManager::EasyEdaClient;
		using Converter = PartManager::EasyEdaConverter;

		TEST_ASSERT(!Client::parseSearchResponse("not json at all").ok);
		TEST_ASSERT(!Client::parseSearchResponse("").ok);
		TEST_ASSERT(!Client::parseComponentResponse("{}").ok);
		// The API reports its own failures inside a 200, so a code that is not 200 is a failure
		// even though the transport succeeded.
		TEST_ASSERT(!Client::parseSearchResponse(R"({"code":500,"msg":"boom"})").ok);
		TEST_COMPARE(Client::parseSearchResponse(R"({"code":500,"msg":"boom"})").errorMessage,
			std::string("boom"));
		// An empty result set is an answer, not an error — LCSC does not carry every part.
		const PartManager::EasyEdaSearchResult none =
			Client::parseSearchResponse(R"({"code":200,"result":{"total":0,"productList":[]}})");
		TEST_ASSERT(none.ok);
		TEST_ASSERT(none.hits.empty());

		// A component with shapes but none convertible must fail loudly rather than produce an
		// empty library KiCad would open and show nothing in.
		PartManager::EasyEdaComponent empty;
		empty.ok = true;
		empty.symbolShapes = { "T~text~1~2~0~#000~gge1~0" };
		empty.footprintShapes = { "SVGNODE~{}" };
		const PartManager::EasyEdaConversion nothing = Converter::convert(empty);
		TEST_ASSERT(!nothing.ok);
		TEST_ASSERT(!nothing.errorMessage.empty());

		TEST_COMPARE(Converter::sanitize(""), std::string("Unnamed"));
		TEST_COMPARE(Converter::sanitize("SOIC-8 (wide)"), std::string("SOIC-8_wide"));
		TEST_COMPARE(Converter::kicadLayer(3), std::string("F.SilkS"));
		TEST_COMPARE(Converter::kicadLayer(4242), std::string());
	}
};

TEST_INSTANTIATE(TST_EasyEda);
