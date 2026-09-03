#pragma once

#include "UnitTest.h"
#include "mouser/PartManager_MouserClient.h"
#include "mouser/PartManager_MouserSearchService.h"

// Offline only: every fixture below is a literal response body shaped after
// MouserAPI_V1.json (SearchResponseRoot / SearchResponse / MouserPart /
// ProductAttribute / ErrorEntity). Nothing here needs the API key or a network.
class TST_MouserSearchService : public UnitTest::Test
{
	TEST_CLASS(TST_MouserSearchService)
public:
	TST_MouserSearchService()
		: Test("TST_MouserSearchService")
	{
		ADD_TEST(TST_MouserSearchService::wellFormedResponseParsesAndMaps);
		ADD_TEST(TST_MouserSearchService::inBodyErrorArrayIsAFailure);
		ADD_TEST(TST_MouserSearchService::emptyResultSetIsNotAnError);
		ADD_TEST(TST_MouserSearchService::malformedBodyIsRejected);
		ADD_TEST(TST_MouserSearchService::attributeValuesBecomeBaseSi);
		ADD_TEST(TST_MouserSearchService::categoryMappingRefusesToGuess);
		ADD_TEST(TST_MouserSearchService::closestMatchIsRankedFirst);
		ADD_TEST(TST_MouserSearchService::productLinksYieldTheirPartNumber);
		ADD_TEST(TST_MouserSearchService::imagePathIsUpgradedToTheLargeVariant);
		ADD_TEST(TST_MouserSearchService::anEmptyDatasheetUrlFallsBackToTheManufacturer);
		ADD_TEST(TST_MouserSearchService::descriptionsFillTheTemplatesSlots);
		ADD_TEST(TST_MouserSearchService::anAmbiguousDescriptionTokenIsLeftAlone);
		ADD_TEST(TST_MouserSearchService::thePartNumberIsTheLastSourceOfAPackage);
	}

private:

	// Mouser sends "" for DataSheetUrl on most real parts, and its product page — where the link
	// actually is — is behind a JavaScript bot challenge. The fallback is a short table of
	// manufacturers who publish at a URL derivable from the part number; see the header.
	TEST_FUNCTION(anEmptyDatasheetUrlFallsBackToTheManufacturer)
	{
		TEST_START;
		using S = PartManager::MouserSearchService;

		TEST_COMPARE(S::datasheetUrlFor("Wurth Elektronik", "150120YS75000"),
			std::string("https://www.we-online.com/catalog/datasheet/150120YS75000.pdf"));
		// However Mouser spells it, and whatever case it arrives in.
		TEST_COMPARE(S::datasheetUrlFor("Würth Elektronik eiSos", "74437346220"),
			std::string("https://www.we-online.com/catalog/datasheet/74437346220.pdf"));
		TEST_COMPARE(S::datasheetUrlFor("WURTH ELEKTRONIK", "885012207072"),
			std::string("https://www.we-online.com/catalog/datasheet/885012207072.pdf"));

		// Everyone else: no guess. A wrong URL downloads a 404 page, and a part then looks like it
		// has a datasheet when it has an HTML error document.
		TEST_COMPARE(S::datasheetUrlFor("Texas Instruments", "LM358DR"), std::string());
		TEST_COMPARE(S::datasheetUrlFor("", "150120YS75000"), std::string());
		TEST_COMPARE(S::datasheetUrlFor("Wurth Elektronik", ""), std::string());
		// A part number that would escape the path is refused rather than pasted into a URL.
		TEST_COMPARE(S::datasheetUrlFor("Wurth Elektronik", "../../etc/passwd"), std::string());
		TEST_COMPARE(S::datasheetUrlFor("Wurth Elektronik", "150120 YS75000"), std::string());

		// The prefill only reaches for it when Mouser gave nothing; a real DataSheetUrl wins.
		PartManager::MouserPartDto dto;
		dto.manufacturer = "Wurth Elektronik";
		dto.manufacturerPartNumber = "150120YS75000";
		TEST_COMPARE(S::toPrefill(dto).datasheetUrl,
			std::string("https://www.we-online.com/catalog/datasheet/150120YS75000.pdf"));
		dto.dataSheetUrl = "https://www.mouser.com/datasheet/2/445/whatever-123.pdf";
		TEST_COMPARE(S::toPrefill(dto).datasheetUrl, dto.dataSheetUrl);
	}

	// The API hands out a ~1.5 kB thumbnail; the same picture sits one path segment away.
	// Real URLs, checked live against api.mouser.com and www.mouser.com on 2026-09-02.
	TEST_FUNCTION(imagePathIsUpgradedToTheLargeVariant)
	{
		TEST_START;
		using S = PartManager::MouserSearchService;

		TEST_COMPARE(S::previewImageUrl("https://www.mouser.ch/images/wurthelectronics/images/WL-SMCW.JPG"),
			std::string("https://www.mouser.ch/images/wurthelectronics/lrg/WL-SMCW.JPG"));
		// The API is not consistent about which variant it names, so all of them must land on lrg.
		TEST_COMPARE(S::previewImageUrl("https://www.mouser.com/images/vishay/sm/CRCW_SPL.jpg"),
			std::string("https://www.mouser.com/images/vishay/lrg/CRCW_SPL.jpg"));
		TEST_COMPARE(S::previewImageUrl("https://www.mouser.com/images/yageo/hd/SMD_MLCC_series_SPL.JPG"),
			std::string("https://www.mouser.com/images/yageo/lrg/SMD_MLCC_series_SPL.JPG"));
		// Already large: rewriting it to itself is the same answer, not a no-op that needs a branch.
		TEST_COMPARE(S::previewImageUrl("https://www.mouser.com/images/onsemiconductor/lrg/TO-220.jpg"),
			std::string("https://www.mouser.com/images/onsemiconductor/lrg/TO-220.jpg"));

		// Anything not in the four-segment /images/ shape is left exactly as it came. A URL we do
		// not recognise still has to download — mangling it would lose the picture altogether.
		for (const char* untouched : {
			"",
			"https://example.com/photo.jpg",
			"https://www.mouser.com/images/vishay/CRCW_SPL.jpg",              // three segments
			"https://www.mouser.com/images/vishay/lrg/sub/CRCW_SPL.jpg",      // five
			"https://www.mouser.com/pictures/vishay/sm/CRCW_SPL.jpg",         // not /images/
			"https://www.mouser.com/images/vishay/sm/" })                     // trailing slash, no file
		{
			TEST_COMPARE(S::previewImageUrl(untouched), std::string(untouched));
		}
	}

	TEST_FUNCTION(productLinksYieldTheirPartNumber)
	{
		TEST_START;

		// Every link shape in the user's own stock list (`.claude/DefaultParts.csv`): the .ch
		// domain, the /en/ language segment, and a percent-encoded qs query to be discarded.
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"https://www.mouser.ch/en/ProductDetail/onsemi/MBRS330T3G?qs=3JMERSakebqQvNYO1akzWA%3D%3D"),
			std::string("MBRS330T3G"));
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"https://www.mouser.com/ProductDetail/Texas-Instruments/DRV5053CAQLPGM"),
			std::string("DRV5053CAQLPGM"));
		// Casing is Mouser's own business; the path segment is not.
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"HTTPS://WWW.MOUSER.DE/EN/PRODUCTDETAIL/Wurth-Elektronik/150120YS75000?qs=x"),
			std::string("150120YS75000"));
		// A trailing slash and a fragment must not swallow or extend the part number.
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"https://www.mouser.ch/en/ProductDetail/Diodes-Incorporated/2N7002-7-F/#specs"),
			std::string("2N7002-7-F"));

		// Anything that is not a Mouser product page has to come back empty, so the caller
		// searches the text the user actually typed instead of a mangled fragment of a URL.
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl("595-LM358DR"), std::string());
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl("10k 0603 resistor"), std::string());
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"https://www.digikey.ch/en/products/detail/onsemi/MBRS330T3G/1234"), std::string());
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(
			"https://www.mouser.ch/en/c/semiconductors/"), std::string());
		TEST_COMPARE(PartManager::MouserSearchService::partNumberFromUrl(""), std::string());
	}

	// One resistor, the shape the real endpoint returns on success (Errors present but empty).
	static std::string resistorResponse()
	{
		return R"({
			"Errors": [],
			"SearchResults": {
				"NumberOfResult": 1,
				"Parts": [{
					"MouserPartNumber": "603-RC0805FR-074K7L",
					"ManufacturerPartNumber": "RC0805FR-074K7L",
					"Manufacturer": "YAGEO",
					"Description": "Thick Film Resistors - SMD 4.7 kOhms 1% 0805",
					"Category": "Chip Resistor - Surface Mount",
					"DataSheetUrl": "https://www.mouser.ch/datasheet/2/447/rc-1664258.pdf",
					"ImagePath": "https://www.mouser.ch/images/yageo/images/RC_SERIES_t.jpg",
					"ProductDetailUrl": "https://www.mouser.ch/ProductDetail/YAGEO/RC0805FR-074K7L",
					"Availability": "5000 In Stock",
					"ROHSStatus": "RoHS Compliant",
					"ProductAttributes": [
						{ "AttributeName": "Resistance", "AttributeValue": "4.7 kOhms" },
						{ "AttributeName": "Tolerance", "AttributeValue": "±1%" },
						{ "AttributeName": "Power Rating", "AttributeValue": "125 mWatts" },
						{ "AttributeName": "Package / Case", "AttributeValue": "0805" },
						{ "AttributeName": "Operating Temperature", "AttributeValue": "-55 C to +155 C" }
					],
					"PriceBreaks": [
						{ "Quantity": 1, "Price": "0,10 CHF", "Currency": "CHF" },
						{ "Quantity": 100, "Price": "0,02 CHF", "Currency": "CHF" }
					]
				}]
			}
		})";
	}

	TEST_FUNCTION(wellFormedResponseParsesAndMaps)
	{
		TEST_START;

		const PartManager::MouserSearchResult result =
			PartManager::MouserClient::parseSearchResponse(resistorResponse());
		TEST_ASSERT_M(result.ok, result.errorMessage.c_str());
		TEST_COMPARE(result.numberOfResults, 1);
		TEST_COMPARE(result.parts.size(), static_cast<size_t>(1));

		const PartManager::MouserPartDto& dto = result.parts[0];
		TEST_COMPARE(dto.manufacturer, std::string("YAGEO"));
		TEST_COMPARE(dto.manufacturerPartNumber, std::string("RC0805FR-074K7L"));
		TEST_COMPARE(dto.mouserPartNumber, std::string("603-RC0805FR-074K7L"));
		TEST_COMPARE(dto.productAttributes.size(), static_cast<size_t>(5));
		TEST_COMPARE(dto.priceBreaks.size(), static_cast<size_t>(2));
		TEST_COMPARE(dto.priceBreaks[1].quantity, 100);

		const PartManager::MouserPartPrefill prefill = PartManager::MouserSearchService::toPrefill(dto);
		TEST_COMPARE(prefill.part.name, std::string("RC0805FR-074K7L"));
		TEST_COMPARE(prefill.part.mpn, std::string("RC0805FR-074K7L"));
		TEST_COMPARE(prefill.part.manufacturer, std::string("YAGEO"));
		TEST_COMPARE(prefill.part.package, std::string("0805"));
		TEST_COMPARE(prefill.suggestedTypeName, std::string("Resistor"));
		// §6: this is the URL every "Open on Mouser" button opens.
		TEST_COMPARE(prefill.productDetailUrl,
			std::string("https://www.mouser.ch/ProductDetail/YAGEO/RC0805FR-074K7L"));
		TEST_ASSERT(!prefill.datasheetUrl.empty());
		// The product photo becomes a role='image' attachment, which is what the part table
		// paints as a thumbnail. Losing it here is invisible until someone looks at the table.
		// Note the `images` -> `lrg` upgrade: the fixture holds the URL exactly as the API sends
		// it, and previewImageUrl() is what turns it into the one worth downloading.
		TEST_COMPARE(prefill.imageUrl,
			std::string("https://www.mouser.ch/images/yageo/lrg/RC_SERIES_t.jpg"));
		// partTypeId is never guessed here — resolving the name to a row is the caller's job.
		TEST_COMPARE(prefill.part.partTypeId, 0);
		// Package / Case and Operating Temperature are not part_type_attribute keys.
		TEST_COMPARE(prefill.unmappedAttributes.size(), static_cast<size_t>(2));
	}

	TEST_FUNCTION(inBodyErrorArrayIsAFailure)
	{
		TEST_START;

		// The trap the spec documents: HTTP 200 with the failure inside the body.
		const std::string body = R"({
			"Errors": [{
				"Id": 0,
				"Code": "InvalidAuthorization",
				"Message": "Invalid unique identifier.",
				"PropertyName": "apiKey"
			}],
			"SearchResults": null
		})";

		const PartManager::MouserSearchResult result = PartManager::MouserClient::parseSearchResponse(body);
		TEST_ASSERT_M(!result.ok, "Errors[] inside a 200 must not be reported as success");
		TEST_COMPARE(result.errorMessage, std::string("Invalid unique identifier."));
		TEST_ASSERT(result.parts.empty());
	}

	TEST_FUNCTION(emptyResultSetIsNotAnError)
	{
		TEST_START;

		const std::string body = R"({"Errors":[],"SearchResults":{"NumberOfResult":0,"Parts":[]}})";
		const PartManager::MouserSearchResult result = PartManager::MouserClient::parseSearchResponse(body);
		TEST_ASSERT_M(result.ok, "a search that matched nothing succeeded, it just found nothing");
		TEST_COMPARE(result.numberOfResults, 0);
		TEST_ASSERT(result.parts.empty());
		TEST_ASSERT(result.errorMessage.empty());
	}

	TEST_FUNCTION(malformedBodyIsRejected)
	{
		TEST_START;

		// Truncated mid-object, e.g. a connection dropped while streaming.
		const std::string truncated = R"({"Errors":[],"SearchResults":{"NumberOfResult":1,"Parts":[{"Manufa)";
		PartManager::MouserSearchResult result = PartManager::MouserClient::parseSearchResponse(truncated);
		TEST_ASSERT(!result.ok);
		TEST_ASSERT(!result.errorMessage.empty());

		result = PartManager::MouserClient::parseSearchResponse("");
		TEST_ASSERT(!result.ok);

		result = PartManager::MouserClient::parseSearchResponse("<html>502 Bad Gateway</html>");
		TEST_ASSERT(!result.ok);
	}

	TEST_FUNCTION(attributeValuesBecomeBaseSi)
	{
		TEST_START;

		const PartManager::MouserSearchResult result =
			PartManager::MouserClient::parseSearchResponse(resistorResponse());
		TEST_ASSERT(result.ok);
		const PartManager::MouserPartPrefill prefill =
			PartManager::MouserSearchService::toPrefill(result.parts[0]);

		// §2a shape, base-SI values, declared dropdown unit — "4.7 kOhms" is stored as 4700, not 4.7.
		const std::string expected =
			"{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"},"
			"\"tolerance\":{\"value\":1,\"unit\":\"%\"},"
			"\"power\":{\"value\":0.125,\"unit\":\"W\"}}";
		TEST_COMPARE(prefill.part.attributes, expected);

		// The word-unit rewrite is what lets ValueParser see a unit it knows.
		TEST_COMPARE(PartManager::MouserSearchService::normalizeUnitWords("4.7 kOhms"),
			std::string("4.7 k\xCE\xA9"));
		TEST_COMPARE(PartManager::MouserSearchService::normalizeUnitWords("\xC2\xB1""1%"), std::string("1%"));

		// A compound value keeps the longest prefix that parses, never the bare leading number.
		std::vector<std::string> unmapped;
		std::vector<PartManager::MouserProductAttribute> attributes;
		attributes.push_back({ "Capacitance", "100 nF" });
		attributes.push_back({ "Voltage Rating DC", "50 Volts" });
		attributes.push_back({ "Inductance", "10 uHenries 20%" });
		attributes.push_back({ "Colour", "Yellow" });
		attributes.push_back({ "Resistance", "see datasheet" });
		const std::string json = PartManager::MouserSearchService::attributesJson(attributes, unmapped);
		TEST_COMPARE(json, std::string(
			"{\"capacitance\":{\"value\":1e-07,\"unit\":\"F\"},"
			"\"voltage\":{\"value\":50,\"unit\":\"V\"},"
			"\"inductance\":{\"value\":1e-05,\"unit\":\"H\"}}"));
		// An unknown name and a known name with an unreadable value both go to manual entry.
		TEST_COMPARE(unmapped.size(), static_cast<size_t>(2));
		TEST_COMPARE(unmapped[0], std::string("Colour"));
		TEST_COMPARE(unmapped[1], std::string("Resistance"));

		std::vector<std::string> none;
		TEST_COMPARE(PartManager::MouserSearchService::attributesJson({}, none), std::string("{}"));
	}

	TEST_FUNCTION(categoryMappingRefusesToGuess)
	{
		TEST_START;

		typedef PartManager::MouserSearchService Service;
		TEST_COMPARE(Service::suggestedTypeName("Chip Resistor - Surface Mount"), std::string("Resistor"));
		TEST_COMPARE(Service::suggestedTypeName("Ceramic Capacitors"), std::string("Ceramic Capacitor"));
		TEST_COMPARE(Service::suggestedTypeName("Aluminum Electrolytic Capacitors"), std::string("Capacitor"));
		TEST_COMPARE(Service::suggestedTypeName("Fixed Inductors"), std::string("Inductor"));
		TEST_COMPARE(Service::suggestedTypeName("Switching Voltage Regulators"), std::string("Power Regulator"));
		TEST_COMPARE(Service::suggestedTypeName("Bipolar Transistors - BJT"), std::string("Transistor"));
		// MOSFET wins over the Transistor parent it would otherwise also match.
		TEST_COMPARE(Service::suggestedTypeName("MOSFET"), std::string("MOSFET"));
		TEST_COMPARE(Service::suggestedTypeName("Transistors - MOSFET"), std::string("MOSFET"));

		// The .claude/DefaultParts.csv categories, now that Diode/LED/Sensor are seeded types.
		TEST_COMPARE(Service::suggestedTypeName("Schottky Diodes & Rectifiers"), std::string("Diode"));
		TEST_COMPARE(Service::suggestedTypeName("Small Signal Switching Diodes"), std::string("Diode"));
		TEST_COMPARE(Service::suggestedTypeName("Single Colour LEDs"), std::string("LED"));
		TEST_COMPARE(Service::suggestedTypeName("Standard LEDs - SMD"), std::string("LED"));
		TEST_COMPARE(Service::suggestedTypeName("Board Mount Hall Effect/Magnetic Sensors"), std::string("Sensor"));
		// "led" as three letters inside another word must not make an LED out of an inductor.
		TEST_COMPARE(Service::suggestedTypeName("Shielded Inductors"), std::string("Inductor"));
		TEST_COMPARE(Service::suggestedTypeName("Coupled Chokes"), std::string());

		// Still no template: no type beats a wrong type.
		TEST_COMPARE(Service::suggestedTypeName("Rectangular Connectors - Headers"), std::string());
		TEST_COMPARE(Service::suggestedTypeName(""), std::string());
	}

	// The live 595-LM358DR case: the part itself and its packaging variant come back together.
	TEST_FUNCTION(closestMatchIsRankedFirst)
	{
		TEST_START;

		auto part = [](const char* mouser, const char* mpn)
		{
			PartManager::MouserPartDto dto;
			dto.mouserPartNumber = mouser;
			dto.manufacturerPartNumber = mpn;
			return dto;
		};

		std::vector<PartManager::MouserPartDto> parts = {
			part("595-LM358DRE4", "LM358DRE4"),
			part("511-LM358APT",  "LM358APT"),
			part("595-LM358DR",   "LM358DR"),
		};
		PartManager::MouserSearchService::rankByMatch(parts, "595-LM358DR");
		TEST_COMPARE(parts[0].mouserPartNumber, std::string("595-LM358DR"));
		TEST_COMPARE(parts[1].mouserPartNumber, std::string("595-LM358DRE4"));

		// Same set searched by manufacturer part number ranks off the MPN field instead.
		PartManager::MouserSearchService::rankByMatch(parts, "lm358dr");
		TEST_COMPARE(parts[0].manufacturerPartNumber, std::string("LM358DR"));
		TEST_COMPARE(parts[1].manufacturerPartNumber, std::string("LM358DRE4"));

		// Nothing matches, or nothing to match on: order is left exactly as Mouser sent it.
		const std::string firstBefore = parts[0].mouserPartNumber;
		PartManager::MouserSearchService::rankByMatch(parts, "");
		TEST_COMPARE(parts[0].mouserPartNumber, firstBefore);
		PartManager::MouserSearchService::rankByMatch(parts, "BC547");
		TEST_COMPARE(parts[0].mouserPartNumber, firstBefore);
	}

	// Every description below is a **real** one, fetched from the live API on 2026-09-03 for a
	// part in the user's own stock list. Invented ones would prove nothing here: the whole reason
	// this parser exists is that Mouser's structured attributes are empty, so the only contract
	// worth testing against is the prose they actually ship.
	TEST_FUNCTION(descriptionsFillTheTemplatesSlots)
	{
		TEST_START;
		using S = PartManager::MouserSearchService;

		std::string package;
		const std::string mlcc = S::attributesFromDescription("Ceramic Capacitor",
			"Multilayer Ceramic Capacitors MLCC - SMD/SMT",
			"Multilayer Ceramic Capacitors MLCC - SMD/SMT 100nF+/-10% 25V X7R 0402", &package);
		TEST_ASSERT_M(mlcc.find("\"capacitance\":{\"value\":1e-07") != std::string::npos, mlcc);
		TEST_ASSERT_M(mlcc.find("\"tolerance\":{\"value\":10") != std::string::npos, mlcc);
		TEST_ASSERT_M(mlcc.find("\"voltage\":{\"value\":25") != std::string::npos, mlcc);
		TEST_ASSERT_M(mlcc.find("\"dielectric\":\"X7R\"") != std::string::npos, mlcc);
		TEST_COMPARE(package, std::string("0402"));

		// "1/10watts" — a fraction and a word unit in one token, and "10K" with the ohm left out
		// as understood, which is the SI-prefix case the primary slot exists for.
		package.clear();
		const std::string resistor = S::attributesFromDescription("Resistor",
			"Thin Film Resistors - SMD", "Thin Film Resistors - SMD 1/10watts 10K .1%", &package);
		TEST_ASSERT_M(resistor.find("\"power\":{\"value\":0.1") != std::string::npos, resistor);
		TEST_ASSERT_M(resistor.find("\"resistance\":{\"value\":10000") != std::string::npos, resistor);
		TEST_ASSERT_M(resistor.find("\"tolerance\":{\"value\":0.1") != std::string::npos, resistor);
		TEST_ASSERT_M(package.empty(), package);

		// Only Vds is measured in volts on a MOSFET, so the bare "60V" is placeable. There is no
		// power slot on the template, so "200mW" has nowhere to go and is dropped, not forced.
		const std::string mosfet = S::attributesFromDescription("MOSFET", "MOSFETs",
			"MOSFETs 60V 200mW");
		TEST_ASSERT_M(mosfet.find("\"vds_max\":{\"value\":60") != std::string::npos, mosfet);
		TEST_ASSERT_M(mosfet.find("power") == std::string::npos, mosfet);

		const std::string inductor = S::attributesFromDescription("Inductor",
			"Power Inductors - SMD", "Power Inductors - SMD 5uH 30% SMD 1038");
		TEST_ASSERT_M(inductor.find("\"inductance\":{\"value\":5e-06") != std::string::npos, inductor);

		const std::string regulator = S::attributesFromDescription("Power Regulator",
			"Switching Voltage Regulators",
			"Switching Voltage Regulators 4.5A, 500kHz Buck Sw Reg");
		TEST_ASSERT_M(regulator.find("\"max_current\":{\"value\":4.5") != std::string::npos, regulator);
		TEST_ASSERT_M(regulator.find("\"regulator_type\":\"Switching\"") != std::string::npos, regulator);

		package.clear();
		const std::string led = S::attributesFromDescription("LED", "Single Colour LEDs",
			"Single Colour LEDs WL-SMCW SMDMono TpVw Waterclr 1206 Yellow", &package);
		TEST_ASSERT_M(led.find("\"color\":\"Yellow\"") != std::string::npos, led);
		TEST_COMPARE(package, std::string("1206"));
	}

	TEST_FUNCTION(anAmbiguousDescriptionTokenIsLeftAlone)
	{
		TEST_START;
		using S = PartManager::MouserSearchService;

		// A diode measures forward *and* reverse voltage in volts, so no bare voltage token can
		// be placed on one. Filling either would be a plausible wrong number, which is worse than
		// the empty field the user then types into.
		const std::string diode = S::attributesFromDescription("Diode",
			"Small Signal Switching Diodes", "Small Signal Switching Diodes 75V 300mA");
		TEST_ASSERT_M(diode.find("voltage") == std::string::npos, diode);
		// The current slot is unambiguous, so that one is placed.
		TEST_ASSERT_M(diode.find("\"forward_current\":{\"value\":0.3") != std::string::npos, diode);

		// Prose with no numbers in it at all — the normal case for a semiconductor.
		TEST_COMPARE(S::attributesFromDescription("Diode", "Small Signal Switching Diodes",
			"Small Signal Switching Diodes Hi Conductance Fast"), std::string("{}"));

		// A type with no slot table (an IC) reads nothing, but the package is still worth having.
		std::string package;
		TEST_COMPARE(S::attributesFromDescription("", "LED Drivers",
			"LED Drivers 16ch 0805 Const Current Sink", &package), std::string("{}"));
		TEST_COMPARE(package, std::string("0805"));

		// "1038" is a Bourns case code, four digits like every chip size and not one of them.
		package.clear();
		S::attributesFromDescription("Inductor", "Power Inductors - SMD",
			"Power Inductors - SMD 5uH 30% SMD 1038", &package);
		TEST_ASSERT_M(package.empty(), package);
	}

	// Every MPN here is one the user actually stocks. The negatives matter more than the
	// positives: this runs on every imported part, and a coincidence in a long digit group would
	// fill the Gehäuse field with a wrong size that looks exactly like a right one.
	TEST_FUNCTION(thePartNumberIsTheLastSourceOfAPackage)
	{
		TEST_START;
		using S = PartManager::MouserSearchService;

		TEST_COMPARE(S::packageFromPartNumber("CRT0603-BY-1002ELF"), std::string("0603"));
		TEST_COMPARE(S::packageFromPartNumber("CR0805-FX-5102ELF"), std::string("0805"));
		TEST_COMPARE(S::packageFromPartNumber("CR0603-JW-202ELF"), std::string("0603"));

		// Digit runs are compared whole. "7002" and "4448" are not chip sizes, "1038" is a Bourns
		// case code, and the Wurth and Samsung numbers are one long run with no size in them.
		TEST_COMPARE(S::packageFromPartNumber("2N7002-7-F"), std::string());
		TEST_COMPARE(S::packageFromPartNumber("1N4448"), std::string());
		TEST_COMPARE(S::packageFromPartNumber("SRU1038-5R0Y"), std::string());
		TEST_COMPARE(S::packageFromPartNumber("885012207054"), std::string());
		TEST_COMPARE(S::packageFromPartNumber("150120YS75000"), std::string());
		TEST_COMPARE(S::packageFromPartNumber("CL05B104KA5NNNC"), std::string());
		TEST_COMPARE(S::packageFromPartNumber(""), std::string());
	}
};

TEST_INSTANTIATE(TST_MouserSearchService);
