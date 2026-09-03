// @file PartManager_MouserSearchService.h
// @brief MouserPart DTO -> domain Part prefill (§6). Pure mapping, no Qt, no network, no SQL.
//
// Everything here is a pure function of a `MouserPartDto`, so the whole mapping
// is unit-testable from a fixed JSON fixture without a key or a network call.
//
// Parametric values go through `ValueParser` (never a second hand-rolled
// parser) and land in the §2a `attributes` shape
// `{"key": {"value": <base-SI number>, "unit": "<dropdown unit>"}}` — always
// base-SI, never the prefix the datasheet happened to use.
//
// Category -> type template mapping is deliberately conservative: only the
// unambiguous cases are mapped and everything else comes back with an empty
// `suggestedTypeName`, so a wrong template is never silently attached to a part.
// @see docs/design/ARCHITECTURE.md §2a, §6
// @see PartManager_MouserClient.h, PartManager_ValueParser.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_Part.h"
#include "domain/PartManager_Seller.h"
#include "mouser/PartManager_MouserClient.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One Mouser search row turned into "everything the New Part form should start out holding".
	// Part alone cannot carry this: it stores a datasheet as a file id, not a URL, and has no
	// field for the Mouser product page at all.
	struct PART_MANAGER_API MouserPartPrefill
	{
		Part part;                         // partTypeId stays 0 whenever the category could not be mapped
		std::string suggestedTypeName;     // 'MOSFET', 'Resistor', ...; empty = undecidable, leave it to the user
		std::string datasheetUrl;          // MouserPart.DataSheetUrl, for the §6 auto-download step
		std::string imageUrl;              // MouserPart.ImagePath — the product photo, downloaded as a role='image' attachment
		std::string productDetailUrl;      // MouserPart.ProductDetailUrl — what "Open on Mouser" opens
		std::string mouserPartNumber;      // the Cart API's article number (§6); not a Part field
		// The quote as seen, one entry per price break, ready for `price_history(source=
		// 'mouser_api_quote')`. Carried through the prefill because the part does not exist yet
		// at search time — the observation can only be written once there is a link to hang it on.
		std::vector<PriceObservation> priceBreaks;
		std::vector<std::string> unmappedAttributes; // Mouser AttributeNames left for manual entry
	};

	class PART_MANAGER_API MouserSearchService
	{
		MouserSearchService() = delete;
	public:
		// Full DTO -> prefill mapping. Never fails; unmappable pieces are simply left empty.
		static MouserPartPrefill toPrefill(const MouserPartDto& dto);

		// Reorders a result set so the closest match to what the user typed comes first.
		// Mouser answers "595-LM358DR" with the part itself *and* its packaging variants
		// ("595-LM358DRE4"): exact beats prefix beats substring, and among equals the
		// candidate carrying the fewest extra characters wins. Stable, so Mouser's own
		// relevance order survives inside a tier. Both the Mouser and the manufacturer
		// part number are scored; the better of the two counts.
		static void rankByMatch(std::vector<MouserPartDto>& parts, const std::string& query);

		// Mouser's `Category` -> one of the seeded type template names, or "" when ambiguous.
		static std::string suggestedTypeName(const std::string& category);

		// The §2a attributes a part's free-text `Description` yields, plus the package code in it.
		//
		// **This, not attributesJson(), is what fills a form in practice.** Measured 2026-09-03
		// over all 38 rows of the user's stock list: `ProductAttributes` holds `Packaging` and
		// `Standard Pack Qty` and nothing else, on both search endpoints — so the AttributeName
		// map has no parametrics to map. The values are in the description tail:
		// `"... MLCC - SMD/SMT 100nF+/-10% 25V X7R 0402"`.
		//
		// `typeName` is suggestedTypeName()'s answer, because the slots a token may land in are a
		// property of the template, not of Mouser's category string. A token is written only when
		// exactly one of that type's slots can read it — so "60V" fills a MOSFET's Vds and is
		// refused on a diode, which measures both forward and reverse voltage in volts. The one
		// exception is a unit-less number carrying an SI prefix ("10K"), which goes to the type's
		// primary quantity; a plain integer never does, since "1206" reads as a value too.
		// Returns "{}" when nothing is placeable, which is the normal answer for an IC.
		static std::string attributesFromDescription(const std::string& typeName,
			const std::string& category, const std::string& description,
			std::string* outPackage = nullptr);

		// A pasted Mouser product page URL -> the part number in its path, "" for anything else.
		// `.../ProductDetail/<Manufacturer>/<PartNumber>?qs=...` on any of Mouser's country
		// domains; the query string and fragment are dropped. Feeding the result to
		// searchByPartNumber() is what turns "here is the link" into a filled-in form, which is
		// the shape the user's own stock list (`.claude/DefaultParts.csv`) is in.
		// ponytail: the path segment is used verbatim. Mouser writes 'LT1506CR-3.3PBF' for the
		// part whose real MPN is 'LT1506CR-3.3#PBF', so the lookup relies on the non-exact
		// part-number search still finding it; upgrade path is falling back to a keyword search.
		static std::string partNumberFromUrl(const std::string& url);

		// A datasheet URL for a part whose MouserPart.DataSheetUrl came back empty, "" when there
		// is no way to know one.
		//
		// **Mouser's own page cannot be used for this.** The `<a id="pdp-datasheet_0">` link is
		// right there in the product page markup, but www.mouser.* serves that page behind a
		// DataDome/Akamai JavaScript challenge: a request with HTTP/2, the full browser header set
		// and WinHTTP — the exact combination that gets the product *images* through — is answered
		// with HTTP 403 and a `geo.captcha-delivery.com` challenge stub (measured 2026-09-02).
		// Only the image CDN is ungated. Reading that link would take a JavaScript-executing
		// browser and would still be one CAPTCHA away from breaking, so it is not attempted.
		//
		// What is left is manufacturers who publish at a URL derivable from the part number. That
		// is a short list, and every entry in it is a URL that was fetched and confirmed to return
		// a PDF rather than a guess: TI, ST and Vishay all host real datasheets but under family
		// document names ("lm358.pdf" for LM358DR) that no rule recovers from an MPN.
		static std::string datasheetUrlFor(const std::string& manufacturer, const std::string& mpn);

		// MouserPart.ImagePath -> the same picture at the size the product page's zoom popup uses.
		// Mouser serves one image under four sibling paths that differ only in a single segment:
		// `/images/<vendor>/{sm|images|lrg|hd}/<file>`. The API always hands out the `images`
		// variant, which is a ~1.5 kB web thumbnail — too soft for the §7a preview panel. `lrg` is
		// the same picture around 8-20 kB. `hd` is bigger still but not always there: it 404s for
		// e.g. 511-STM32F103C8T6 where `lrg` answers, so the always-present one is the one used.
		// Scraping the product page HTML for the popup's <a href> yields this exact URL, so it
		// buys nothing but a second request through Mouser's bot filter and a regex against markup
		// they can restyle. Anything not in the four-segment shape comes back unchanged.
		// Note the file extension lies — every variant is served as image/webp regardless of the
		// `.JPG` in the name, which is FileStore::correctedFilename()'s job, not this one's.
		static std::string previewImageUrl(const std::string& imagePath);

		// Mouser's ProductAttributes[] -> the §2a attributes JSON, base-SI values only.
		// Attribute names we have no mapping for, and values ValueParser rejects, are reported
		// in outUnmapped instead of being guessed at. Returns "{}" when nothing mapped.
		static std::string attributesJson(const std::vector<MouserProductAttribute>& attributes,
			std::vector<std::string>& outUnmapped);

		// Mouser's PriceBreaks[] -> price observations. `Price` is a currency-formatted string
		// that **already contains the currency** ("0.21 CHF"), so the separate `Currency` field is
		// only a fallback — appending both is how the same currency ends up printed twice. Breaks
		// whose price does not parse are dropped rather than recorded as 0.
		static std::vector<PriceObservation> toPriceObservations(
			const std::vector<MouserPriceBreak>& breaks);

		// Mouser writes units as words ("4.7 kOhms", "10 Volts"). Rewrites them to the §2a
		// dropdown symbols so ValueParser sees a unit it knows. Public because it is the one
		// piece worth asserting on its own.
		static std::string normalizeUnitWords(const std::string& value);
	};

}
