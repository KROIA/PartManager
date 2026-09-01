// @file PartManager_MouserClient.h
// @brief Mouser Search API v1 client (§6) — DTOs, offline response parsing, and the HTTP call.
//
// Grounded on `.claude/MouserAPI/MouserAPI_V1.json` (Swagger 2.0, host
// `api.mouser.com`): the key travels as the `apiKey` **query parameter**, the
// search body is a single-property envelope (`{"SearchByPartRequest":{...}}`),
// and failures come back inside a 200 response as `Errors[]` — so an HTTP 200
// alone never means success. `parseSearchResponse()` is deliberately split off
// from `searchByPartNumber()`/`searchByKeyword()` so response handling is
// unit-testable with a fixed JSON string and no network.
//
// The key is read at runtime from the `MOUSER_SEARCH_API` environment variable
// and from nowhere else — never a literal, never a settings file, never logged.
// When it is unset every request fails immediately with a message that does not
// contain the key or any part of it.
//
// The DTOs are plain C++ and always available; the parsing/HTTP half needs
// QtCore/QtNetwork and is gated behind QT_ENABLED (QtCore + QtNetwork only,
// never QtWidgets — §12a).
// @see docs/design/ARCHITECTURE.md §6, §12a
// @see PartManager_MouserSearchService.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

#if QT_ENABLED
class QNetworkAccessManager;
#endif

namespace PartManager
{

	// One `ProductAttribute` row — Mouser's own parametric spec table, e.g. { "Resistance", "4.7 kOhms" }.
	struct PART_MANAGER_API MouserProductAttribute
	{
		std::string name;   // AttributeName
		std::string value;  // AttributeValue
	};

	// One `Pricebreak` row. Price is a string in the spec (currency-formatted), kept verbatim.
	struct PART_MANAGER_API MouserPriceBreak
	{
		int quantity = 0;
		std::string price;
		std::string currency;
	};

	// One `MouserPart` from `SearchResults.Parts[]`. Only the fields §6 actually consumes are
	// carried; the spec's ~40 remaining fields (compliance, packaging, surcharges, ...) are ignored.
	struct PART_MANAGER_API MouserPartDto
	{
		std::string mouserPartNumber;
		std::string manufacturerPartNumber;
		std::string manufacturer;
		std::string description;
		std::string category;
		std::string dataSheetUrl;
		std::string productDetailUrl;   // the page every "Open on Mouser" button opens (§6)
		std::string imagePath;
		std::string lifecycleStatus;
		std::string availability;
		std::string availabilityInStock;
		std::string rohsStatus;
		std::vector<MouserProductAttribute> productAttributes;
		std::vector<MouserPriceBreak> priceBreaks;
	};

	// Outcome of one search. `ok == false` covers all four failure modes uniformly: missing key,
	// network/timeout, non-200 status, and Mouser's in-body `Errors[]` inside a 200.
	struct PART_MANAGER_API MouserSearchResult
	{
		bool ok = false;
		std::string errorMessage;   // never contains the API key
		int numberOfResults = 0;    // SearchResults.NumberOfResult, may exceed parts.size() when paged
		std::vector<MouserPartDto> parts;
	};

	class PART_MANAGER_API MouserClient
	{
	public:
		// The only place the key ever comes from.
		static const char* const ApiKeyEnvVar;

		// True when MOUSER_SEARCH_API is set and non-empty. Does not expose the value.
		static bool hasApiKey();

		// Pure response handling, no network, no key — the offline-testable half.
		// Handles: malformed/truncated body, Errors[] inside a 200, and an empty result set
		// (which is ok == true with zero parts, not an error).
		static MouserSearchResult parseSearchResponse(const std::string& json);

		MouserClient();
		~MouserClient();
		MouserClient(const MouserClient&) = delete;
		MouserClient& operator=(const MouserClient&) = delete;

		// Per-request timeout in milliseconds. Default 15000.
		void setTimeoutMs(int timeoutMs);
		int timeoutMs() const;

		// POST /api/v1/search/partnumber. `partNumber` may be a Mouser part number or a
		// manufacturer part number; up to 10 may be passed pipe-separated, per the spec.
		// `exactMatch` sets partSearchOptions to "Exact" instead of the default "None".
		MouserSearchResult searchByPartNumber(const std::string& partNumber, bool exactMatch = false);

		// POST /api/v1/search/keyword. `records`/`startingRecord` page the result set;
		// the spec caps one response at 50 parts.
		MouserSearchResult searchByKeyword(const std::string& keyword, int records = 10, int startingRecord = 0);

	private:
		// Shared POST path for both endpoints. `endpointPath` is relative to the API root,
		// e.g. "search/partnumber". Never logs or returns the key.
		MouserSearchResult post(const std::string& endpointPath, const std::string& jsonBody);

		int m_timeoutMs = 15000;
#if QT_ENABLED
		QNetworkAccessManager* m_network = nullptr;
#endif
	};

}
