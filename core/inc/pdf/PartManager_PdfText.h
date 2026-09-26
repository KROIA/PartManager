// @file PartManager_PdfText.h
// @brief Pulls the text out of a PDF, so a datasheet can be read rather than only stored (§14g).
//
// Nothing else in this project reads a PDF, and the alternatives were worse than
// writing one: a CAD-grade library (Poppler, PDFium) is a new third-party
// dependency for one feature, and a vision model needs a renderer this project
// also does not have. **Qt already carries zlib**, reachable through
// `qUncompress`, and `FlateDecode` is the only filter the text streams in real
// datasheets actually use — so the whole job is inflate plus a content-stream
// parser, with no new dependency at all.
//
// **Measured** (2026-09-26, the 27 datasheets in the developer's own
// `KicadFresh` filestore). A scan beforehand found 22 of 27 carrying inflatable
// `FlateDecode` streams with `BT`/`Tj`/`TJ` text operators; running the finished
// extractor over the same 27 gives **21 to text, 6 scans, 0 failures**. The two
// numbers agree: 22 files contain a text stream, 21 yield text worth reading —
// the odd one out has a 186-character text layer over a scanned page, which is
// a scan with a caption, and is reported as `looksScanned`.
//
// The filters across the corpus are `FlateDecode` (4333 streams), `DCTDecode`
// (26) and `CCITTFaxDecode` (6) — the last two are images, which is what a
// scanned page is. So roughly four datasheets in five are readable this way and
// the rest are pictures of text. **Nothing in the corpus failed to parse**, and
// none was encrypted or used a filter this does not implement.
//
// **This is a text extractor, not a layout engine.** It recovers words and their
// page, in content-stream order. It does *not* reconstruct columns, tables or
// reading order, and a two-column datasheet will interleave. That is a real
// limit and it is why `search()` returns a snippet with its page number rather
// than pretending to quote a table: the honest use is "find where it says this",
// not "read me the pinout as a grid".
//
// `looksScanned` exists so the failure is legible. A datasheet with no text
// operators and plenty of `DCTDecode`/`CCITTFaxDecode` image data is not a
// broken parse — it is a scan, nothing will get text out of it without OCR, and
// saying so is more use to a caller than an empty string.
// @see docs/design/ARCHITECTURE.md §14g
// @see PartManager_DatasheetToolset.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	struct PART_MANAGER_API PdfTextPage
	{
		int number = 0;        // 1-based, as a datasheet prints it
		std::string text;      // UTF-8, content-stream order
	};

	struct PART_MANAGER_API PdfTextResult
	{
		bool ok = false;
		std::string error;              // empty when ok
		int pageCount = 0;
		std::vector<PdfTextPage> pages; // only the pages actually extracted
		// True when the file carries image data and no text operators — a scan.
		// `ok` is still true in that case: the file was read, it simply has no text.
		bool looksScanned = false;

		// Everything joined, pages separated by a form feed. The convenience a
		// caller that does not care about pages wants; `search()` is what a
		// caller that does should use instead.
		std::string allText() const;
	};

	// One hit: enough surrounding text to be worth reading, and the page it is on.
	struct PART_MANAGER_API PdfTextMatch
	{
		int page = 0;
		std::string snippet;
	};

	class PART_MANAGER_API PdfText
	{
		PdfText() = delete;
	public:
		// `maxPages` 0 means every page. Never throws; a file that is not a PDF,
		// is encrypted, or uses a filter this does not implement comes back with
		// `ok == false` and an `error` that names which of those it was.
		static PdfTextResult extract(const std::string& filePath, int maxPages = 0);
		// Same, from bytes already in hand — which is what makes this testable
		// from a fixture without touching the filesystem.
		static PdfTextResult extractBytes(const std::string& bytes, int maxPages = 0);

		// Case-insensitive substring search across the extracted pages, returning
		// at most `maxMatches` snippets of about `contextChars` around each hit.
		// Separate from extraction because a datasheet runs to tens of pages and
		// a local model's context does not: handing back three snippets and their
		// page numbers is the difference between answering a question and filling
		// the window with a PDF.
		static std::vector<PdfTextMatch> search(const PdfTextResult& extracted,
			const std::string& needle, int maxMatches = 5, int contextChars = 400);
	};

}
