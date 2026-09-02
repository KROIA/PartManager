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
	}

private:

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
		TEST_COMPARE(prefill.imageUrl,
			std::string("https://www.mouser.ch/images/yageo/images/RC_SERIES_t.jpg"));
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
};

TEST_INSTANTIATE(TST_MouserSearchService);
