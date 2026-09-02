// @file PartManager_EasyEdaClient.h
// @brief EasyEDA/LCSC component API client (§5c) — the one ECAD source that needs no account.
//
// **Why EasyEDA and not the provider Mouser's own page links to.** The ECAD
// button on a Mouser product page calls SamacSys / Component Search Engine,
// which publishes no API at all; SnapEDA/SnapMagic gates theirs behind a
// request form. Neither is unlocked by a login — an account there buys a
// browser session, not a download endpoint — so no credential store would have
// helped. EasyEDA's product API is open, keyless and returns symbol *and*
// footprint geometry, which is why it is the source here.
//
// Two endpoints, both plain GETs:
//
//     /api/eda/product/search?version=6.5.36&keyword=<mpn>&needAggs=false
//     /api/products/<lcscCode>/components?version=6.4.19.5
//
// **A User-Agent is required.** The API sits behind CloudFront, which answers
// the bare `Mozilla/5.0` Qt sends by default with HTTP 403. Any other value is
// accepted — measured 2026-09-02, `PartManager/1.0`, an empty UA and a full
// Chrome UA all returned 200 while `Mozilla/5.0` alone did not. So the header is
// set explicitly, and honestly: PartManager names itself rather than pretending
// to be a browser.
//
// Coverage is LCSC's catalogue, which is the real limit of this route: `LM358DR`
// finds 19 candidates, the Würth LED `150120YS75000` finds none. A miss is a
// normal outcome, not an error — the caller falls back to a vendor ZIP
// (EcadArchive).
//
// Parsing is split from the HTTP call, so the whole shape mapping is testable
// from a fixed JSON string with no network. The DTOs are plain C++ and always
// available; parsing and HTTP need QtCore/QtNetwork (never QtWidgets, §12a).
// @see docs/design/ARCHITECTURE.md §5c, §12a
// @see PartManager_EasyEdaConverter.h, PartManager_EcadArchive.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

#if QT_ENABLED
class QNetworkAccessManager;
#endif

namespace PartManager
{

	// One row of a product search. `lcscCode` ("C7950") is what the component endpoint takes.
	struct PART_MANAGER_API EasyEdaSearchHit
	{
		std::string lcscCode;
		std::string mpn;
		std::string manufacturer;
		std::string package;
	};

	struct PART_MANAGER_API EasyEdaSearchResult
	{
		bool ok = false;
		std::string errorMessage;
		int total = 0;                          // may exceed hits.size(); the API pages
		std::vector<EasyEdaSearchHit> hits;
	};

	// One component's raw EasyEDA geometry, still in EasyEDA's own tilde-delimited shape lines.
	// Kept unparsed here on purpose: this class knows the API, EasyEdaConverter knows the format,
	// and keeping the split means a shape kind can be added without touching the network code.
	struct PART_MANAGER_API EasyEdaComponent
	{
		bool ok = false;
		std::string errorMessage;

		std::string lcscCode;
		std::string title;                      // EasyEDA's own name for the part
		std::string manufacturer;
		std::string mpn;
		std::string packageName;                // "SOIC-8_L5.0-W4.0-P1.27-LS6.0-BL"
		std::string referencePrefix;            // c_para.pre, e.g. "U?" — the "?" is stripped

		// Schematic symbol. EasyEDA schematic units are 10 mil; the origin is the head x/y.
		std::vector<std::string> symbolShapes;
		double symbolOriginX = 0.0;
		double symbolOriginY = 0.0;

		// PCB footprint. EasyEDA PCB units are 1 mil; the origin is again the head x/y.
		std::vector<std::string> footprintShapes;
		double footprintOriginX = 0.0;
		double footprintOriginY = 0.0;

		bool hasSymbol() const { return !symbolShapes.empty(); }
		bool hasFootprint() const { return !footprintShapes.empty(); }
	};

	class PART_MANAGER_API EasyEdaClient
	{
	public:
		// The User-Agent every request carries. Not a disguise — see the header note; it exists
		// because CloudFront rejects Qt's default `Mozilla/5.0`.
		static const char* const UserAgent;

		// Pure response handling, no network — the offline-testable half.
		static EasyEdaSearchResult parseSearchResponse(const std::string& json);
		static EasyEdaComponent parseComponentResponse(const std::string& json);

		// The hit worth fetching for a part, or "" when none is convincing enough to attach
		// without asking. An exact manufacturer-part-number match wins; among several, a matching
		// manufacturer breaks the tie. **A near-miss is never accepted**: "LM358DRG" is a
		// different part from "LM358DR", and silently attaching its footprint would put a wrong
		// land pattern on a board, which is worse than attaching nothing at all.
		static std::string bestMatch(const EasyEdaSearchResult& result, const std::string& mpn,
			const std::string& manufacturer);

		// Case- and separator-insensitive part-number comparison: "LM358-DR" == "lm358dr".
		// Mouser and LCSC do not agree on punctuation inside a part number.
		static bool samePartNumber(const std::string& left, const std::string& right);

		EasyEdaClient();
		~EasyEdaClient();
		EasyEdaClient(const EasyEdaClient&) = delete;
		EasyEdaClient& operator=(const EasyEdaClient&) = delete;

		void setTimeoutMs(int timeoutMs);
		int timeoutMs() const;

		EasyEdaSearchResult search(const std::string& keyword);
		EasyEdaComponent fetch(const std::string& lcscCode);

		// search() + bestMatch() + fetch() in one step — what the import flow calls. A part LCSC
		// does not carry comes back with ok == false and an errorMessage saying so, which is a
		// normal outcome rather than a failure worth a dialog.
		EasyEdaComponent lookup(const std::string& mpn, const std::string& manufacturer);

	private:
		std::string get(const std::string& url, bool& outOk, std::string& outError);

		int m_timeoutMs = 15000;
#if QT_ENABLED
		QNetworkAccessManager* m_network = nullptr;
#endif
	};

}
