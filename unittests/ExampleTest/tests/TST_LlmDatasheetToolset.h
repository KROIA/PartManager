#pragma once

#include "UnitTest.h"
#include "llm/PartManager_DatasheetToolset.h"
#include "tests/TST_LlmTestDatabase.h"
#include "tests/TST_PdfText.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "domain/PartManager_PartFileRole.h"
	#include "filestore/PartManager_FileStore.h"
	#include <QJsonArray>
	#include <QJsonObject>
	#include <QString>
	#include <string>
	#include <vector>
#endif

// The §14g datasheet tools driven the way a model drives them: a tool looked up by its
// model-facing name, called with a QJsonObject, against a throwaway database in %TEMP%. No
// Ollama, no network, no event loop.
//
// The PDFs come from `PdfFixtures` in TST_PdfText.h — built in memory, never read from the user's
// filestore, so this suite passes on a clean clone (ORIENTATION §6). They reach the database the
// way a user's datasheet does: through `FileStore::replaceRoleFileBytes()` into the single-slot
// `Datasheet` role.
//
// Most of these cases are about one property: **a datasheet that cannot be read must come back
// saying so.** A scan, an encrypted file and a file that yielded no text are three different
// answers here, and none of them is an empty string — because an empty string is what a model
// fills in from its own memory of what an LM358 does.
// @see PartManager_DatasheetToolset.h, PartManager_PdfText.h, docs/design/ARCHITECTURE.md §14g
class TST_LlmDatasheetToolset : public UnitTest::Test
{
	TEST_CLASS(TST_LlmDatasheetToolset)
public:
	TST_LlmDatasheetToolset()
		: Test("TST_LlmDatasheetToolset")
	{
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_LlmDatasheetToolset::everyDocumentedToolIsThereAndNoneTakesAPath);
		ADD_TEST(TST_LlmDatasheetToolset::aPartWithNoDatasheetIsListedAndNotRefused);
		ADD_TEST(TST_LlmDatasheetToolset::listingCountsThePagesOfTheOnesThatHaveOne);
		ADD_TEST(TST_LlmDatasheetToolset::searchFindsThePassageAndSaysWhichPage);
		ADD_TEST(TST_LlmDatasheetToolset::noHitIsAnEmptyMatchListWithStatusOk);
		ADD_TEST(TST_LlmDatasheetToolset::aScannedDatasheetSaysScannedRatherThanNothing);
		ADD_TEST(TST_LlmDatasheetToolset::anUnreadableDatasheetIsRefusedByName);
		ADD_TEST(TST_LlmDatasheetToolset::readPastTheEndIsRefusedWithTheRealPageCount);
		ADD_TEST(TST_LlmDatasheetToolset::maxCharsReportsThatItTruncated);
		ADD_TEST(TST_LlmDatasheetToolset::readDefaultsToOnePageAndSpansOnRequest);
		ADD_TEST(TST_LlmDatasheetToolset::anInventedPartIdIsRefusedWithTheWayToFindOne);
		ADD_TEST(TST_LlmDatasheetToolset::readOnlyChangesNothingBecauseAllThreeAreReads);
#endif
	}

private:

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	static QJsonObject call(const std::vector<PartManager::LlmTool>& tools, const char* name,
		const QJsonObject& args = QJsonObject())
	{
		return callLlmTool(tools, name, args);
	}

	static int createPart(const std::vector<PartManager::LlmTool>& partTools, int categoryId,
		const char* name, const char* mpn)
	{
		QJsonObject args;
		args["categoryId"] = categoryId;
		args["name"] = QString::fromLatin1(name);
		args["mpn"] = QString::fromLatin1(mpn);
		return call(partTools, "create_part", args).value("id").toInt();
	}

	// The datasheet arrives the way a user's does: content into the store, `part_file` row into
	// the single-slot `Datasheet` role, both in one step.
	static bool attachDatasheet(PartManager::DatabaseHandle& handle, int partId,
		const std::string& bytes, const char* fileName)
	{
		PartManager::FileStore store(handle.filestorePath());
		return store.replaceRoleFileBytes(handle.connection(), partId,
			PartManager::PartFileRole::Datasheet, bytes,
			std::string(fileName)) != 0;
	}

	static QJsonObject entryForPart(const QJsonObject& listing, int partId)
	{
		const QJsonArray parts = listing.value("parts").toArray();
		for (const QJsonValue& entry : parts)
		{
			if (entry.toObject().value("partId").toInt() == partId)
			{
				return entry.toObject();
			}
		}
		return QJsonObject();
	}

	static bool contains(const std::string& haystack, const char* needle)
	{
		return haystack.find(needle) != std::string::npos;
	}

	// ---- Tests -----------------------------------------------------------------------------

	// The three tools the contract header documents, and the property §14f turns on: the model
	// reaches a datasheet through its part and there is no way to name a file on disk.
	TEST_FUNCTION(everyDocumentedToolIsThereAndNoneTakesAPath)
	{
		TEST_START;

		ScopedDatabase database("datasheet_tools");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();
		TEST_ASSERT_M(tools.size() == 3, "expected exactly three datasheet tools");

		for (const char* const name : { "list_datasheets", "search_datasheet", "read_datasheet" })
		{
			const PartManager::LlmTool* tool =
				PartManager::findLlmTool(tools, QString::fromLatin1(name));
			TEST_ASSERT_M(tool != nullptr, std::string("missing tool: ") + name);
		}

		// §14f, checked the same way TST_LlmKicadToolset checks it: over every parameter of
		// every tool, so a fourth tool cannot quietly add one. A datasheet is reached through
		// its part and there is no way to name a file on disk.
		const char* forbidden[] = { "path", "file", "dir", "folder", "url" };
		for (const PartManager::LlmTool& tool : tools)
		{
			const QJsonObject api = tool.schema.toApiObject();
			const std::string toolName = api.value("name").toString().toStdString();
			const QJsonObject properties =
				api.value("input_schema").toObject().value("properties").toObject();
			for (const QString& parameter : properties.keys())
			{
				const QString lowered = parameter.toLower();
				for (const char* word : forbidden)
				{
					TEST_ASSERT_M(!lowered.contains(QString::fromLatin1(word)),
						"a datasheet tool parameter names a filesystem object: "
						+ toolName + "." + parameter.toStdString());
				}
			}
		}
	}

	// `hasDatasheet: false` is the common case in a real library — Mouser returns an empty
	// DataSheetUrl for most parts (§6) — so it is an answer and not an error.
	TEST_FUNCTION(aPartWithNoDatasheetIsListedAndNotRefused)
	{
		TEST_START;

		ScopedDatabase database("datasheet_none");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		TEST_ASSERT(categoryId != 0);
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(partId != 0);

		const QJsonObject listing = call(tools, "list_datasheets");
		TEST_ASSERT_M(llmResultOk(listing),
			"listing a database with no datasheets is not an error: "
				+ llmResultMessage(listing));
		const QJsonObject entry = entryForPart(listing, partId);
		TEST_ASSERT_M(!entry.isEmpty(), "the part was not listed at all");
		TEST_ASSERT_M(entry.value("hasDatasheet").toBool() == false,
			"a part with nothing attached must say hasDatasheet:false");
		TEST_ASSERT_M(listing.value("withDatasheet").toInt() == 0,
			"nothing is attached, so the count is zero");

		// The two reading tools do refuse — the model asked for something that is not there —
		// and the refusal names the tool that would have told it so.
		QJsonObject args;
		args["partId"] = partId;
		args["query"] = QStringLiteral("supply voltage");
		const QJsonObject searched = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(!llmResultOk(searched), "searching a part with no datasheet must refuse");
		TEST_ASSERT_M(contains(llmResultMessage(searched), "list_datasheets"),
			"the refusal must name the way out: " + llmResultMessage(searched));
	}

	TEST_FUNCTION(listingCountsThePagesOfTheOnesThatHaveOne)
	{
		TEST_START;

		ScopedDatabase database("datasheet_list");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int withOne = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		const int without = createPart(partTools, categoryId, "R 220R", "");
		TEST_ASSERT(withOne != 0 && without != 0);
		TEST_ASSERT_M(attachDatasheet(*database.handle(), withOne,
			PdfFixtures::twoPageFlatePdf(), "RC0603FR-071KL.pdf"),
			"the datasheet did not attach");

		const QJsonObject listing = call(tools, "list_datasheets");
		TEST_ASSERT_M(llmResultOk(listing), llmResultMessage(listing));
		TEST_ASSERT_M(listing.value("withDatasheet").toInt() == 1, "exactly one part has one");

		const QJsonObject attached = entryForPart(listing, withOne);
		TEST_ASSERT_M(attached.value("hasDatasheet").toBool(), "the attached one says so");
		TEST_ASSERT_M(attached.value("pageCount").toInt() == 2,
			"the page count came back wrong");
		TEST_ASSERT_M(attached.value("scanned").toBool() == false, "a text PDF is not a scan");
		// The readable name, not the content-addressed one — it is what the user's PDF viewer
		// shows them (FileStore.h).
		TEST_ASSERT_M(attached.value("fileName").toString().contains(QStringLiteral("RC0603")),
			"the file name is not the one the file was attached under: "
				+ attached.value("fileName").toString().toStdString());

		const QJsonObject bare = entryForPart(listing, without);
		TEST_ASSERT_M(bare.value("hasDatasheet").toBool() == false, "the bare part has none");
		TEST_ASSERT_M(!bare.contains(QStringLiteral("pageCount")),
			"a part with no datasheet has no page count to report");

		// A category id that exists narrows the list; one that does not is corrected with the
		// ones that do, the same way every other toolset corrects an invented id.
		QJsonObject bad;
		bad["categoryId"] = 999999;
		const QJsonObject refused = call(tools, "list_datasheets", bad);
		TEST_ASSERT_M(!llmResultOk(refused), "an unknown categoryId must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(refused), "no category with id"),
			"the refusal reads like the other toolsets': " + llmResultMessage(refused));
	}

	// The primary tool: find the paragraph, cite the page. A datasheet runs to tens of pages and
	// a local model's context does not.
	TEST_FUNCTION(searchFindsThePassageAndSaysWhichPage)
	{
		TEST_START;

		ScopedDatabase database("datasheet_search");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::twoPageFlatePdf(),
			"RC0603FR-071KL.pdf"));

		QJsonObject args;
		args["partId"] = partId;
		args["query"] = QStringLiteral("thermal resistance");
		const QJsonObject result = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(llmResultOk(result), llmResultMessage(result));
		TEST_ASSERT_M(result.value("scanned").toBool() == false, "this one is text");
		TEST_ASSERT_M(result.value("pageCount").toInt() == 2, "two pages");
		TEST_ASSERT_M(result.value("textChars").toInt() > 0, "there was text to search");

		const QJsonArray matches = result.value("matches").toArray();
		TEST_ASSERT_M(matches.size() == 1, "expected one hit");
		TEST_ASSERT_M(matches[0].toObject().value("page").toInt() == 2,
			"the passage is on page 2 and the answer must say so");
		TEST_ASSERT_M(matches[0].toObject().value("snippet").toString()
			.contains(QStringLiteral("Thermal resistance junction")),
			"the snippet does not carry the passage");

		// A missing query is a refusal that says what to send instead, not a silent whole-file
		// read: handing a model the entire datasheet is the thing this tool exists to avoid.
		QJsonObject noQuery;
		noQuery["partId"] = partId;
		const QJsonObject refused = call(tools, "search_datasheet", noQuery);
		TEST_ASSERT_M(!llmResultOk(refused), "an empty query must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(refused), "read_datasheet"),
			"the refusal names the other tool: " + llmResultMessage(refused));
	}

	// "The datasheet does not say" is an answer, and it has to be distinguishable from a failure.
	TEST_FUNCTION(noHitIsAnEmptyMatchListWithStatusOk)
	{
		TEST_START;

		ScopedDatabase database("datasheet_nohit");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::flatePdf(),
			"RC0603FR-071KL.pdf"));

		QJsonObject args;
		args["partId"] = partId;
		args["query"] = QStringLiteral("unobtainium");
		const QJsonObject result = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(llmResultOk(result),
			"a term the datasheet does not carry is an answer, not an error: "
				+ llmResultMessage(result));
		TEST_ASSERT_M(result.value("matches").toArray().isEmpty(), "there is nothing to match");
		// And the thing that tells the two apart: there *was* text, it simply does not say that.
		TEST_ASSERT_M(result.value("textChars").toInt() > 0,
			"textChars is what separates 'does not say' from 'could not be read'");
		TEST_ASSERT_M(result.value("scanned").toBool() == false, "not a scan either");
	}

	// Roughly one in five of the user's own datasheets is a picture of text. The model has to be
	// able to say "this one is a scan" rather than inventing a specification.
	TEST_FUNCTION(aScannedDatasheetSaysScannedRatherThanNothing)
	{
		TEST_START;

		ScopedDatabase database("datasheet_scan");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "LM358", "LM358P");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::scannedPdf(),
			"LM358P.pdf"));

		QJsonObject args;
		args["partId"] = partId;
		args["query"] = QStringLiteral("supply voltage");
		const QJsonObject searched = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(llmResultOk(searched),
			"a scan is a normal answer, not an error: " + llmResultMessage(searched));
		TEST_ASSERT_M(searched.value("scanned").toBool(), "the answer must say scanned:true");
		TEST_ASSERT_M(searched.value("matches").toArray().isEmpty(), "nothing to match in a scan");
		TEST_ASSERT_M(searched.value("textChars").toInt() == 0, "and no text behind it");

		QJsonObject readArgs;
		readArgs["partId"] = partId;
		const QJsonObject read = call(tools, "read_datasheet", readArgs);
		TEST_ASSERT_M(llmResultOk(read), llmResultMessage(read));
		TEST_ASSERT_M(read.value("scanned").toBool(), "read_datasheet says it too");
		TEST_ASSERT_M(read.value("text").toString().isEmpty(), "there is no text on a scan");

		const QJsonObject listing = call(tools, "list_datasheets");
		TEST_ASSERT_M(entryForPart(listing, partId).value("scanned").toBool(),
			"the listing flags it as well, so the model knows before it asks");
	}

	// Two files that are not readable datasheets, each refused by name. The message is the whole
	// point: a model handed nothing answers from its own memory of the part.
	TEST_FUNCTION(anUnreadableDatasheetIsRefusedByName)
	{
		TEST_START;

		ScopedDatabase database("datasheet_unreadable");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int junkId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		const int lockedId = createPart(partTools, categoryId, "R 10k 0603", "RC0603FR-0710KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), junkId, PdfFixtures::notAPdf(),
			"RC0603FR-071KL.pdf"));
		TEST_ASSERT(attachDatasheet(*database.handle(), lockedId, PdfFixtures::encryptedPdf(),
			"RC0603FR-0710KL.pdf"));

		QJsonObject junkArgs;
		junkArgs["partId"] = junkId;
		junkArgs["query"] = QStringLiteral("supply voltage");
		const QJsonObject junk = call(tools, "search_datasheet", junkArgs);
		TEST_ASSERT_M(!llmResultOk(junk), "a file that is not a PDF must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(junk), "not a PDF"),
			"the refusal must name which failure it was: " + llmResultMessage(junk));
		TEST_ASSERT_M(contains(llmResultMessage(junk), "memory"),
			"and must tell the model not to answer from memory: " + llmResultMessage(junk));

		QJsonObject lockedArgs;
		lockedArgs["partId"] = lockedId;
		lockedArgs["query"] = QStringLiteral("supply voltage");
		const QJsonObject locked = call(tools, "search_datasheet", lockedArgs);
		TEST_ASSERT_M(!llmResultOk(locked), "an encrypted file must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(locked), "encrypted"),
			"an encrypted datasheet is named as encrypted, not reported as empty: "
				+ llmResultMessage(locked));

		// The listing reports the same two as unreadable rather than as zero-page files.
		const QJsonObject listing = call(tools, "list_datasheets");
		TEST_ASSERT_M(llmResultOk(listing), llmResultMessage(listing));
		TEST_ASSERT_M(entryForPart(listing, junkId).contains(QStringLiteral("error")),
			"the listing says which file it could not open");
		TEST_ASSERT_M(!entryForPart(listing, lockedId).value("error").toString().isEmpty(),
			"and why");
	}

	TEST_FUNCTION(readPastTheEndIsRefusedWithTheRealPageCount)
	{
		TEST_START;

		ScopedDatabase database("datasheet_pastend");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::twoPageFlatePdf(),
			"RC0603FR-071KL.pdf"));

		QJsonObject args;
		args["partId"] = partId;
		args["fromPage"] = 7;
		const QJsonObject result = call(tools, "read_datasheet", args);
		TEST_ASSERT_M(!llmResultOk(result), "page 7 of a two-page file must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(result), "2 pages"),
			"the refusal must carry the real page count: " + llmResultMessage(result));
		TEST_ASSERT_M(result.value("pageCount").toInt() == 2,
			"and carry it as a field a caller can read");

		// Zero is not a page: the numbering is what the datasheet prints, which starts at 1.
		QJsonObject zero;
		zero["partId"] = partId;
		zero["fromPage"] = 0;
		TEST_ASSERT_M(!llmResultOk(call(tools, "read_datasheet", zero)),
			"fromPage 0 must be refused");
	}

	// A model that does not know it was cut off will finish the sentence itself.
	TEST_FUNCTION(maxCharsReportsThatItTruncated)
	{
		TEST_START;

		ScopedDatabase database("datasheet_truncate");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::flatePdf(),
			"RC0603FR-071KL.pdf"));

		QJsonObject cut;
		cut["partId"] = partId;
		cut["maxChars"] = 10;
		const QJsonObject truncated = call(tools, "read_datasheet", cut);
		TEST_ASSERT_M(llmResultOk(truncated), llmResultMessage(truncated));
		TEST_ASSERT_M(truncated.value("truncated").toBool(),
			"maxChars cut the text and the answer must say so");
		TEST_ASSERT_M(truncated.value("text").toString().size() == 10,
			"the cap is a character count, not a suggestion");

		QJsonObject whole;
		whole["partId"] = partId;
		const QJsonObject full = call(tools, "read_datasheet", whole);
		TEST_ASSERT_M(llmResultOk(full), llmResultMessage(full));
		TEST_ASSERT_M(full.value("truncated").toBool() == false,
			"the default cap is far above one page, so nothing was cut");
		TEST_ASSERT_M(full.value("text").toString().contains(
			QStringLiteral("Absolute Maximum Ratings")),
			"the page text did not come through: "
				+ full.value("text").toString().toStdString());
	}

	TEST_FUNCTION(readDefaultsToOnePageAndSpansOnRequest)
	{
		TEST_START;

		ScopedDatabase database("datasheet_span");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::twoPageFlatePdf(),
			"RC0603FR-071KL.pdf"));

		QJsonObject onePage;
		onePage["partId"] = partId;
		const QJsonObject first = call(tools, "read_datasheet", onePage);
		TEST_ASSERT_M(llmResultOk(first), llmResultMessage(first));
		TEST_ASSERT_M(first.value("fromPage").toInt() == 1 && first.value("toPage").toInt() == 1,
			"with no page named it reads page 1 and nothing else");
		TEST_ASSERT_M(!first.value("text").toString().contains(
			QStringLiteral("Thermal resistance")),
			"page 2 leaked into a single-page read");

		QJsonObject both;
		both["partId"] = partId;
		both["fromPage"] = 1;
		both["toPage"] = 99;
		const QJsonObject spanned = call(tools, "read_datasheet", both);
		TEST_ASSERT_M(llmResultOk(spanned), llmResultMessage(spanned));
		// A toPage past the end is clamped to the file rather than refused — what was actually
		// read is what the answer reports.
		TEST_ASSERT_M(spanned.value("toPage").toInt() == 2,
			"toPage must be reported as the page it really stopped at");
		TEST_ASSERT_M(spanned.value("text").toString().contains(
			QStringLiteral("Thermal resistance")),
			"the second page is missing from the span");
	}

	TEST_FUNCTION(anInventedPartIdIsRefusedWithTheWayToFindOne)
	{
		TEST_START;

		ScopedDatabase database("datasheet_badid");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools();

		QJsonObject args;
		args["partId"] = 424242;
		args["query"] = QStringLiteral("supply voltage");
		const QJsonObject searched = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(!llmResultOk(searched), "an id no part has must be refused");
		TEST_ASSERT_M(contains(llmResultMessage(searched), "search_parts"),
			"the refusal names how to find a real one: " + llmResultMessage(searched));

		QJsonObject missing;
		missing["query"] = QStringLiteral("supply voltage");
		TEST_ASSERT_M(!llmResultOk(call(tools, "search_datasheet", missing)),
			"a missing partId must be refused rather than defaulted");

		// A quoted integer is accepted, because a local model sends one about as often as not.
		QJsonObject quoted;
		quoted["partId"] = QStringLiteral("424242");
		quoted["query"] = QStringLiteral("supply voltage");
		TEST_ASSERT_M(contains(llmResultMessage(call(tools, "search_datasheet", quoted)),
			"no part with id 424242"),
			"a quoted id must be read as the id it is, then refused for the right reason");
	}

	// All three are reads, so `allowWrites` is not consulted anywhere in this toolset — a
	// "just answer questions about my library" assistant gets the full set.
	TEST_FUNCTION(readOnlyChangesNothingBecauseAllThreeAreReads)
	{
		TEST_START;

		ScopedDatabase database("datasheet_readonly");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> partTools = database.tools();
		const std::vector<PartManager::LlmTool> tools = database.datasheetTools(false);
		TEST_ASSERT_M(tools.size() == 3, "read-only offers the same three tools");

		const int categoryId = llmCategoryIdNamed(partTools, QStringLiteral("Resistor"));
		const int partId = createPart(partTools, categoryId, "R 1k 0603", "RC0603FR-071KL");
		TEST_ASSERT(attachDatasheet(*database.handle(), partId, PdfFixtures::flatePdf(),
			"RC0603FR-071KL.pdf"));

		TEST_ASSERT_M(llmResultOk(call(tools, "list_datasheets")), "listing still works");

		QJsonObject args;
		args["partId"] = partId;
		args["query"] = QStringLiteral("absolute maximum");
		const QJsonObject searched = call(tools, "search_datasheet", args);
		TEST_ASSERT_M(llmResultOk(searched),
			"searching is a read and must not be blocked: " + llmResultMessage(searched));
		TEST_ASSERT_M(searched.value("matches").toArray().size() == 1,
			"and it still finds the passage");

		QJsonObject readArgs;
		readArgs["partId"] = partId;
		TEST_ASSERT_M(llmResultOk(call(tools, "read_datasheet", readArgs)),
			"reading is a read too");
	}

#endif // QT_ENABLED && QTLLM_LIBRARY_AVAILABLE && SQLITEWRAPPER_LIBRARY_AVAILABLE
};

TEST_INSTANTIATE(TST_LlmDatasheetToolset);
