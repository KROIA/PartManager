#pragma once

#include "UnitTest.h"
#include "pdf/PartManager_PdfText.h"

#include <QByteArray>
#include <string>
#include <vector>

// The §14g PDF text extractor, against PDFs this file builds byte by byte.
//
// **The fixtures are hand-written PDFs, not files on disk.** The corpus that shaped this feature
// is `C:\Users\KRIA\Documents\PartManager\KicadFresh\filestore` — the user's own datasheets — and
// a suite that read them would pass here and fail on every other machine (ORIENTATION §6, and the
// same reason TST_LlmKicadToolset embeds its vendor files rather than reading `.claude/`). So each
// case assembles the smallest PDF that carries the structure under test: an uncompressed content
// stream, a `FlateDecode` one, a filter *chain*, a page tree hidden inside an `/ObjStm`, a page
// that is nothing but a `DCTDecode` image, a `/ToUnicode` CMap, a file that is not a PDF, and one
// whose trailer says `/Encrypt`.
//
// The `FlateDecode` streams are real deflate: `qCompress` produces a zlib stream with a 4-byte
// size header in front, and dropping those four bytes leaves exactly what a PDF `/FlateDecode`
// carries. That is the same fact `flateDecode()` in the extractor depends on from the other side.
// @see PartManager_PdfText.h, docs/design/ARCHITECTURE.md §14g
// @see TST_LlmDatasheetToolset.h

// Minimal PDF construction, shared with TST_LlmDatasheetToolset. Free functions rather than
// members of the suite so the toolset cases can attach the same bytes to a part without a second
// copy of the builder drifting away from this one.
namespace PdfFixtures
{
	// Objects are written in the order they are added and numbered from 1, so a fixture that
	// needs a forward reference works out the number itself. There is no cross-reference table:
	// PdfText scans for `N G obj` rather than trusting one, which is what lets it read the
	// incrementally-updated and slightly-damaged files a real filestore is full of.
	struct Builder
	{
		std::vector<std::string> bodies;

		int add(const std::string& body)
		{
			bodies.push_back(body);
			return static_cast<int>(bodies.size());
		}

		int addStream(const std::string& dictInner, const std::string& data)
		{
			const std::string body = "<< " + dictInner + " /Length "
				+ std::to_string(data.size()) + " >>\nstream\n" + data + "\nendstream";
			return add(body);
		}

		std::string build(int rootObject, const std::string& trailerExtra = std::string()) const
		{
			std::string out = "%PDF-1.7\n";
			for (size_t i = 0; i < bodies.size(); ++i)
			{
				out += std::to_string(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
			}
			out += "trailer\n<< /Size " + std::to_string(bodies.size() + 1) + " /Root "
				+ std::to_string(rootObject) + " 0 R " + trailerExtra + ">>\n%%EOF\n";
			return out;
		}
	};

	// The raw zlib stream a `/FlateDecode` carries: qCompress's output minus its own 4-byte
	// uncompressed-size header, which no PDF has.
	inline std::string deflated(const std::string& text)
	{
		const QByteArray compressed =
			qCompress(QByteArray(text.data(), static_cast<int>(text.size())), 9);
		const QByteArray raw = compressed.mid(4);
		return std::string(raw.constData(), static_cast<size_t>(raw.size()));
	}

	inline std::string hexOf(const std::string& bytes)
	{
		static const char* const digits = "0123456789ABCDEF";
		std::string out;
		for (const char c : bytes)
		{
			const unsigned char value = static_cast<unsigned char>(c);
			out += digits[value >> 4];
			out += digits[value & 0x0F];
		}
		out += '>';
		return out;
	}

	// N pages, one content stream each, sharing one Helvetica font that declares no encoding —
	// so the WinAnsi fallback is what decodes every byte.
	inline std::string textPdf(const std::vector<std::string>& contents, bool compress)
	{
		const int pageCount = static_cast<int>(contents.size());
		const int fontObject = 3 + 2 * pageCount;
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		std::string kids;
		for (int i = 0; i < pageCount; ++i)
		{
			kids += std::to_string(3 + i) + " 0 R ";
		}
		builder.add("<< /Type /Pages /Count " + std::to_string(pageCount) + " /Kids [ " + kids
			+ "] >>");
		for (int i = 0; i < pageCount; ++i)
		{
			builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << "
				"/Font << /F1 " + std::to_string(fontObject) + " 0 R >> >> /Contents "
				+ std::to_string(3 + pageCount + i) + " 0 R >>");
		}
		for (int i = 0; i < pageCount; ++i)
		{
			if (compress)
			{
				builder.addStream("/Filter /FlateDecode", deflated(contents[i]));
			}
			else
			{
				builder.addStream(std::string(), contents[i]);
			}
		}
		builder.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
		return builder.build(1);
	}

	inline std::string onePageContent()
	{
		return "BT\n/F1 12 Tf\n72 720 Td\n(Absolute Maximum Ratings) Tj\n"
			"0 -14 Td\n(Supply voltage 32 V) Tj\nET\n";
	}

	inline std::string secondPageContent()
	{
		return "BT\n/F1 12 Tf\n72 720 Td\n(Thermal resistance junction to ambient) Tj\nET\n";
	}

	inline std::string uncompressedPdf()
	{
		return textPdf(std::vector<std::string>(1, onePageContent()), false);
	}

	inline std::string flatePdf()
	{
		return textPdf(std::vector<std::string>(1, onePageContent()), true);
	}

	inline std::string twoPageFlatePdf()
	{
		std::vector<std::string> pages;
		pages.push_back(onePageContent());
		pages.push_back(secondPageContent());
		return textPdf(pages, true);
	}

	// `[/ASCIIHexDecode /FlateDecode]` — a chain, applied in array order, which is the shape a
	// producer that wanted a 7-bit-safe file writes.
	inline std::string chainedFilterPdf()
	{
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
			"<< /F1 5 0 R >> >> /Contents 4 0 R >>");
		builder.addStream("/Filter [ /ASCIIHexDecode /FlateDecode ]",
			hexOf(deflated(onePageContent())));
		builder.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
		return builder.build(1);
	}

	// Catalogue, page tree and page dictionary inside a compressed `/ObjStm`, which is how every
	// PDF written this decade stores them — and what a scan that only looks for `N G obj` at file
	// level finds nothing in.
	inline std::string objectStreamPdf()
	{
		// File level holds 1 (content stream), 2 (font) and 3 (the /ObjStm itself). The
		// catalogue, the page tree and the page are 4, 5 and 6 — numbers nothing at file level
		// defines, so the only place they exist is inside the compressed stream.
		std::vector<std::string> inner;
		inner.push_back("<< /Type /Catalog /Pages 5 0 R >>");
		inner.push_back("<< /Type /Pages /Count 1 /Kids [ 6 0 R ] >>");
		inner.push_back("<< /Type /Page /Parent 5 0 R /MediaBox [0 0 612 792] /Resources << "
			"/Font << /F1 2 0 R >> >> /Contents 1 0 R >>");

		std::string payload;
		std::string header;
		for (size_t i = 0; i < inner.size(); ++i)
		{
			header += std::to_string(i + 4) + " " + std::to_string(payload.size()) + " ";
			payload += inner[i];
			payload += " ";
		}
		const std::string data = header + payload;

		Builder builder;
		builder.addStream("/Filter /FlateDecode", deflated(onePageContent()));
		builder.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
		builder.addStream("/Type /ObjStm /N " + std::to_string(inner.size()) + " /First "
			+ std::to_string(header.size()) + " /Filter /FlateDecode", deflated(data));
		return builder.build(4);
	}

	// A page that is one `DCTDecode` image and no text operators at all: a scan.
	inline std::string scannedPdf()
	{
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << "
			"/XObject << /Im0 5 0 R >> >> /Contents 4 0 R >>");
		builder.addStream("/Filter /FlateDecode",
			deflated("q\n612 0 0 792 0 0 cm\n/Im0 Do\nQ\n"));
		// Not a decodable JPEG, and it does not need to be: nothing here ever asks for the
		// pixels. What matters is that the file carries image data and no text.
		std::string jpeg("\xFF\xD8\xFF\xE0", 4);
		jpeg += "JFIF";
		jpeg.append(64, '\x7F');
		builder.addStream("/Type /XObject /Subtype /Image /Width 1700 /Height 2200 "
			"/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /DCTDecode", jpeg);
		return builder.build(1);
	}

	// A subset-embedded font: the byte codes are whatever the subsetter assigned, and only the
	// `/ToUnicode` CMap says what they mean. Without it this page reads as control characters.
	inline std::string toUnicodePdf()
	{
		const std::string cmap =
			"/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
			"1 begincodespacerange\n<00> <FF>\nendcodespacerange\n"
			"3 beginbfchar\n<01> <004C>\n<02> <004D>\n<03> <03A9>\nendbfchar\n"
			"1 beginbfrange\n<10> <12> <0030>\nendbfrange\n"
			"endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
			"<< /F1 5 0 R >> >> /Contents 4 0 R >>");
		builder.addStream("/Filter /FlateDecode",
			deflated("BT\n/F1 12 Tf\n72 720 Td\n<010203101112> Tj\nET\n"));
		builder.add("<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Arial /FirstChar 1 "
			"/LastChar 18 /ToUnicode 6 0 R >>");
		builder.addStream(std::string(), cmap);
		return builder.build(1);
	}

	// `TJ` kerning: the number between two strings is a gap in thousandths of the text space
	// unit. A big one is a word space; a small one is letter fitting inside one word. Getting
	// this wrong turns a whole page into one run-on word.
	inline std::string kerningPdf()
	{
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font "
			"<< /F1 5 0 R >> >> /Contents 4 0 R >>");
		builder.addStream("/Filter /FlateDecode",
			deflated("BT\n/F1 12 Tf\n72 720 Td\n"
				"[(Sup) -18 (ply) -320 (voltage)] TJ\nET\n"));
		builder.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
		return builder.build(1);
	}

	// The body text inside a `/Subtype /Form` XObject, which is where a vendor-generated
	// datasheet very often puts it — the page's own stream is a `Do` and nothing else.
	inline std::string formXObjectPdf()
	{
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << "
			"/XObject << /Fm0 5 0 R >> /Font << /F1 6 0 R >> >> /Contents 4 0 R >>");
		builder.addStream("/Filter /FlateDecode", deflated("q\n/Fm0 Do\nQ\n"));
		builder.addStream("/Type /XObject /Subtype /Form /BBox [0 0 612 792] /Resources << "
			"/Font << /F1 6 0 R >> >> /Filter /FlateDecode", deflated(onePageContent()));
		builder.add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
		return builder.build(1);
	}

	// A trailer that names an `/Encrypt` dictionary. Common in the wild — plenty of vendors
	// restrict printing with an empty user password — and it must be *named*, not reported as a
	// datasheet with no text in it.
	inline std::string encryptedPdf()
	{
		Builder builder;
		builder.add("<< /Type /Catalog /Pages 2 0 R >>");
		builder.add("<< /Type /Pages /Count 1 /Kids [ 3 0 R ] >>");
		builder.add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>");
		builder.addStream(std::string(), "BT ET");
		builder.add("<< /Filter /Standard /V 2 /R 3 /Length 128 /P -44 >>");
		return builder.build(1, "/Encrypt 5 0 R ");
	}

	inline std::string notAPdf()
	{
		return "(module \"RESC1608X50N\" (layer F.Cu)\n  (descr \"CRCW0603\")\n)\n";
	}
}

class TST_PdfText : public UnitTest::Test
{
	TEST_CLASS(TST_PdfText)
public:
	TST_PdfText()
		: Test("TST_PdfText")
	{
		ADD_TEST(TST_PdfText::anUncompressedContentStreamReadsBack);
		ADD_TEST(TST_PdfText::aFlateStreamReadsTheSameAsAnUncompressedOne);
		ADD_TEST(TST_PdfText::aFilterChainIsAppliedInOrder);
		ADD_TEST(TST_PdfText::aPageTreeInsideAnObjectStreamIsFound);
		ADD_TEST(TST_PdfText::textInsideAFormXObjectIsFound);
		ADD_TEST(TST_PdfText::toUnicodeTurnsSubsetCodesBackIntoCharacters);
		ADD_TEST(TST_PdfText::kerningBecomesSpacesOnlyWhereTheGapIsReal);
		ADD_TEST(TST_PdfText::everyPageIsCountedAndMaxPagesLimitsOnlyTheText);
		ADD_TEST(TST_PdfText::anImageOnlyPageIsReportedAsScannedAndNotAsEmpty);
		ADD_TEST(TST_PdfText::anEncryptedFileIsNamedAsEncrypted);
		ADD_TEST(TST_PdfText::somethingThatIsNotAPdfIsRefusedAndNeverThrows);
		ADD_TEST(TST_PdfText::searchReturnsSnippetsWithTheirPageNumber);
		ADD_TEST(TST_PdfText::searchWithNoHitReturnsNothingRatherThanGuessing);
	}

private:

	static bool contains(const std::string& haystack, const char* needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	// ---- Tests -----------------------------------------------------------------------------

	// The floor everything else stands on: the operators, not the compression.
	TEST_FUNCTION(anUncompressedContentStreamReadsBack)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::uncompressedPdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		TEST_ASSERT_M(result.pageCount == 1, "expected one page");
		TEST_ASSERT_M(result.pages.size() == 1, "expected one extracted page");
		TEST_ASSERT_M(result.pages[0].number == 1, "pages are numbered as the datasheet prints");
		TEST_ASSERT_M(contains(result.pages[0].text, "Absolute Maximum Ratings"),
			"the first line is missing: " + result.pages[0].text);
		TEST_ASSERT_M(contains(result.pages[0].text, "Supply voltage 32 V"),
			"the second line is missing: " + result.pages[0].text);
		// A `Td` that moved the baseline down is a line break, not a space — without it the
		// whole page arrives as one paragraph and a snippet quotes across two rows of a table.
		TEST_ASSERT_M(contains(result.pages[0].text, "Ratings\nSupply"),
			"the line break between the two Td blocks was lost: " + result.pages[0].text);
		TEST_ASSERT_M(!result.looksScanned, "a text page is not a scan");
	}

	// The 22-of-27 case from the corpus: `FlateDecode` carrying `BT`/`Tj`.
	TEST_FUNCTION(aFlateStreamReadsTheSameAsAnUncompressedOne)
	{
		TEST_START;

		const PartManager::PdfTextResult flate =
			PartManager::PdfText::extractBytes(PdfFixtures::flatePdf());
		const PartManager::PdfTextResult plain =
			PartManager::PdfText::extractBytes(PdfFixtures::uncompressedPdf());
		TEST_ASSERT_M(flate.ok, "the compressed file failed: " + flate.error);
		TEST_ASSERT_M(plain.ok, "the uncompressed file failed: " + plain.error);
		TEST_ASSERT_M(flate.allText() == plain.allText(),
			"inflating changed the text: [" + flate.allText() + "] vs [" + plain.allText() + "]");
		TEST_ASSERT_M(!flate.allText().empty(), "both came back empty, which proves nothing");
	}

	TEST_FUNCTION(aFilterChainIsAppliedInOrder)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::chainedFilterPdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		TEST_ASSERT_M(contains(result.allText(), "Absolute Maximum Ratings"),
			"[/ASCIIHexDecode /FlateDecode] did not round-trip: " + result.allText());
	}

	// Very likely what the crude corpus scan's misses were: a file whose whole page tree is
	// inside a compressed object stream, so nothing at file level says `/Type /Page` at all.
	TEST_FUNCTION(aPageTreeInsideAnObjectStreamIsFound)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::objectStreamPdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		TEST_ASSERT_M(result.pageCount == 1, "the page inside the /ObjStm was not found");
		TEST_ASSERT_M(contains(result.allText(), "Absolute Maximum Ratings"),
			"the page was found but its text was not: " + result.allText());
	}

	TEST_FUNCTION(textInsideAFormXObjectIsFound)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::formXObjectPdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		TEST_ASSERT_M(contains(result.allText(), "Supply voltage 32 V"),
			"a page whose content is one `Do` came back empty: " + result.allText());
	}

	// The difference between a datasheet and mojibake. Codes 1-3 and 0x10-0x12 mean nothing on
	// their own; the CMap is the only thing that says they are L, M, Omega, 0, 1, 2.
	TEST_FUNCTION(toUnicodeTurnsSubsetCodesBackIntoCharacters)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::toUnicodePdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		const std::string text = result.allText();
		// "LM" from bfchar, the ohm sign as UTF-8, then "012" from the bfrange.
		TEST_ASSERT_M(contains(text, "LM"), "bfchar did not map: [" + text + "]");
		TEST_ASSERT_M(contains(text, "\xCE\xA9"),
			"U+03A9 did not survive as UTF-8: [" + text + "]");
		TEST_ASSERT_M(contains(text, "012"), "bfrange did not count up: [" + text + "]");
	}

	TEST_FUNCTION(kerningBecomesSpacesOnlyWhereTheGapIsReal)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::kerningPdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		const std::string text = result.allText();
		// -18 is letter fitting inside one word; -320 is a word space.
		TEST_ASSERT_M(contains(text, "Supply voltage"),
			"the kerning threshold split or fused the words: [" + text + "]");
	}

	// What read_datasheet's "past the end" refusal depends on: `pageCount` is the file's real
	// total whatever `maxPages` caps the *text* at.
	TEST_FUNCTION(everyPageIsCountedAndMaxPagesLimitsOnlyTheText)
	{
		TEST_START;

		const std::string bytes = PdfFixtures::twoPageFlatePdf();
		const PartManager::PdfTextResult whole = PartManager::PdfText::extractBytes(bytes);
		TEST_ASSERT_M(whole.ok, "extraction failed: " + whole.error);
		TEST_ASSERT_M(whole.pageCount == 2, "expected two pages");
		TEST_ASSERT_M(whole.pages.size() == 2, "expected both pages extracted");
		TEST_ASSERT_M(whole.pages[1].number == 2, "the second page is page 2");
		TEST_ASSERT_M(contains(whole.pages[1].text, "Thermal resistance"),
			"the second page's text is missing: " + whole.pages[1].text);
		// A form feed between pages, which is what allText() promises.
		TEST_ASSERT_M(whole.allText().find('\f') != std::string::npos,
			"pages are not separated in allText()");

		const PartManager::PdfTextResult first =
			PartManager::PdfText::extractBytes(bytes, 1);
		TEST_ASSERT_M(first.ok, "the capped extraction failed: " + first.error);
		TEST_ASSERT_M(first.pageCount == 2, "pageCount must still be the file's real total");
		TEST_ASSERT_M(first.pages.size() == 1, "maxPages did not limit the extracted pages");
	}

	// The failure that has to be legible. Nothing will get text out of a scan, and an empty
	// string is the answer a model fills in from memory.
	TEST_FUNCTION(anImageOnlyPageIsReportedAsScannedAndNotAsEmpty)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::scannedPdf());
		TEST_ASSERT_M(result.ok, "a scan is a file that was read, not a failure: " + result.error);
		TEST_ASSERT_M(result.pageCount == 1, "expected one page");
		TEST_ASSERT_M(result.looksScanned, "a DCTDecode page with no text operators is a scan");
		TEST_ASSERT_M(result.allText().empty(),
			"there is no text on it: [" + result.allText() + "]");
	}

	TEST_FUNCTION(anEncryptedFileIsNamedAsEncrypted)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::encryptedPdf());
		TEST_ASSERT_M(!result.ok, "an encrypted file must not report success");
		TEST_ASSERT_M(contains(result.error, "encrypted"),
			"the error has to say which failure this is: " + result.error);
	}

	// "Never throws" is a contract, not an aspiration: a handler that throws costs a whole turn.
	TEST_FUNCTION(somethingThatIsNotAPdfIsRefusedAndNeverThrows)
	{
		TEST_START;

		bool threw = false;
		PartManager::PdfTextResult notPdf;
		PartManager::PdfTextResult empty;
		PartManager::PdfTextResult truncated;
		PartManager::PdfTextResult missing;
		try
		{
			notPdf = PartManager::PdfText::extractBytes(PdfFixtures::notAPdf());
			empty = PartManager::PdfText::extractBytes(std::string());
			// A real PDF cut in half: the header is there and nothing else is.
			const std::string whole = PdfFixtures::flatePdf();
			truncated = PartManager::PdfText::extractBytes(whole.substr(0, whole.size() / 3));
			missing = PartManager::PdfText::extract("Z:\\no\\such\\datasheet.pdf");
		}
		catch (...)
		{
			threw = true;
		}
		TEST_ASSERT_M(!threw, "extraction threw; the contract says it never does");
		TEST_ASSERT_M(!notPdf.ok, "a KiCad footprint is not a PDF");
		TEST_ASSERT_M(contains(notPdf.error, "not a PDF"),
			"the error must name the failure: " + notPdf.error);
		TEST_ASSERT_M(!empty.ok && !empty.error.empty(), "an empty file is refused with a reason");
		// A file cut in half may still have a readable catalogue, so this does not insist on a
		// failure — what it insists on is that nothing is *invented* from the half that is gone.
		// The empty-text case is caught one level up: DatasheetToolset refuses an extraction that
		// is readable, not a scan, and carries no text.
		TEST_ASSERT_M(!contains(truncated.allText(), "Absolute"),
			"a truncated PDF produced text it does not contain: " + truncated.allText());
		TEST_ASSERT_M(!missing.ok && !missing.error.empty(),
			"a path that does not exist is refused with a reason");
	}

	TEST_FUNCTION(searchReturnsSnippetsWithTheirPageNumber)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::twoPageFlatePdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);

		// Case-insensitive, because a model types the term the way it reads best.
		const std::vector<PartManager::PdfTextMatch> hits =
			PartManager::PdfText::search(result, "thermal RESISTANCE");
		TEST_ASSERT_M(hits.size() == 1, "expected exactly one hit");
		TEST_ASSERT_M(hits[0].page == 2, "the hit is on page 2, not page 1");
		TEST_ASSERT_M(contains(hits[0].snippet, "Thermal resistance junction"),
			"the snippet does not carry the passage: " + hits[0].snippet);

		const std::vector<PartManager::PdfTextMatch> capped =
			PartManager::PdfText::search(result, "a", 3);
		TEST_ASSERT_M(capped.size() <= 3, "maxMatches was not honoured");

		// The quota is spread over the pages rather than spent on the first one. Measured on
		// the user's 48-page PCA9745B: "supply voltage" appears twice on page 1 and the
		// limiting-values table is on page 36, so a straight reading-order scan answers a
		// question about the ratings from the summary paragraph and never reaches the table.
		// "voltage" is on page 1 twice here and on page 2 once.
		const std::vector<PartManager::PdfTextMatch> spread =
			PartManager::PdfText::search(result, "e", 2);
		TEST_ASSERT_M(spread.size() == 2, "expected two hits");
		TEST_ASSERT_M(spread[0].page == 1 && spread[1].page == 2,
			"the second hit came from page 1 again instead of page 2");
	}

	TEST_FUNCTION(searchWithNoHitReturnsNothingRatherThanGuessing)
	{
		TEST_START;

		const PartManager::PdfTextResult result =
			PartManager::PdfText::extractBytes(PdfFixtures::flatePdf());
		TEST_ASSERT_M(result.ok, "extraction failed: " + result.error);
		TEST_ASSERT_M(PartManager::PdfText::search(result, "unobtainium").empty(),
			"a term the datasheet does not carry must produce no hits");
		TEST_ASSERT_M(PartManager::PdfText::search(result, std::string()).empty(),
			"an empty needle matches nothing rather than everything");
	}
};

TEST_INSTANTIATE(TST_PdfText);
