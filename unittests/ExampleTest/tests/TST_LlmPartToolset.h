#pragma once

#include "UnitTest.h"
#include "llm/PartManager_PartToolset.h"
#include "tests/TST_LlmTestDatabase.h"

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "units/PartManager_UnitTable.h"
	#include <QJsonArray>
	#include <QJsonObject>
	#include <QString>
	#include <cmath>
#endif

// The §14a database toolset driven the way a model drives it: a tool looked up by its
// model-facing name, called with a QJsonObject of arguments, against a throwaway database in
// %TEMP%. No client, no network, no model — that split is the whole reason an LlmTool carries a
// plain handler instead of registering itself onto a QtLLM::Client (PartManager_LlmTool.h).
//
// What is pinned here is the behaviour that was *measured* rather than assumed: a create that is
// idempotent, an id that cannot be invented, and a value that goes through ValueParser on the way
// in. Each of those exists because the obvious version of it cost a local model a retry loop, so
// each gets a case that fails if it is ever "simplified" back.
class TST_LlmPartToolset : public UnitTest::Test
{
	TEST_CLASS(TST_LlmPartToolset)
public:
	TST_LlmPartToolset()
		: Test("TST_LlmPartToolset")
	{
#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_LlmPartToolset::everyDocumentedToolIsThere);
		ADD_TEST(TST_LlmPartToolset::createCategoryIsIdempotentOnNameAndParent);
		ADD_TEST(TST_LlmPartToolset::anInventedCategoryIdIsRefusedWithTheRealOnes);
		ADD_TEST(TST_LlmPartToolset::attributeValuesGoThroughTheUnitParser);
		ADD_TEST(TST_LlmPartToolset::anUnreadableValueWritesNothing);
		ADD_TEST(TST_LlmPartToolset::readOnlyBlocksEveryWriteAndStillReads);
		ADD_TEST(TST_LlmPartToolset::getCategoryResolvesInheritedAttributes);
		ADD_TEST(TST_LlmPartToolset::tagsAreAppliedByNameAndUnknownOnesReported);
		ADD_TEST(TST_LlmPartToolset::updatePartWritesOnlyTheKeysGiven);
#endif
	}

private:

#if QT_ENABLED && QTLLM_LIBRARY_AVAILABLE == 1 && SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// The database and the four tool-driving one-liners live in TST_LlmTestDatabase.h, because
	// TST_LlmMigration drives the same tools against the same kind of database and a second copy
	// of either would drift. These names are kept so the cases below read as they always did.
	static QJsonObject call(const std::vector<PartManager::LlmTool>& tools, const char* name,
		const QJsonObject& args = QJsonObject())
	{
		return callLlmTool(tools, name, args);
	}

	static bool isOk(const QJsonObject& result) { return llmResultOk(result); }

	static std::string messageOf(const QJsonObject& result) { return llmResultMessage(result); }

	static int categoryIdNamed(const std::vector<PartManager::LlmTool>& tools, const QString& name)
	{
		return llmCategoryIdNamed(tools, name);
	}

	// One attribute's value out of a get_part answer, or a null value when the part has none.
	static QJsonValue attributeValue(const QJsonObject& part, const QString& key)
	{
		const QJsonArray attributes = part.value("attributes").toArray();
		for (const QJsonValue& entry : attributes)
		{
			if (entry.toObject().value("key").toString() == key)
			{
				return entry.toObject().value("value");
			}
		}
		return QJsonValue();
	}

	// The "enum" array a parameter carries in the schema the model is actually shown. Empty when
	// the parameter is free text — which, for a column with a fixed vocabulary, is the bug.
	static QJsonArray parameterEnum(const std::vector<PartManager::LlmTool>& tools,
		const char* toolName, const char* parameter)
	{
		const PartManager::LlmTool* tool =
			PartManager::findLlmTool(tools, QString::fromLatin1(toolName));
		if (tool == nullptr)
		{
			return QJsonArray();
		}
		return tool->schema.toApiObject().value("input_schema").toObject()
			.value("properties").toObject()
			.value(QString::fromLatin1(parameter)).toObject()
			.value("enum").toArray();
	}

	static bool arrayContainsText(const QJsonArray& array, const QString& text)
	{
		for (const QJsonValue& entry : array)
		{
			if (entry.toString() == text)
			{
				return true;
			}
		}
		return false;
	}

	// Tests
	TEST_FUNCTION(everyDocumentedToolIsThere)
	{
		TEST_START;

		ScopedDatabase database("tools");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());

		const std::vector<PartManager::LlmTool> tools = database.tools();
		TEST_COMPARE(tools.size(), static_cast<size_t>(11));

		// The list PartManager_PartToolset.h documents, in the order it documents it.
		const char* expected[] = { "list_categories", "get_category", "create_category",
			"add_category_attribute", "search_parts", "get_part", "create_part", "update_part",
			"set_part_attribute", "list_tags", "set_part_tags" };
		for (const char* name : expected)
		{
			TEST_ASSERT_M(PartManager::findLlmTool(tools, QString::fromLatin1(name)) != nullptr,
				std::string("tool missing: ") + name);
		}
		TEST_ASSERT_M(PartManager::findLlmTool(tools, QStringLiteral("delete_everything")) == nullptr,
			"findLlmTool must answer nullptr for a name no tool carries");

		// The migration subset has to name tools that exist, or MigrationAgent registers nothing
		// and fails with an empty tool list instead of a missing one.
		const std::vector<QString> migration = PartManager::PartToolset::migrationToolNames();
		TEST_ASSERT_M(!migration.empty(), "migrationToolNames must not be empty");
		// Measured 2026-09-26: the migration loop ran in five calls with all fourteen tools
		// advertised, so the list is not trimmed for size. A case that lets it shrink again would
		// let that lesson be un-learned quietly.
		TEST_ASSERT_M(migration.size() == tools.size(),
			"the migration list must advertise every database tool, not a hand-picked few");
		for (const QString& name : migration)
		{
			TEST_ASSERT_M(PartManager::findLlmTool(tools, name) != nullptr,
				"migrationToolNames names a tool that does not exist: " + name.toStdString());
		}

		// Every parameter backed by a fixed-vocabulary column has to carry that vocabulary in the
		// schema, not only in the handler. Asked for a `domain` as free text, a local model
		// answered "Circuit Protection" — it read the word as the supplier's product family. The
		// enum is what turns that into a correction before the call is even made.
		TEST_COMPARE(parameterEnum(tools, "create_category", "domain").size(), 3);
		TEST_COMPARE(parameterEnum(tools, "add_category_attribute", "datatype").size(), 5);
		const int unitCount = static_cast<int>(PartManager::UnitTable::units().size());
		TEST_ASSERT_M(parameterEnum(tools, "add_category_attribute", "unit").size() == unitCount,
			"the unit parameter must offer the whole unit dropdown, \"(no unit)\" included");
		TEST_ASSERT_M(parameterEnum(tools, "set_part_attribute", "unit").size() == unitCount,
			"the cross-check unit must come from that same list");
	}

	// Rule 1 of PartManager_PartToolset.h, and the one that cost eight calls when it was missing.
	TEST_FUNCTION(createCategoryIsIdempotentOnNameAndParent)
	{
		TEST_START;

		ScopedDatabase database("create_category");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int seededCount = call(tools, "list_categories").value("categories").toArray().size();
		TEST_ASSERT_M(seededCount > 0, "a new database seeds its default categories");

		QJsonObject args;
		args["name"] = "Varistor";
		const QJsonObject first = call(tools, "create_category", args);
		TEST_ASSERT_M(isOk(first), messageOf(first));
		TEST_ASSERT_M(first.value("created").toBool(), "the first create must report created=true");
		const int varistorId = first.value("id").toInt();
		TEST_ASSERT(varistorId != 0);

		// The same call again — what a model does when it loses track of what it already did.
		const QJsonObject second = call(tools, "create_category", args);
		TEST_ASSERT_M(isOk(second), messageOf(second));
		TEST_ASSERT_M(second.value("id").toInt() == varistorId,
			"a repeated create must answer with the id it already made");
		TEST_ASSERT_M(!second.value("created").toBool(),
			"a repeated create must report created=false, or the model cannot tell the two apart");

		// Case and surrounding whitespace are not a new category.
		QJsonObject shouty;
		shouty["name"] = "  vARISTOR ";
		const QJsonObject third = call(tools, "create_category", shouty);
		TEST_ASSERT_M(isOk(third), messageOf(third));
		TEST_ASSERT_M(third.value("id").toInt() == varistorId,
			"the match on an existing name is case- and whitespace-insensitive");
		TEST_ASSERT(!third.value("created").toBool());

		// A different parent *is* a different category: two branches may carry the same leaf name.
		const int resistorId = categoryIdNamed(tools, QStringLiteral("Resistor"));
		TEST_ASSERT_M(resistorId != 0, "the seeded Resistor category must be there");
		QJsonObject child;
		child["name"] = "Varistor";
		child["parentId"] = resistorId;
		const QJsonObject fourth = call(tools, "create_category", child);
		TEST_ASSERT_M(isOk(fourth), messageOf(fourth));
		TEST_ASSERT_M(fourth.value("created").toBool(),
			"the same name under a different parent is a new category");
		const int childId = fourth.value("id").toInt();
		TEST_ASSERT(childId != varistorId);

		// ...and it is idempotent on that pair too, not only on the root.
		const QJsonObject fifth = call(tools, "create_category", child);
		TEST_ASSERT_M(isOk(fifth), messageOf(fifth));
		TEST_ASSERT_M(fifth.value("id").toInt() == childId && !fifth.value("created").toBool(),
			"idempotency is on (name, parentId), not on the name alone");

		// Exactly two new rows after five create calls.
		TEST_COMPARE(call(tools, "list_categories").value("categories").toArray().size(),
			seededCount + 2);

		// A blank name, an invented parent and a nonsense enum are all corrections, not rows.
		QJsonObject blank;
		blank["name"] = "   ";
		TEST_ASSERT_M(!isOk(call(tools, "create_category", blank)), "an empty name must be refused");

		QJsonObject orphan;
		orphan["name"] = "Ghost";
		orphan["parentId"] = 99999;
		const QJsonObject orphanResult = call(tools, "create_category", orphan);
		TEST_ASSERT(!isOk(orphanResult));
		TEST_ASSERT_M(messageOf(orphanResult).find("Resistor") != std::string::npos,
			"an unknown parent id must come back naming the ids that do exist");

		// Rule 3: the enum is checked here, not only by the client-side validator. The value is the
		// one a local model actually answered with on 2026-09-26 — it read `domain` as the
		// supplier's product family rather than as part_type.domain's three-value vocabulary, and
		// free text there is inherited by every child category through effectiveDomain().
		QJsonObject badDomain;
		badDomain["name"] = "Ghost2";
		badDomain["domain"] = "Circuit Protection";
		const QJsonObject badDomainResult = call(tools, "create_category", badDomain);
		TEST_ASSERT_M(!isOk(badDomainResult), "a domain outside the fixed vocabulary must be refused");
		TEST_ASSERT_M(badDomainResult.value("allowed_values").toArray().size() == 3,
			"an enum refusal must hand back the values that are allowed");

		// The schema says so too, so the client-side validator catches it a step earlier.
		const PartManager::LlmTool* createTool =
			PartManager::findLlmTool(tools, QStringLiteral("create_category"));
		TEST_ASSERT(createTool != nullptr);
		const QJsonObject domainSchema = createTool->schema.toApiObject()
			.value("input_schema").toObject()
			.value("properties").toObject()
			.value("domain").toObject();
		TEST_COMPARE(domainSchema.value("enum").toArray().size(), 3);
		TEST_ASSERT_M(!createTool->schema.validate(badDomain).isEmpty(),
			"the tool schema itself must reject a domain outside the vocabulary");

		// None of the three refusals left a row behind.
		TEST_COMPARE(call(tools, "list_categories").value("categories").toArray().size(),
			seededCount + 2);

		// Omitted, the domain is "electronic" — never empty, and never whatever the model was
		// thinking of. An empty one would be inherited as empty by every child (§2b).
		QJsonObject noDomain;
		noDomain["name"] = "Ferrite Bead";
		const int beadId = call(tools, "create_category", noDomain).value("id").toInt();
		TEST_ASSERT(beadId != 0);
		TEST_COMPARE(call(tools, "get_category", QJsonObject{ { "categoryId", beadId } })
			.value("domain").toString().toStdString(), std::string("electronic"));
	}

	// Rule 2: ids are never invented, and a refusal names the ones that exist.
	TEST_FUNCTION(anInventedCategoryIdIsRefusedWithTheRealOnes)
	{
		TEST_START;

		ScopedDatabase database("invented_id");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		QJsonObject args;
		args["categoryId"] = 99999;
		args["name"] = "Ghost part";
		const QJsonObject result = call(tools, "create_part", args);

		TEST_ASSERT_M(!isOk(result), "create_part must refuse a categoryId that does not exist");
		TEST_ASSERT_M(messageOf(result).find("99999") != std::string::npos,
			"the refusal must repeat the id that was wrong");
		TEST_ASSERT_M(messageOf(result).find("Resistor") != std::string::npos,
			"the refusal must name the categories that do exist — that is the retry it saves");

		const QJsonArray offered = result.value("categories").toArray();
		TEST_ASSERT_M(offered.size() > 0, "the refusal must also carry the list as data");
		bool sawResistor = false;
		for (const QJsonValue& entry : offered)
		{
			if (entry.toObject().value("name").toString() == QStringLiteral("Resistor"))
			{
				sawResistor = true;
			}
		}
		TEST_ASSERT(sawResistor);

		// A category *name* is never accepted in place of an id.
		QJsonObject byName;
		byName["categoryId"] = "Resistor";
		byName["name"] = "Ghost part";
		TEST_ASSERT_M(!isOk(call(tools, "create_part", byName)),
			"a category name must not be accepted where an id is asked for");

		// Nothing was written by any of that.
		QJsonObject everything;
		everything["query"] = "";
		const QJsonObject search = call(tools, "search_parts", everything);
		TEST_ASSERT_M(isOk(search), messageOf(search));
		TEST_COMPARE(search.value("total").toInt(), 0);

		// The same id is refused by every other tool that takes one.
		QJsonObject ghost;
		ghost["categoryId"] = 99999;
		TEST_ASSERT(!isOk(call(tools, "get_category", ghost)));
		TEST_ASSERT(!isOk(call(tools, "add_category_attribute", ghost)));

		QJsonObject ghostSearch;
		ghostSearch["query"] = "";
		ghostSearch["categoryId"] = 99999;
		TEST_ASSERT(!isOk(call(tools, "search_parts", ghostSearch)));

		QJsonObject ghostPart;
		ghostPart["partId"] = 4242;
		TEST_ASSERT(!isOk(call(tools, "get_part", ghostPart)));
		TEST_ASSERT(!isOk(call(tools, "update_part", ghostPart)));
		TEST_ASSERT(!isOk(call(tools, "set_part_attribute", ghostPart)));
	}

	// §2a: "4k7" is 4700, and what lands in the database is the number, never the typed text.
	TEST_FUNCTION(attributeValuesGoThroughTheUnitParser)
	{
		TEST_START;

		ScopedDatabase database("value_parser");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int resistorId = categoryIdNamed(tools, QStringLiteral("Resistor"));
		TEST_ASSERT(resistorId != 0);

		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 4k7 0805";
		create["mpn"] = "RC0805FR-074K7L";
		create["package"] = "0805";
		const QJsonObject created = call(tools, "create_part", create);
		TEST_ASSERT_M(isOk(created), messageOf(created));
		const int partId = created.value("id").toInt();
		TEST_ASSERT(partId != 0);

		QJsonObject setValue;
		setValue["partId"] = partId;
		setValue["key"] = "resistance";
		setValue["value"] = "4k7";
		const QJsonObject stored = call(tools, "set_part_attribute", setValue);
		TEST_ASSERT_M(isOk(stored), messageOf(stored));
		TEST_ASSERT_M(std::abs(stored.value("storedValue").toDouble() - 4700.0) < 1e-6,
			"\"4k7\" must be stored as 4700, not as the text that was typed");
		TEST_ASSERT_M(stored.value("storedUnit").toString().toStdString()
			== std::string(PartManager::UnitTable::OhmSymbol),
			"the stored unit is the one the category declares, not one the model chose");

		const QJsonObject part = call(tools, "get_part", QJsonObject{ { "partId", partId } });
		TEST_ASSERT_M(isOk(part), messageOf(part));
		TEST_ASSERT_M(std::abs(attributeValue(part, QStringLiteral("resistance")).toDouble() - 4700.0) < 1e-6,
			"get_part must read back the base-SI number");

		// The same value spelled the other way lands on the same number.
		setValue["value"] = "4.7k";
		const QJsonObject again = call(tools, "set_part_attribute", setValue);
		TEST_ASSERT_M(isOk(again), messageOf(again));
		TEST_ASSERT(std::abs(again.value("storedValue").toDouble() - 4700.0) < 1e-6);

		// And the fast-filter column went with it: the search engine can only find this part by
		// its resistance if the number reached attr_resistance rather than the JSON alone.
		QJsonObject query;
		query["query"] = "resistance=4k7";
		const QJsonObject search = call(tools, "search_parts", query);
		TEST_ASSERT_M(isOk(search), messageOf(search));
		TEST_COMPARE(search.value("total").toInt(), 1);
		TEST_COMPARE(search.value("parts").toArray().at(0).toObject().value("id").toInt(), partId);
		TEST_COMPARE(search.value("parts").toArray().at(0).toObject().value("categoryName").toString()
			.toStdString(), std::string("Resistor"));

		// An enum attribute is checked against its own options, case-insensitively, and stored in
		// the category's spelling rather than the model's.
		const int regulatorId = categoryIdNamed(tools, QStringLiteral("Power Regulator"));
		TEST_ASSERT(regulatorId != 0);
		QJsonObject regulator;
		regulator["categoryId"] = regulatorId;
		regulator["name"] = "LM1117-3.3";
		const int regulatorPartId = call(tools, "create_part", regulator).value("id").toInt();
		TEST_ASSERT(regulatorPartId != 0);

		QJsonObject enumValue;
		enumValue["partId"] = regulatorPartId;
		enumValue["key"] = "regulator_type";
		enumValue["value"] = "linear";
		const QJsonObject enumStored = call(tools, "set_part_attribute", enumValue);
		TEST_ASSERT_M(isOk(enumStored), messageOf(enumStored));
		TEST_COMPARE(enumStored.value("storedValue").toString().toStdString(), std::string("Linear"));

		enumValue["value"] = QString::fromUtf8("\xF0\x9F\x99\x82");   // the emoji answer, measured
		const QJsonObject enumRefused = call(tools, "set_part_attribute", enumValue);
		TEST_ASSERT_M(!isOk(enumRefused), "an enum value outside the list must be refused here too");
		TEST_ASSERT_M(enumRefused.value("allowed_values").toArray().size() == 2,
			"the refusal must hand back the values that are allowed");
	}

	// "A value the parser rejects is an llmError, never a 0" — and never a half-written row.
	TEST_FUNCTION(anUnreadableValueWritesNothing)
	{
		TEST_START;

		ScopedDatabase database("bad_value");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int resistorId = categoryIdNamed(tools, QStringLiteral("Resistor"));
		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 4k7";
		const int partId = call(tools, "create_part", create).value("id").toInt();
		TEST_ASSERT(partId != 0);

		QJsonObject good;
		good["partId"] = partId;
		good["key"] = "resistance";
		good["value"] = "4k7";
		TEST_ASSERT(isOk(call(tools, "set_part_attribute", good)));

		auto resistanceNow = [&]()
		{
			const QJsonObject part = call(tools, "get_part", QJsonObject{ { "partId", partId } });
			return attributeValue(part, QStringLiteral("resistance")).toDouble();
		};
		TEST_ASSERT(std::abs(resistanceNow() - 4700.0) < 1e-6);

		// Prose where a number was asked for.
		QJsonObject prose;
		prose["partId"] = partId;
		prose["key"] = "resistance";
		prose["value"] = "about four and a bit kiloohms";
		const QJsonObject proseResult = call(tools, "set_part_attribute", prose);
		TEST_ASSERT_M(!isOk(proseResult), "an unparsable value must be refused, not stored as 0");
		TEST_ASSERT_M(messageOf(proseResult).find("4k7") != std::string::npos,
			"the refusal must show a spelling that would work");
		TEST_ASSERT_M(std::abs(resistanceNow() - 4700.0) < 1e-6, "a refused write must write nothing");

		// The right shape, the wrong unit: "100uF" in an ohms field is a misreading of the
		// category, and storing it as 0.0001 would hide that misreading forever.
		QJsonObject wrongUnit;
		wrongUnit["partId"] = partId;
		wrongUnit["key"] = "resistance";
		wrongUnit["value"] = "100uF";
		TEST_ASSERT_M(!isOk(call(tools, "set_part_attribute", wrongUnit)),
			"a unit suffix that disagrees with the field must be refused");
		TEST_ASSERT(std::abs(resistanceNow() - 4700.0) < 1e-6);

		// The optional cross-check parameter catches the same misreading one step earlier.
		QJsonObject claimedUnit;
		claimedUnit["partId"] = partId;
		claimedUnit["key"] = "resistance";
		claimedUnit["value"] = "100";
		claimedUnit["unit"] = "F";
		const QJsonObject claimedResult = call(tools, "set_part_attribute", claimedUnit);
		TEST_ASSERT_M(!isOk(claimedResult), "a claimed unit the category disagrees with must be refused");
		TEST_ASSERT(std::abs(resistanceNow() - 4700.0) < 1e-6);

		// A key the category does not declare comes back with the keys it does.
		QJsonObject typo;
		typo["partId"] = partId;
		typo["key"] = "resistanse";
		typo["value"] = "1k";
		const QJsonObject typoResult = call(tools, "set_part_attribute", typo);
		TEST_ASSERT(!isOk(typoResult));
		TEST_ASSERT_M(messageOf(typoResult).find("resistance") != std::string::npos,
			"a mistyped key must come back with the keys that exist");
		TEST_ASSERT(std::abs(resistanceNow() - 4700.0) < 1e-6);
	}

	// allowWrites == false is enforced in the handlers, so the model is told why rather than left
	// to guess at a capability that quietly is not there (PartManager_LlmTool.h).
	TEST_FUNCTION(readOnlyBlocksEveryWriteAndStillReads)
	{
		TEST_START;

		ScopedDatabase database("read_only");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> writable = database.tools();
		const std::vector<PartManager::LlmTool> readOnly = database.readOnlyTools();

		const int resistorId = categoryIdNamed(writable, QStringLiteral("Resistor"));
		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 10k";
		const int partId = call(writable, "create_part", create).value("id").toInt();
		TEST_ASSERT(partId != 0);
		const int categoryCount =
			call(writable, "list_categories").value("categories").toArray().size();

		QJsonObject newCategory;
		newCategory["name"] = "Should never exist";
		const QJsonObject refused = call(readOnly, "create_category", newCategory);
		TEST_ASSERT_M(!isOk(refused), "a read-only toolset must refuse create_category");
		TEST_ASSERT_M(messageOf(refused).find("may not change") != std::string::npos,
			"the refusal must say why, not merely fail");

		QJsonObject newPart;
		newPart["categoryId"] = resistorId;
		newPart["name"] = "Should never exist";
		TEST_ASSERT(!isOk(call(readOnly, "create_part", newPart)));

		QJsonObject setValue;
		setValue["partId"] = partId;
		setValue["key"] = "resistance";
		setValue["value"] = "10k";
		TEST_ASSERT(!isOk(call(readOnly, "set_part_attribute", setValue)));

		QJsonObject rename;
		rename["partId"] = partId;
		rename["name"] = "Renamed by an assistant that may not";
		TEST_ASSERT(!isOk(call(readOnly, "update_part", rename)));

		QJsonObject tags;
		tags["partId"] = partId;
		tags["tags"] = QJsonArray{ "SMD" };
		TEST_ASSERT(!isOk(call(readOnly, "set_part_tags", tags)));

		QJsonObject attribute;
		attribute["categoryId"] = resistorId;
		attribute["key"] = "nope";
		attribute["label"] = "Nope";
		attribute["datatype"] = "text";
		TEST_ASSERT(!isOk(call(readOnly, "add_category_attribute", attribute)));

		// Reading still works — that is the whole point of the flag.
		const QJsonObject listed = call(readOnly, "list_categories");
		TEST_ASSERT_M(isOk(listed), messageOf(listed));
		TEST_COMPARE(listed.value("categories").toArray().size(), categoryCount);

		const QJsonObject part = call(readOnly, "get_part", QJsonObject{ { "partId", partId } });
		TEST_ASSERT_M(isOk(part), messageOf(part));
		TEST_COMPARE(part.value("name").toString().toStdString(), std::string("R 10k"));
		TEST_ASSERT(isOk(call(readOnly, "list_tags")));

		QJsonObject query;
		query["query"] = "";
		const QJsonObject search = call(readOnly, "search_parts", query);
		TEST_ASSERT_M(isOk(search), messageOf(search));
		TEST_COMPARE(search.value("total").toInt(), 1);

		// And none of the refusals left anything behind.
		TEST_COMPARE(call(writable, "list_categories").value("categories").toArray().size(),
			categoryCount);
		TEST_COMPARE(call(writable, "search_parts", query).value("total").toInt(), 1);
		TEST_ASSERT_M(attributeValue(call(writable, "get_part",
			QJsonObject{ { "partId", partId } }), QStringLiteral("resistance")).isNull()
			|| attributeValue(call(writable, "get_part",
				QJsonObject{ { "partId", partId } }), QStringLiteral("resistance")).isUndefined(),
			"the blocked set_part_attribute must not have written a value");
	}

	// §2b: a child category's attribute list is its ancestors' plus its own, and the model is
	// handed the resolved list — a part carries them all with no distinction.
	TEST_FUNCTION(getCategoryResolvesInheritedAttributes)
	{
		TEST_START;

		ScopedDatabase database("inherited");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int capacitorId = categoryIdNamed(tools, QStringLiteral("Capacitor"));
		const int ceramicId = categoryIdNamed(tools, QStringLiteral("Ceramic Capacitor"));
		TEST_ASSERT_M(capacitorId != 0 && ceramicId != 0, "the seeded §2b example must be there");

		QJsonObject args;
		args["categoryId"] = ceramicId;
		const QJsonObject category = call(tools, "get_category", args);
		TEST_ASSERT_M(isOk(category), messageOf(category));
		TEST_COMPARE(category.value("parentId").toInt(), capacitorId);
		TEST_COMPARE(category.value("domain").toString().toStdString(), std::string("electronic"));

		const QJsonArray attributes = category.value("attributes").toArray();
		QStringList keys;
		QStringList units;
		for (const QJsonValue& entry : attributes)
		{
			keys.append(entry.toObject().value("key").toString());
			units.append(entry.toObject().value("unit").toString());
		}
		TEST_COMPARE(attributes.size(), 4);
		TEST_ASSERT_M(keys.indexOf(QStringLiteral("capacitance")) == 0
			&& keys.indexOf(QStringLiteral("voltage")) == 1
			&& keys.indexOf(QStringLiteral("tolerance")) == 2,
			"the ancestor's attributes come first, in the ancestor's own order");
		TEST_ASSERT_M(keys.indexOf(QStringLiteral("dielectric")) == 3,
			"the category's own attribute comes last");
		TEST_COMPARE(units.at(0).toStdString(), std::string("F"));

		// The declared unit reaches a write on the child, which is what makes "100n" usable there.
		QJsonObject create;
		create["categoryId"] = ceramicId;
		create["name"] = "C 100n X7R 0603";
		const int partId = call(tools, "create_part", create).value("id").toInt();
		TEST_ASSERT(partId != 0);

		QJsonObject setValue;
		setValue["partId"] = partId;
		setValue["key"] = "capacitance";
		setValue["value"] = "100n";
		const QJsonObject stored = call(tools, "set_part_attribute", setValue);
		TEST_ASSERT_M(isOk(stored), messageOf(stored));
		TEST_ASSERT_M(std::abs(stored.value("storedValue").toDouble() - 1e-7) < 1e-13,
			"an inherited attribute is parsed against the unit it was declared with");
		TEST_COMPARE(stored.value("storedUnit").toString().toStdString(), std::string("F"));

		// A text attribute the child declares itself keeps its text rather than being numbered.
		setValue["key"] = "dielectric";
		setValue["value"] = "X7R";
		const QJsonObject text = call(tools, "set_part_attribute", setValue);
		TEST_ASSERT_M(isOk(text), messageOf(text));
		TEST_COMPARE(text.value("storedValue").toString().toStdString(), std::string("X7R"));

		// The file slots resolve the same way — declared on the root, inherited by the leaf.
		TEST_ASSERT_M(category.value("fileSlots").toArray().size() > 0,
			"file slots resolve down the chain too");
	}

	// Tags are a managed vocabulary (§2d): a name that is not in it is reported, not invented.
	TEST_FUNCTION(tagsAreAppliedByNameAndUnknownOnesReported)
	{
		TEST_START;

		ScopedDatabase database("tags");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const QJsonObject listed = call(tools, "list_tags");
		TEST_ASSERT_M(isOk(listed), messageOf(listed));
		const QJsonArray tagList = listed.value("tags").toArray();
		TEST_ASSERT_M(tagList.size() > 0, "a new database seeds its starting tag vocabulary");
		bool sawSmd = false;
		for (const QJsonValue& entry : tagList)
		{
			if (entry.toObject().value("name").toString() == QStringLiteral("SMD"))
			{
				sawSmd = true;
				TEST_COMPARE(entry.toObject().value("category").toString().toStdString(),
					std::string("PCB placement"));
			}
		}
		TEST_ASSERT(sawSmd);

		const int resistorId = categoryIdNamed(tools, QStringLiteral("Resistor"));
		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 1k 0402";
		const int partId = call(tools, "create_part", create).value("id").toInt();
		TEST_ASSERT(partId != 0);

		QJsonObject apply;
		apply["partId"] = partId;
		apply["tags"] = QJsonArray{ "smd", "Not A Tag" };
		const QJsonObject applied = call(tools, "set_part_tags", apply);
		TEST_ASSERT_M(isOk(applied), messageOf(applied));
		TEST_COMPARE(applied.value("applied").toArray().size(), 1);
		TEST_ASSERT_M(arrayContainsText(applied.value("applied").toArray(), QStringLiteral("SMD")),
			"a tag matched case-insensitively is reported in the vocabulary's own spelling");
		TEST_COMPARE(applied.value("unknown").toArray().size(), 1);
		TEST_ASSERT(arrayContainsText(applied.value("unknown").toArray(), QStringLiteral("Not A Tag")));

		const QJsonObject part = call(tools, "get_part", QJsonObject{ { "partId", partId } });
		TEST_ASSERT_M(arrayContainsText(part.value("tags").toArray(), QStringLiteral("SMD")),
			"the good name must survive the bad one in the same call");

		// An empty list is a legitimate instruction, not a missing argument.
		QJsonObject clear;
		clear["partId"] = partId;
		clear["tags"] = QJsonArray();
		TEST_ASSERT(isOk(call(tools, "set_part_tags", clear)));
		TEST_COMPARE(call(tools, "get_part", QJsonObject{ { "partId", partId } })
			.value("tags").toArray().size(), 0);

		// ...but omitting it entirely is not.
		QJsonObject missing;
		missing["partId"] = partId;
		TEST_ASSERT_M(!isOk(call(tools, "set_part_tags", missing)),
			"a missing 'tags' must be an error rather than a silent clear");
	}

	// update_part writes the keys it was given and leaves the rest of the row alone.
	TEST_FUNCTION(updatePartWritesOnlyTheKeysGiven)
	{
		TEST_START;

		ScopedDatabase database("update_part");
		TEST_ASSERT_M(database.handle() != nullptr, "createNew failed: " + database.error());
		const std::vector<PartManager::LlmTool> tools = database.tools();

		const int resistorId = categoryIdNamed(tools, QStringLiteral("Resistor"));
		QJsonObject create;
		create["categoryId"] = resistorId;
		create["name"] = "R 220R";
		create["mpn"] = "CRCW0805220RFKEA";
		create["manufacturer"] = "Vishay";
		create["package"] = "0805";
		create["stockQty"] = 40;
		const QJsonObject created = call(tools, "create_part", create);
		TEST_ASSERT_M(isOk(created), messageOf(created));
		const int partId = created.value("id").toInt();

		QJsonObject rename;
		rename["partId"] = partId;
		rename["name"] = "Resistor 220 Ohm 0805";
		const QJsonObject updated = call(tools, "update_part", rename);
		TEST_ASSERT_M(isOk(updated), messageOf(updated));
		TEST_COMPARE(updated.value("updated").toArray().size(), 1);

		const QJsonObject part = call(tools, "get_part", QJsonObject{ { "partId", partId } });
		TEST_COMPARE(part.value("name").toString().toStdString(), std::string("Resistor 220 Ohm 0805"));
		TEST_ASSERT_M(part.value("mpn").toString().toStdString() == std::string("CRCW0805220RFKEA"),
			"a key that was not passed must keep its value");
		TEST_COMPARE(part.value("manufacturer").toString().toStdString(), std::string("Vishay"));
		TEST_COMPARE(part.value("package").toString().toStdString(), std::string("0805"));
		TEST_COMPARE(part.value("stockQty").toInt(), 40);

		// An update that names no field is a mistake worth saying out loud rather than a no-op
		// the model reads as success.
		QJsonObject empty;
		empty["partId"] = partId;
		const QJsonObject emptyResult = call(tools, "update_part", empty);
		TEST_ASSERT_M(!isOk(emptyResult), "an update with no fields must be refused");
		TEST_ASSERT_M(messageOf(emptyResult).find("stockMinQty") != std::string::npos,
			"the refusal must list the fields that could have been passed");

		// A quoted id is still an id: a local model quotes its integers about as often as not.
		QJsonObject quoted;
		quoted["partId"] = QString::number(partId);
		quoted["stockMinQty"] = 10;
		TEST_ASSERT_M(isOk(call(tools, "update_part", quoted)), "a quoted integer id must be accepted");
		TEST_COMPARE(call(tools, "get_part", QJsonObject{ { "partId", partId } })
			.value("stockMinQty").toInt(), 10);

		// add_category_attribute is idempotent the same way create_category is.
		QJsonObject attribute;
		attribute["categoryId"] = resistorId;
		attribute["key"] = "operating_voltage";
		attribute["label"] = "Operating voltage";
		attribute["datatype"] = "dimension";
		attribute["unit"] = "V";
		const QJsonObject first = call(tools, "add_category_attribute", attribute);
		TEST_ASSERT_M(isOk(first), messageOf(first));
		TEST_ASSERT(first.value("created").toBool());
		const QJsonObject second = call(tools, "add_category_attribute", attribute);
		TEST_ASSERT_M(isOk(second), messageOf(second));
		TEST_ASSERT_M(second.value("id").toInt() == first.value("id").toInt()
			&& !second.value("created").toBool(),
			"adding the same field twice must answer with the field that is already there");

		// ...and an unknown unit is a correction that hands back the list.
		QJsonObject badUnit;
		badUnit["categoryId"] = resistorId;
		badUnit["key"] = "weight";
		badUnit["label"] = "Weight";
		badUnit["datatype"] = "dimension";
		badUnit["unit"] = "stone";
		const QJsonObject badUnitResult = call(tools, "add_category_attribute", badUnit);
		TEST_ASSERT_M(!isOk(badUnitResult), "a unit outside the fixed dropdown must be refused");
		TEST_ASSERT_M(badUnitResult.value("allowed_values").toArray().size() > 0,
			"the refusal must hand back the units that are allowed");
	}

#endif

};

TEST_INSTANTIATE(TST_LlmPartToolset);
