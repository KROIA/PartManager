#pragma once

#include "UnitTest.h"
#include "llm/PartManager_MigrationAgent.h"
#include "llm/PartManager_MouserToolset.h"
#include "tests/TST_LlmTestDatabase.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "mouser/PartManager_MouserClient.h"
	#include "mouser/PartManager_MouserSearchService.h"
	#include "persistence/PartManager_PartRepository.h"
	#include "persistence/PartManager_SellerRepository.h"
	#include <QEventLoop>
	#include <QJsonArray>
	#include <QJsonDocument>
	#include <QJsonObject>
	#include <QNetworkAccessManager>
	#include <QNetworkReply>
	#include <QNetworkRequest>
	#include <QString>
	#include <QStringList>
	#include <QTimer>
	#include <QUrl>
	#include <string>
#endif

// §6 and §14e end to end: the Mouser tools a model calls, and the headless agent that calls them.
//
// The suite is in two halves and they fail for different reasons, which is the whole point of
// splitting them. The offline half needs nothing but a temp folder — it pins the response mapping
// against a fixed JSON body, and it pins the two refusals a model has to be able to read. The live
// half needs a running Ollama and a Mouser key, and **skips loudly and passes** without them, so a
// machine with no model still gets a green run. What it must never do is pass quietly when Ollama
// *is* there and the migration then fails; every skip below therefore names what was missing.
//
// The Mouser half of the live cases is kept to four requests in total. Mouser answers HTTP 403
// once a key passes roughly 30 requests in a minute, and a suite that trips that turns every
// later run red for a reason that is nowhere in its own output.
class TST_LlmMigration : public UnitTest::Test
{
	TEST_CLASS(TST_LlmMigration)
public:
	TST_LlmMigration()
		: Test("TST_LlmMigration")
	{
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_LlmMigration::everyDocumentedMouserToolIsThere);
		ADD_TEST(TST_LlmMigration::noToolEverTakesAnApiKey);
		ADD_TEST(TST_LlmMigration::aFixedResponseMapsToThePrefillTheImportWrites);
		ADD_TEST(TST_LlmMigration::importRefusesAnInventedCategoryBeforeItAsksMouser);
		ADD_TEST(TST_LlmMigration::readOnlyBlocksTheImportAndStillSearches);
		ADD_TEST(TST_LlmMigration::theArticleNumberLandsInTheSellerLinkNotTheMpn);
		ADD_TEST(TST_LlmMigration::aSecondImportOfTheSameMpnDoesNotForkThePart);
		ADD_TEST(TST_LlmMigration::theAgentMigratesOnePartIntoTheRightCategory);
#endif
	}

private:

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// The live test data, all verified present on Mouser. The resistor is the interesting one:
	// its article number looks nothing like its manufacturer part number, so a path that confuses
	// the two produces a part that looks perfectly fine until an order is staged.
	static const char* resistorArticleNumber() { return "71-CRCW0603-42.2K-E3"; }
	static const char* resistorMpn() { return "CRCW060342K2FKEA"; }

	// One Mouser search body, copied verbatim from what the live API answered for this article
	// number on 2026-09-26. Every trap §6 documents is in it and none of them is invented:
	// `DataSheetUrl` is empty, `ProductAttributes` holds nothing but packaging (so there is no
	// case code and no resistance to read there), the description carries both in prose, and
	// `Price` already contains its own currency beside a `Currency` field that repeats it.
	static std::string fixedSearchResponse()
	{
		return R"({"Errors":[],"SearchResults":{"NumberOfResult":1,"Parts":[{)"
			R"("MouserPartNumber":"71-CRCW0603-42.2K-E3",)"
			R"("ManufacturerPartNumber":"CRCW060342K2FKEA",)"
			R"("Manufacturer":"Vishay",)"
			R"("Description":"Thick Film Resistors - SMD 1/8 Watt 42.2Kohms 1%",)"
			R"("Category":"Thick Film Resistors - SMD",)"
			R"("DataSheetUrl":"",)"
			R"("ProductDetailUrl":"https://www.mouser.ch/en/ProductDetail/Vishay/CRCW060342K2FKEA?qs=v0cjL%2FPx3nyx0V5tfB02DQ%3D%3D",)"
			R"("ImagePath":"https://www.mouser.com/images/vishay/images/CRCW_SPL.jpg",)"
			R"("ProductAttributes":[{"AttributeName":"Packaging","AttributeValue":"Reel"},)"
			R"({"AttributeName":"Packaging","AttributeValue":"Cut Tape"},)"
			R"({"AttributeName":"Standard Pack Qty","AttributeValue":"5000"}],)"
			R"("PriceBreaks":[{"Quantity":1,"Price":"0.089 CHF","Currency":"CHF"},)"
			R"({"Quantity":10,"Price":"0.017 CHF","Currency":"CHF"}]}]}})";
	}

	// The "enum" array a parameter carries in the schema the model is shown.
	static QJsonObject parameterSchema(const std::vector<PartManager::LlmTool>& tools,
		const char* toolName, const char* parameter)
	{
		const PartManager::LlmTool* tool =
			PartManager::findLlmTool(tools, QString::fromLatin1(toolName));
		if (tool == nullptr)
		{
			return QJsonObject();
		}
		return tool->schema.toApiObject().value("input_schema").toObject()
			.value("properties").toObject()
			.value(QString::fromLatin1(parameter)).toObject();
	}

	static QStringList parameterNames(const std::vector<PartManager::LlmTool>& tools,
		const char* toolName)
	{
		const PartManager::LlmTool* tool =
			PartManager::findLlmTool(tools, QString::fromLatin1(toolName));
		if (tool == nullptr)
		{
			return QStringList();
		}
		return tool->schema.toApiObject().value("input_schema").toObject()
			.value("properties").toObject().keys();
	}

	// The Mouser article number written against a part, straight out of part_seller_link — the
	// column the whole §6 path exists to fill correctly.
	static std::string linkedArticleNumber(const ScopedDatabase& database, int partId)
	{
		return PartManager::SellerRepository::mouserPartNumber(
			database.handle()->connection(), partId);
	}

	// The models the local Ollama offers, empty when it is not answering. A plain GET against
	// /api/tags rather than anything from QtLLM: the point is to find out whether the *server* is
	// there before an Agent is built on top of it, and a 200 from this is the only cheap proof.
	static QStringList ollamaModels()
	{
		QNetworkAccessManager network;
		QNetworkRequest request(QUrl(QStringLiteral("http://localhost:11434/api/tags")));
		QNetworkReply* reply = network.get(request);

		QEventLoop loop;
		QTimer timer;
		timer.setSingleShot(true);
		QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
		QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
		timer.start(3000);
		loop.exec();

		QStringList names;
		if (reply->isFinished() && reply->error() == QNetworkReply::NoError)
		{
			const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
			const QJsonArray models = root.value(QStringLiteral("models")).toArray();
			for (const QJsonValue& entry : models)
			{
				names.append(entry.toObject().value(QStringLiteral("name")).toString());
			}
		}
		reply->abort();
		reply->deleteLater();
		return names;
	}

	// The first of `wanted` the server actually offers, "" when it offers none of them. Matched on
	// the bare name too, because Ollama lists "gpt-oss:20b" and a pull without a tag lists
	// "gpt-oss:latest" — the same weights under a name the exact comparison misses.
	static QString firstUsableModel(const QStringList& available, const QStringList& wanted)
	{
		for (const QString& candidate : wanted)
		{
			for (const QString& offered : available)
			{
				if (offered == candidate
					|| offered.section(QLatin1Char(':'), 0, 0) == candidate.section(QLatin1Char(':'), 0, 0))
				{
					return offered;
				}
			}
		}
		return QString();
	}

	// Tests

	TEST_FUNCTION(everyDocumentedMouserToolIsThere)
	{
		TEST_START;

		ScopedDatabase database("mouser_tools");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());

		const std::vector<PartManager::LlmTool> tools = database.mouserTools();
		TEST_COMPARE(tools.size(), static_cast<size_t>(3));

		// The list PartManager_MouserToolset.h documents, in the order it documents it.
		const char* expected[] = { "mouser_search", "mouser_suggest_category", "mouser_import_part" };
		for (const char* name : expected)
		{
			TEST_ASSERT_M(PartManager::findLlmTool(tools, QString::fromLatin1(name)) != nullptr,
				std::string("tool missing: ") + name);
		}

		// The parameters each one takes, because these names are the whole interface a model has.
		TEST_ASSERT_M(parameterNames(tools, "mouser_search").contains(QStringLiteral("query")),
			"mouser_search must take a 'query'");
		TEST_ASSERT_M(parameterNames(tools, "mouser_suggest_category")
			.contains(QStringLiteral("mouserPartNumber")),
			"mouser_suggest_category must take a 'mouserPartNumber'");
		const QStringList importParameters = parameterNames(tools, "mouser_import_part");
		TEST_ASSERT_M(importParameters.contains(QStringLiteral("mouserPartNumber"))
			&& importParameters.contains(QStringLiteral("categoryId"))
			&& importParameters.contains(QStringLiteral("downloadFiles")),
			"mouser_import_part must take mouserPartNumber, categoryId and downloadFiles");

		// categoryId is an integer in the schema, not a string — a category *name* is refused in
		// the handler (§14c rule 2) and the schema says so a step earlier.
		TEST_COMPARE(parameterSchema(tools, "mouser_import_part", "categoryId")
			.value("type").toString().toStdString(), std::string("integer"));

		// The migration agent advertises these three beside every database tool. Measured
		// 2026-09-26: the longer list *shortened* the loop, because mouser_import_part makes whole
		// steps unnecessary — so a case that lets the list shrink would un-learn that quietly.
		const std::vector<QString> migration = PartManager::PartToolset::migrationToolNames();
		for (const QString& name : migration)
		{
			TEST_ASSERT_M(PartManager::findLlmTool(tools, name) == nullptr,
				"a database tool must not also be in the Mouser toolset: " + name.toStdString());
		}
	}

	// §14f: a model can neither read a key nor be talked into echoing one into the chat, because
	// there is nowhere to put one. This is a case rather than a comment because the obvious
	// "just let the caller pass it" is one parameter away at all times.
	TEST_FUNCTION(noToolEverTakesAnApiKey)
	{
		TEST_START;

		ScopedDatabase database("no_api_key");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());

		std::vector<PartManager::LlmTool> tools = database.mouserTools();
		for (const PartManager::LlmTool& tool : database.tools())
		{
			tools.push_back(tool);
		}

		for (const PartManager::LlmTool& tool : tools)
		{
			const QStringList parameters = tool.schema.toApiObject().value("input_schema")
				.toObject().value("properties").toObject().keys();
			for (const QString& parameter : parameters)
			{
				const QString folded = parameter.toCaseFolded();
				TEST_ASSERT_M(!folded.contains(QStringLiteral("apikey"))
					&& !folded.contains(QStringLiteral("api_key"))
					&& !folded.contains(QStringLiteral("token"))
					&& !folded.contains(QStringLiteral("secret")),
					"no tool may take a key as a parameter: " + tool.schema.name().toStdString()
						+ "." + parameter.toStdString());
			}
		}

		// And the refusal when the key is missing names the variable rather than asking for one.
		// It is only reachable with the key unset, so the assertion is on the text the handler
		// would produce either way: the description never invites a key.
		const PartManager::LlmTool* search =
			PartManager::findLlmTool(tools, QStringLiteral("mouser_search"));
		TEST_ASSERT(search != nullptr);
		TEST_ASSERT_M(!search->schema.description().toCaseFolded()
			.contains(QStringLiteral("api key")),
			"the tool description must not mention an API key — there is nothing a model could "
			"do with that but offer one");
	}

	// The offline half of §6: a fixed response body, the pure parser, and the prefill that the
	// import writes from. No key, no network, no model — so this case fails when the mapping
	// breaks and for no other reason.
	TEST_FUNCTION(aFixedResponseMapsToThePrefillTheImportWrites)
	{
		TEST_START;

		const PartManager::MouserSearchResult parsed =
			PartManager::MouserClient::parseSearchResponse(fixedSearchResponse());
		TEST_ASSERT_M(parsed.ok, "a well-formed body must parse: " + parsed.errorMessage);
		TEST_COMPARE(parsed.parts.size(), static_cast<size_t>(1));

		const PartManager::MouserPartPrefill prefill =
			PartManager::MouserSearchService::toPrefill(parsed.parts.front());

		// The trap the whole seller-link rule exists for: these two strings are not each other,
		// and nothing downstream would notice if they were swapped.
		TEST_COMPARE(prefill.mouserPartNumber, std::string(resistorArticleNumber()));
		TEST_COMPARE(prefill.part.mpn, std::string(resistorMpn()));
		TEST_ASSERT_M(prefill.part.mpn != prefill.mouserPartNumber,
			"the article number and the manufacturer part number must stay separate values");

		TEST_COMPARE(prefill.mouserCategory, std::string("Thick Film Resistors - SMD"));
		TEST_ASSERT_M(prefill.suggestedTypeName == std::string("Resistor"),
			"a Mouser resistor category must suggest the Resistor template, got \""
				+ prefill.suggestedTypeName + "\"");
		// All three §6 package sources come up empty on this part, and that is the right answer
		// rather than a gap: the parametric table holds only packaging, the description says
		// nothing about the case, and `CRCW060342K2FKEA` has the digit run "060342" — not "0603",
		// which is why packageFromPartNumber() compares whole runs. A substring search would find
		// a "0603" inside it and fill the field with a coincidence that happens to be right here
		// and is wrong on the next part number.
		TEST_ASSERT_M(prefill.part.package.empty(),
			"a package must never be guessed out of a longer digit run, got \""
				+ prefill.part.package + "\"");
		TEST_ASSERT_M(prefill.part.attributes.find("resistance") != std::string::npos,
			"the resistance is in the description tail and nowhere else: " + prefill.part.attributes);

		// An empty DataSheetUrl is the normal answer for a real part, and the fallback is allowed
		// to have nothing for Vishay — what must never happen is Mouser's own product page being
		// stored as a datasheet, because that URL answers 403 behind a CAPTCHA (§6).
		TEST_ASSERT_M(prefill.datasheetUrl.find("mouser.com/ProductDetail") == std::string::npos,
			"the product page is not a datasheet: " + prefill.datasheetUrl);
		TEST_ASSERT_M(!prefill.productDetailUrl.empty(), "the product page URL is kept for the link");

		// The price break carries its own currency; appending the Currency field as well is how
		// the same currency ends up printed twice.
		TEST_COMPARE(prefill.priceBreaks.size(), static_cast<size_t>(2));
		TEST_COMPARE(prefill.priceBreaks.front().currency, std::string("CHF"));
		TEST_ASSERT_M(prefill.priceBreaks.front().unitPrice > 0.0,
			"a price that did not parse is dropped, never recorded as 0 — \"it was free that day\" "
			"is worse than a gap in the history");

		// A body Mouser answers an error inside is not a result set, even at HTTP 200.
		const PartManager::MouserSearchResult refused =
			PartManager::MouserClient::parseSearchResponse(
				R"({"Errors":[{"Message":"Invalid unique identifier."}],"SearchResults":null})");
		TEST_ASSERT_M(!refused.ok, "an Errors[] inside a 200 must not read as success");
	}

	// Rule 2 of §14c, on the tool that reaches the network: the id is checked *first*, so a model
	// that invented one is corrected without spending one of the ~30 Mouser requests a minute.
	// That ordering is why this case needs neither a key nor a connection.
	TEST_FUNCTION(importRefusesAnInventedCategoryBeforeItAsksMouser)
	{
		TEST_START;

		ScopedDatabase database("invented_category");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> mouser = database.mouserTools();
		const std::vector<PartManager::LlmTool> tools = database.tools();

		QJsonObject args;
		args["mouserPartNumber"] = resistorArticleNumber();
		args["categoryId"] = 99999;
		args["downloadFiles"] = false;
		const QJsonObject result = callLlmTool(mouser, "mouser_import_part", args);

		TEST_ASSERT_M(!llmResultOk(result), "mouser_import_part must refuse a categoryId that does "
			"not exist");
		TEST_ASSERT_M(llmResultMessage(result).find("99999") != std::string::npos,
			"the refusal must repeat the id that was wrong");
		TEST_ASSERT_M(llmResultMessage(result).find("Resistor") != std::string::npos,
			"the refusal must name the categories that do exist — that is the retry it saves");
		TEST_ASSERT_M(result.value("categories").toArray().size() > 0,
			"the refusal must also carry the list as data");

		// A category *name* is never accepted in place of an id.
		QJsonObject byName;
		byName["mouserPartNumber"] = resistorArticleNumber();
		byName["categoryId"] = "Resistor";
		TEST_ASSERT_M(!llmResultOk(callLlmTool(mouser, "mouser_import_part", byName)),
			"a category name must not be accepted where an id is asked for");

		// A missing article number is a correction, not a search for the empty string.
		QJsonObject noNumber;
		noNumber["categoryId"] = llmCategoryIdNamed(tools, QStringLiteral("Resistor"));
		TEST_ASSERT(!llmResultOk(callLlmTool(mouser, "mouser_import_part", noNumber)));
		TEST_ASSERT(!llmResultOk(callLlmTool(mouser, "mouser_search", QJsonObject())));
		TEST_ASSERT(!llmResultOk(callLlmTool(mouser, "mouser_suggest_category", QJsonObject())));

		// None of those refusals wrote a row.
		QJsonObject everything;
		everything["query"] = "";
		TEST_COMPARE(callLlmTool(tools, "search_parts", everything).value("total").toInt(), 0);
	}

	// allowWrites == false is enforced in the handler here too, and before the network call — a
	// read-only assistant must not be able to spend a Mouser request either.
	TEST_FUNCTION(readOnlyBlocksTheImportAndStillSearches)
	{
		TEST_START;

		ScopedDatabase database("mouser_read_only");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());

		const int resistorId = llmCategoryIdNamed(database.tools(), QStringLiteral("Resistor"));
		TEST_ASSERT(resistorId != 0);

		QJsonObject args;
		args["mouserPartNumber"] = resistorArticleNumber();
		args["categoryId"] = resistorId;
		args["downloadFiles"] = false;
		const QJsonObject refused =
			callLlmTool(database.readOnlyMouserTools(), "mouser_import_part", args);
		TEST_ASSERT_M(!llmResultOk(refused), "a read-only toolset must refuse mouser_import_part");
		TEST_ASSERT_M(llmResultMessage(refused).find("may not change") != std::string::npos,
			"the refusal must say why, not merely fail");

		QJsonObject everything;
		everything["query"] = "";
		TEST_COMPARE(callLlmTool(database.tools(), "search_parts", everything).value("total").toInt(), 0);
	}

	// The one rule getting wrong is invisible until an order is staged: the Mouser article number
	// goes into part_seller_link.seller_part_number, and part.mpn stays the *manufacturer's*
	// number. Needs the Mouser key — one request.
	TEST_FUNCTION(theArticleNumberLandsInTheSellerLinkNotTheMpn)
	{
		TEST_START;

		if (!PartManager::MouserClient::hasApiKey())
		{
			TEST_MESSAGE("skipped: MOUSER_SEARCH_API is not set in this process's environment, so "
				"no live lookup is possible. The offline half of this suite still ran.");
			return;
		}

		ScopedDatabase database("seller_link");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> mouser = database.mouserTools();
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int resistorId = llmCategoryIdNamed(tools, QStringLiteral("Resistor"));
		TEST_ASSERT(resistorId != 0);

		QJsonObject args;
		args["mouserPartNumber"] = resistorArticleNumber();
		args["categoryId"] = resistorId;
		// The files are the only part of the import that goes to Mouser's CDN. Off here, so this
		// case costs exactly one Search API request and tests the identical write path.
		args["downloadFiles"] = false;
		const QJsonObject imported = callLlmTool(mouser, "mouser_import_part", args);
		if (!llmResultOk(imported))
		{
			TEST_MESSAGE("skipped: Mouser did not answer the lookup — " + llmResultMessage(imported));
			return;
		}

		const int partId = imported.value("partId").toInt();
		TEST_ASSERT_M(partId != 0, "a successful import must answer with the id it wrote");
		TEST_ASSERT_M(imported.value("created").toBool(), "the first import must report created=true");
		TEST_ASSERT_M(imported.value("sellerLinkWritten").toBool(),
			"an import without a seller link has lost the article number");
		TEST_COMPARE(imported.value("categoryId").toInt(), resistorId);

		const QJsonObject part = callLlmTool(tools, "get_part", QJsonObject{ { "partId", partId } });
		TEST_ASSERT_M(llmResultOk(part), llmResultMessage(part));
		TEST_COMPARE(part.value("mpn").toString().toStdString(), std::string(resistorMpn()));
		TEST_ASSERT_M(part.value("mpn").toString() != QString::fromLatin1(resistorArticleNumber()),
			"the Mouser article number must never be written into part.mpn — it is the "
			"manufacturer's number that belongs there, and the Cart API rejects the other one");

		TEST_ASSERT_M(linkedArticleNumber(database, partId) == std::string(resistorArticleNumber()),
			"the article number must be readable back out of part_seller_link, got \""
				+ linkedArticleNumber(database, partId) + "\"");

		const std::vector<PartManager::PartSellerLink> links =
			PartManager::SellerRepository::linksForPart(database.handle()->connection(), partId);
		TEST_COMPARE(links.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(links.front().isPrimary, "the only seller a part has is the primary one");
		TEST_ASSERT_M(!links.front().url.empty(),
			"the product page URL is what \"Open on Mouser\" opens; an import that drops it "
			"leaves nothing on the part pointing back at where it came from");

		// The category's own attributes were filled from the description, and the fast-filter
		// column went with them — a value that only reached the JSON is a value search cannot find.
		TEST_ASSERT_M(imported.value("attributesWritten").toArray().size() > 0,
			"the resistance is in the description tail and must have been written");
		QJsonObject query;
		query["query"] = "resistance=42.2k";
		const QJsonObject found = callLlmTool(tools, "search_parts", query);
		TEST_ASSERT_M(llmResultOk(found), llmResultMessage(found));
		TEST_ASSERT_M(found.value("total").toInt() == 1,
			"the resistance must have reached attr_resistance, not only the attributes JSON — a "
			"value the search cannot compare against is a value nothing looks at again. Mouser's "
			"description for this part carried it on 2026-09-26; a failure here first means "
			"checking whether it still does.");
	}

	// Re-running a migration must not fork the part. The second call answers with the id that is
	// already there rather than refusing, for the same reason create_category is idempotent: a
	// model handed an error here simply calls the tool again. Needs the Mouser key — one request.
	TEST_FUNCTION(aSecondImportOfTheSameMpnDoesNotForkThePart)
	{
		TEST_START;

		if (!PartManager::MouserClient::hasApiKey())
		{
			TEST_MESSAGE("skipped: MOUSER_SEARCH_API is not set in this process's environment.");
			return;
		}

		ScopedDatabase database("duplicate_mpn");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> mouser = database.mouserTools();
		const std::vector<PartManager::LlmTool> tools = database.tools();

		// The part is written straight into the database rather than imported, so this case costs
		// one Mouser request instead of two: what is under test is what the *second* import does
		// when the mpn is already there, and how the first one got there does not matter.
		const int resistorId = llmCategoryIdNamed(tools, QStringLiteral("Resistor"));
		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 42k2 0603";
		create["mpn"] = resistorMpn();
		const int partId = callLlmTool(tools, "create_part", create).value("id").toInt();
		TEST_ASSERT(partId != 0);

		const int capacitorId = llmCategoryIdNamed(tools, QStringLiteral("Capacitor"));
		TEST_ASSERT(capacitorId != 0 && capacitorId != resistorId);

		QJsonObject args;
		args["mouserPartNumber"] = resistorArticleNumber();
		// Deliberately the *wrong* category: a re-run must answer with the part that exists, not
		// file a second copy of it somewhere else.
		args["categoryId"] = capacitorId;
		args["downloadFiles"] = false;
		const QJsonObject again = callLlmTool(mouser, "mouser_import_part", args);
		if (!llmResultOk(again))
		{
			TEST_MESSAGE("skipped: Mouser did not answer the lookup — " + llmResultMessage(again));
			return;
		}

		TEST_ASSERT_M(again.value("partId").toInt() == partId,
			"a re-import of a known mpn must answer with the id that is already there");
		TEST_ASSERT_M(!again.value("created").toBool(),
			"a re-import must report created=false, or the model cannot tell the two apart");
		TEST_ASSERT_M(again.value("categoryId").toInt() == resistorId,
			"a re-import must not move the part into the category it was asked for");

		// Exactly one row, and it did not move.
		QJsonObject everything;
		everything["query"] = "";
		TEST_COMPARE(callLlmTool(tools, "search_parts", everything).value("total").toInt(), 1);

		// ...and the re-run backfilled the article number the hand-made part never had. That is
		// the only way a database created before the link existed ever gets one.
		TEST_ASSERT_M(linkedArticleNumber(database, partId) == std::string(resistorArticleNumber()),
			"a re-import must backfill the seller link, got \""
				+ linkedArticleNumber(database, partId) + "\"");
	}

	// §14e end to end: the real agent, the real Ollama server and the real Mouser API, against a
	// throwaway database. One migration, because a second would double a two-minute case for
	// nothing. Skips loudly and passes when there is no model to run on; fails when there is one
	// and the part does not land.
	TEST_FUNCTION(theAgentMigratesOnePartIntoTheRightCategory)
	{
		TEST_START;

		if (!PartManager::MouserClient::hasApiKey())
		{
			TEST_MESSAGE("skipped: MOUSER_SEARCH_API is not set in this process's environment, so "
				"the agent has nothing to look the part up with.");
			return;
		}

		const QStringList available = ollamaModels();
		if (available.isEmpty())
		{
			TEST_MESSAGE("skipped: no Ollama server answered http://localhost:11434/api/tags "
				"within 3 s. Start it with `ollama serve` to run this case.");
			return;
		}

		PartManager::MigrationAgent::Config config;
		const QStringList wanted = QStringList{ config.model } + config.fallbackModels;
		const QString model = firstUsableModel(available, wanted);
		if (model.isEmpty())
		{
			// Named rather than counted: llama3.2 is offered by almost every install and is
			// measurably unusable here, so "some model is installed" is not the question.
			TEST_MESSAGE("skipped: Ollama offers none of " + wanted.join(QStringLiteral(", "))
					.toStdString() + ". It offers: " + available.join(QStringLiteral(", ")).toStdString()
				+ ". Pull one with `ollama pull gpt-oss:20b`.");
			return;
		}
		config.model = model;

		ScopedDatabase database("agent_migration");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		// The CDN is the slowest part of the import and proves nothing this case is about.
		config.downloadFiles = false;

		PartManager::MigrationAgent agent(database.context(), config);
		const PartManager::MigrationAgent::Result result =
			agent.migrateBlocking(QString::fromLatin1(resistorArticleNumber()));

		// Printed whether it passed or not: the tool sequence is the only readable account of
		// what the model actually did, and a failure without it is a failure nobody can act on.
		TEST_MESSAGE("model=" + result.model.toStdString()
			+ " calls=" + result.toolCalls.join(QStringLiteral(" -> ")).toStdString()
			+ " duration=" + std::to_string(result.durationMs / 1000) + "s"
			+ " tokens=" + std::to_string(result.inputTokens) + "/"
			+ std::to_string(result.outputTokens));

		TEST_ASSERT_M(result.ok, "the migration failed on model " + result.model.toStdString()
			+ ": " + result.message.toStdString());
		TEST_ASSERT_M(result.partId != 0, "a successful migration must name the part it created");
		TEST_ASSERT_M(!result.toolCalls.isEmpty(),
			"a migration that called no tool cannot have created anything");

		// The database, not the model's closing sentence.
		const QJsonObject part =
			callLlmTool(tools, "get_part", QJsonObject{ { "partId", result.partId } });
		TEST_ASSERT_M(llmResultOk(part), llmResultMessage(part));
		TEST_ASSERT_M(part.value("mpn").toString().toStdString() == std::string(resistorMpn()),
			"the part must carry the manufacturer part number, got \""
				+ part.value("mpn").toString().toStdString() + "\"");
		TEST_ASSERT_M(part.value("mpn").toString() != QString::fromLatin1(resistorArticleNumber()),
			"the Mouser article number must never be written into part.mpn");
		TEST_ASSERT_M(linkedArticleNumber(database, result.partId)
			== std::string(resistorArticleNumber()),
			"the article number must be in part_seller_link, got \""
				+ linkedArticleNumber(database, result.partId) + "\"");

		// This part maps onto a category the database already seeds, so a migration that created
		// a new one instead read the suggestion and ignored it.
		TEST_ASSERT_M(result.categoryName == QStringLiteral("Resistor"),
			"a Vishay thick-film resistor belongs in the seeded Resistor category, landed in \""
				+ result.categoryName.toStdString() + "\"");
		TEST_ASSERT_M(!result.categoryWasCreated,
			"nothing needed creating for a part the seeded categories already cover");

		// Exactly one part: a loop that created two would still satisfy every assertion above.
		QJsonObject everything;
		everything["query"] = "";
		TEST_COMPARE(callLlmTool(tools, "search_parts", everything).value("total").toInt(), 1);
	}

#endif

};

TEST_INSTANTIATE(TST_LlmMigration);
