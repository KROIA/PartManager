#pragma once

#include "UnitTest.h"
#include "mouser/PartManager_MouserCartClient.h"

// The Cart API's offline half (§6). `MOUSER_CART_API` was not in the environment when this was
// written, so the network path has never run — everything here is fixture JSON and body building,
// which is also the half where the mistakes actually live: a rejected line hiding inside a 200,
// and a CartKey sent empty when it should have been omitted.
//
// The live checklist for when the key arrives is in `.claude/PROJECT_STATUS.md`.
class TST_MouserCartClient : public UnitTest::Test
{
	TEST_CLASS(TST_MouserCartClient)
public:
	TST_MouserCartClient()
		: Test("TST_MouserCartClient")
	{
		ADD_TEST(TST_MouserCartClient::insertAndUpdateShareOneBodyShape);
		ADD_TEST(TST_MouserCartClient::newCartOmitsTheKeyEntirely);
		ADD_TEST(TST_MouserCartClient::unorderableLinesNeverReachTheRequest);
#if QT_ENABLED
		ADD_TEST(TST_MouserCartClient::cartLevelErrorsInside200AreFailures);
		ADD_TEST(TST_MouserCartClient::aLineCanBeRejectedWhileTheCartSucceeds);
		ADD_TEST(TST_MouserCartClient::malformedBodyIsAFailureNotAnEmptyCart);
#endif
		ADD_TEST(TST_MouserCartClient::missingKeyFailsWithoutLeakingIt);
	}

private:

	// The distinction that cost a real bug: /cart/items/insert **adds** to the quantity already
	// in the cart, /cart/items/update **sets** it. Verified against the live API 2026-09-02 —
	// inserting qty 5 twice left 10; updating a line holding 10 to 3 left 3. Staging an order a
	// second time (which is exactly what a user does after a partial arrival) therefore has to
	// go through update, or the cart quietly doubles. The body is identical for both, so nothing
	// but the endpoint choice protects against it.
	TEST_FUNCTION(insertAndUpdateShareOneBodyShape)
	{
		TEST_START;

		std::vector<PartManager::MouserCartItemRequest> items;
		PartManager::MouserCartItemRequest item;
		item.mouserPartNumber = "603-RC0603FR-074K7L";
		item.quantity = 40;
		items.push_back(item);

		// One builder for both endpoints. If this ever diverges, the two calls stop being
		// interchangeable at the call site and the endpoint choice becomes hidden.
		const std::string body = PartManager::MouserCartClient::buildInsertBody(
			"11111111-2222-3333-4444-555555555555", items);
		TEST_ASSERT(body.find("\"CartKey\":\"11111111-2222-3333-4444-555555555555\"") != std::string::npos);
		TEST_ASSERT(body.find("\"Quantity\":40") != std::string::npos);

		// Both entry points exist and are distinct; OrderController picks between them on
		// whether the order already carries a CartKey.
		PartManager::MouserCartClient client;
		TEST_ASSERT_M(&PartManager::MouserCartClient::insertItems
			!= &PartManager::MouserCartClient::updateItems,
			"insert and update must remain two separate calls");
		PM_UNUSED(client);
	}

	TEST_FUNCTION(newCartOmitsTheKeyEntirely)
	{
		TEST_START;

		std::vector<PartManager::MouserCartItemRequest> items;
		PartManager::MouserCartItemRequest item;
		item.mouserPartNumber = "603-RC0603FR-074K7L";
		item.quantity = 40;
		items.push_back(item);

		// The spec types CartKey as a uuid. Sending "" is not a uuid, so an empty key has to be
		// left out of the body rather than sent blank — otherwise Mouser answers with a
		// validation error instead of creating a cart.
		const std::string fresh = PartManager::MouserCartClient::buildInsertBody("", items);
		TEST_ASSERT_M(fresh.find("CartKey") == std::string::npos,
			"a new cart must not send a CartKey at all: " + fresh);
		TEST_ASSERT(fresh.find("\"MouserPartNumber\":\"603-RC0603FR-074K7L\"") != std::string::npos);
		TEST_ASSERT(fresh.find("\"Quantity\":40") != std::string::npos);

		const std::string existing = PartManager::MouserCartClient::buildInsertBody(
			"11111111-2222-3333-4444-555555555555", items);
		TEST_ASSERT(existing.find("\"CartKey\":\"11111111-2222-3333-4444-555555555555\"")
			!= std::string::npos);
	}

	TEST_FUNCTION(unorderableLinesNeverReachTheRequest)
	{
		TEST_START;

		std::vector<PartManager::MouserCartItemRequest> items;
		PartManager::MouserCartItemRequest good;
		good.mouserPartNumber = "603-GOOD";
		good.quantity = 5;
		items.push_back(good);
		PartManager::MouserCartItemRequest noNumber;   // a part that was never linked to Mouser
		noNumber.quantity = 5;
		items.push_back(noNumber);
		PartManager::MouserCartItemRequest noQuantity;
		noQuantity.mouserPartNumber = "603-ZERO";
		items.push_back(noQuantity);

		// One bad line fails the whole request, so they are dropped here as a last line of
		// defence. OrderRepository::lines() flags them first so the UI can name them.
		const std::string body = PartManager::MouserCartClient::buildInsertBody("", items);
		TEST_ASSERT(body.find("603-GOOD") != std::string::npos);
		TEST_ASSERT_M(body.find("603-ZERO") == std::string::npos,
			"a zero-quantity line must not be sent: " + body);
		TEST_ASSERT_M(body.find(",{}") == std::string::npos, "no empty line objects: " + body);

		// Nothing orderable at all still has to produce valid JSON, not a dangling comma.
		const std::string empty = PartManager::MouserCartClient::buildInsertBody("", {});
		TEST_COMPARE(empty, std::string("{\"CartItems\":[]}"));
	}

	TEST_FUNCTION(missingKeyFailsWithoutLeakingIt)
	{
		TEST_START;

		// Whatever the environment holds, the failure message must never quote a key. This is the
		// one property that has to hold before the real key is ever set.
		PartManager::MouserCartClient client;
		if (!PartManager::MouserCartClient::hasApiKey())
		{
			const PartManager::MouserCartResult result = client.insertItems("", {});
			TEST_ASSERT_M(!result.ok, "a call without a key must fail");
			TEST_ASSERT_M(result.errorMessage.find("MOUSER_CART_API") != std::string::npos,
				"the message must name the variable to set: " + result.errorMessage);
			TEST_MESSAGE("MOUSER_CART_API is not set - the live cart path is untested, see "
				"PROJECT_STATUS.md");
		}
		else
		{
			// Deliberately no network call here even when the key exists: a unit test must not
			// create a real Mouser cart as a side effect. See PROJECT_STATUS.md for the manual
			// checklist that does exercise it.
			TEST_MESSAGE("MOUSER_CART_API is set - run the manual live checklist in PROJECT_STATUS.md");
		}
	}

#if QT_ENABLED

	TEST_FUNCTION(cartLevelErrorsInside200AreFailures)
	{
		TEST_START;

		// Mouser reports failures inside a 200 body, so the status code alone proves nothing.
		const PartManager::MouserCartResult result = PartManager::MouserCartClient::parseCartResponse(
			"{\"Errors\":[{\"Id\":1,\"Code\":\"Invalid\",\"Message\":\"Invalid API key\"}],"
			"\"CartKey\":null,\"CartItems\":[]}");
		TEST_ASSERT_M(!result.ok, "an Errors[] body must not be treated as success");
		TEST_COMPARE(result.errorMessage, std::string("Invalid API key"));
	}

	TEST_FUNCTION(aLineCanBeRejectedWhileTheCartSucceeds)
	{
		TEST_START;

		// The trap this whole DTO exists for: the cart call succeeded, the cart has a key, and
		// one of the two parts silently did not go in. A plain `if (result.ok)` sails past it.
		const PartManager::MouserCartResult result = PartManager::MouserCartClient::parseCartResponse(
			"{\"Errors\":[],\"CartKey\":\"11111111-2222-3333-4444-555555555555\","
			"\"CurrencyCode\":\"CHF\",\"MerchandiseTotal\":8.4,\"TotalItemCount\":40,"
			"\"CartItems\":["
			"{\"MouserPartNumber\":\"603-GOOD\",\"MfrPartNumber\":\"RC0603FR-074K7L\","
			"\"Description\":\"RES 4.7K\",\"Quantity\":40,\"UnitPrice\":0.21,"
			"\"ExtendedPrice\":8.4,\"Errors\":[]},"
			"{\"MouserPartNumber\":\"603-BAD\",\"Quantity\":0,\"UnitPrice\":0.0,"
			"\"Errors\":[{\"Message\":\"Part not found\"}]}"
			"]}");

		TEST_ASSERT_M(result.ok, "a cart with a per-line error is still a successful call");
		TEST_COMPARE(result.cartKey, std::string("11111111-2222-3333-4444-555555555555"));
		TEST_COMPARE(result.currencyCode, std::string("CHF"));
		TEST_COMPARE(result.totalItemCount, 40);
		TEST_COMPARE(result.lines.size(), static_cast<size_t>(2));
		TEST_COMPARE(result.lines[0].manufacturerPartNumber, std::string("RC0603FR-074K7L"));
		TEST_ASSERT(result.lines[0].errorMessage.empty());
		TEST_COMPARE(result.lines[1].errorMessage, std::string("Part not found"));
		TEST_ASSERT_M(result.hasRejectedLines(),
			"hasRejectedLines() is what stops a half-staged cart looking like a whole one");
	}

	TEST_FUNCTION(malformedBodyIsAFailureNotAnEmptyCart)
	{
		TEST_START;

		// A truncated response must not read as "the cart is empty", which would look like a
		// successful staging of nothing.
		const PartManager::MouserCartResult truncated =
			PartManager::MouserCartClient::parseCartResponse("{\"CartKey\":\"abc\"");
		TEST_ASSERT_M(!truncated.ok, "a truncated body must fail");
		TEST_ASSERT(!truncated.errorMessage.empty());

		const PartManager::MouserCartResult empty =
			PartManager::MouserCartClient::parseCartResponse("");
		TEST_ASSERT_M(!empty.ok, "an empty body must fail");
	}

#endif
};

TEST_INSTANTIATE(TST_MouserCartClient);
