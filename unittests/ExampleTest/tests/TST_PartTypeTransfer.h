#pragma once

#include "UnitTest.h"
#include "import/PartManager_PartTypeTransfer.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#include "database/PartManager_DatabaseMetadata.h"
#include "database/PartManager_SchemaMigrator.h"
#include "persistence/PartManager_ListColumnRepository.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"
#endif

class TST_PartTypeTransfer : public UnitTest::Test
{
	TEST_CLASS(TST_PartTypeTransfer)
public:
	TST_PartTypeTransfer()
		: Test("TST_PartTypeTransfer")
	{
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_PartTypeTransfer::jsonRoundTripIsIdentical);
		ADD_TEST(TST_PartTypeTransfer::fromJsonRefusesABadMarkerAndAFutureVersion);
		ADD_TEST(TST_PartTypeTransfer::subsetExportDragsItsAncestorsAlong);
		ADD_TEST(TST_PartTypeTransfer::exactPathMatchMergesRatherThanDuplicating);
		ADD_TEST(TST_PartTypeTransfer::uniqueLeafNameFallbackMergesAndReparents);
		ADD_TEST(TST_PartTypeTransfer::anAmbiguousLeafNameIsNeverGuessed);
		ADD_TEST(TST_PartTypeTransfer::keepLocalKeepsAndTakeIncomingWrites);
		ADD_TEST(TST_PartTypeTransfer::anIncomingOnlyRequiredAttributeIsFlagged);
		ADD_TEST(TST_PartTypeTransfer::applyMergeNeverTouchesAnyPart);
		ADD_TEST(TST_PartTypeTransfer::defaultTagsImportByName);
		ADD_TEST(TST_PartTypeTransfer::aFailureRollsTheWholeMergeBack);
		ADD_TEST(TST_PartTypeTransfer::aReparentThatWouldCloseACycleIsRefused);
		ADD_TEST(TST_PartTypeTransfer::bundleFromDatabaseFileReadsAPmdbAndRefusesANewerSchema);
		ADD_TEST(TST_PartTypeTransfer::aNameContainingTheSeparatorStaysOneCategory);
		ADD_TEST(TST_PartTypeTransfer::aNameContainingTheSeparatorMergesRatherThanDuplicating);
		ADD_TEST(TST_PartTypeTransfer::aNestedNameContainingTheSeparatorFindsItsParent);
		ADD_TEST(TST_PartTypeTransfer::anArbitraryCategoryNameSurvivesAndMatchesItself);
		ADD_TEST(TST_PartTypeTransfer::caseFoldingIsUnicodeAwareNotByteWise);
		ADD_TEST(TST_PartTypeTransfer::aFormatVersionOneBundleStillLoads);
#endif
	}

private:

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	// A fresh temp database with every table the transfer touches. Same shape as the freshDb()
	// the other repository suites use — a plain temp file, no DatabaseHandle, no folder layout.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		PartManager::PartRepository::createSchema(*db);
		PartManager::ListColumnRepository::createSchema(*db);
		PartManager::TagRepository::createSchema(*db);
		return db;
	}

	static int addType(SQLiteWrapper::SQLite& db, const std::string& name, int parentId = 0,
		const std::string& description = std::string())
	{
		PartManager::PartType type;
		type.name = name;
		type.domain = "electronic";
		type.parentTypeId = parentId;
		type.description = description;
		return PartManager::PartTypeRepository::insertType(db, type);
	}

	static int addAttribute(SQLiteWrapper::SQLite& db, int typeId, const std::string& key,
		const std::string& label, PartManager::AttributeDataType datatype, bool required = false,
		bool searchable = false)
	{
		PartManager::PartTypeAttribute attribute;
		attribute.partTypeId = typeId;
		attribute.key = key;
		attribute.label = label;
		attribute.datatype = datatype;
		attribute.required = required;
		attribute.searchable = searchable;
		return PartManager::PartTypeRepository::insertAttribute(db, attribute);
	}

	static int addPart(SQLiteWrapper::SQLite& db, int typeId, const std::string& name,
		const std::string& attributesJson)
	{
		PartManager::Part part;
		part.partTypeId = typeId;
		part.name = name;
		part.attributes = attributesJson;
		return PartManager::PartRepository::insertPart(db, part);
	}

	// A path is a list of segments now, so these helpers take one. The joined spelling could not
	// survive a category named `Crystal / Oscilator` and is gone from the model entirely.
	static const PartManager::TypeMergeEntry* entryFor(const PartManager::MergePlan& plan,
		const std::vector<std::string>& path)
	{
		for (const PartManager::TypeMergeEntry& entry : plan.entries)
		{
			if (entry.path == path)
			{
				return &entry;
			}
		}
		return nullptr;
	}

	static const PartManager::FieldConflict* conflictFor(const PartManager::TypeMergeEntry& entry,
		const std::string& fieldKey)
	{
		for (const PartManager::FieldConflict& conflict : entry.conflicts)
		{
			if (conflict.fieldKey == fieldKey)
			{
				return &conflict;
			}
		}
		return nullptr;
	}

	static bool entryPathAbsent(const PartManager::PartTypeBundle& bundle,
		const std::vector<std::string>& path)
	{
		for (const PartManager::PartTypeNodeBundle& node : bundle.nodes)
		{
			if (node.path == path)
			{
				return false;
			}
		}
		return true;
	}

	static bool hasString(const std::vector<std::string>& values, const std::string& wanted)
	{
		return std::find(values.begin(), values.end(), wanted) != values.end();
	}

	static const PartManager::PartType* typeNamed(const std::vector<PartManager::PartType>& types,
		const std::string& name)
	{
		for (const PartManager::PartType& type : types)
		{
			if (type.name == name)
			{
				return &type;
			}
		}
		return nullptr;
	}

	// Tests

	// Everything that goes out has to come back: the export is the only copy of a category tree
	// a user will ever hold in their hand, and a field lost in JSON is a field silently dropped
	// from their categories on the far side.
	TEST_FUNCTION(jsonRoundTripIsIdentical)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_roundtrip.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		capacitor.kicadRelevant = true;
		capacitor.kicadCategory = "Capacitors";
		capacitor.description = "Anything that stores charge";
		capacitor.searchKeywords = "C\nCap\nFarad";
		capacitor.excludedKeywords = "Coil";
		capacitor.nameTemplate = "Capacitor {capacitance}";
		const int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);
		TEST_ASSERT(capacitorId != PartManager::NoParentType);

		PartManager::PartTypeAttribute capacitance;
		capacitance.partTypeId = capacitorId;
		capacitance.key = "capacitance";
		capacitance.label = "Capacitance";
		capacitance.unit = "F";
		capacitance.datatype = PartManager::AttributeDataType::Dimension;
		capacitance.searchable = true;
		capacitance.required = true;
		capacitance.tooltip = "In farad";
		capacitance.sortOrder = 3;
		TEST_ASSERT(PartManager::PartTypeRepository::insertAttribute(*db, capacitance) != 0);

		PartManager::PartTypeFileSlot datasheet;
		datasheet.partTypeId = capacitorId;
		datasheet.role = "datasheet";
		datasheet.label = "Datasheet";
		datasheet.required = true;
		datasheet.tooltip = "The PDF";
		datasheet.sortOrder = 1;
		TEST_ASSERT(PartManager::PartTypeRepository::insertFileSlot(*db, datasheet) != 0);

		const int ceramicId = addType(*db, "Ceramic Capacitor", capacitorId);
		PartManager::PartTypeAttribute dielectric;
		dielectric.partTypeId = ceramicId;
		dielectric.key = "dielectric";
		dielectric.label = "Dielectric";
		dielectric.datatype = PartManager::AttributeDataType::Enum;
		dielectric.enumOptions = { "X7R", "C0G", "Y5V" };
		TEST_ASSERT(PartManager::PartTypeRepository::insertAttribute(*db, dielectric) != 0);

		std::vector<PartManager::PartTypeListColumn> columns;
		PartManager::PartTypeListColumn column;
		column.columnKey = "dielectric";
		column.labelOverride = "Diel.";
		column.visible = false;
		column.widthPx = 80;
		columns.push_back(column);
		TEST_ASSERT(PartManager::ListColumnRepository::saveColumns(*db, ceramicId, columns));

		PartManager::Tag smd;
		smd.name = "SMD";
		smd.color = "#2196F3";
		const int smdId = PartManager::TagRepository::insertTag(*db, smd);
		TEST_ASSERT(smdId != PartManager::NoTagId);
		TEST_ASSERT(PartManager::TagRepository::addTypeDefaultTag(*db, ceramicId, smdId));

		const PartManager::PartTypeBundle exported = PartManager::bundleFromDatabase(*db);
		TEST_COMPARE(exported.nodes.size(), static_cast<size_t>(2));
		// Parents precede children, which is what lets the importer resolve a parent in one pass.
		TEST_COMPARE(exported.nodes[0].path, std::vector<std::string>({ "Capacitor" }));
		TEST_COMPARE(exported.nodes[1].path, std::vector<std::string>({ "Capacitor", "Ceramic Capacitor" }));
		// OWN rows only (§2b): the child owns one attribute, not its parent's as well.
		TEST_COMPARE(exported.nodes[1].ownAttributes.size(), static_cast<size_t>(1));
		TEST_COMPARE(exported.nodes[1].ownAttributes[0].key, std::string("dielectric"));
		// Ids are meaningless across databases and must not travel.
		TEST_COMPARE(exported.nodes[0].type.id, PartManager::NoParentType);
		TEST_COMPARE(exported.nodes[1].type.parentTypeId, PartManager::NoParentType);

		const std::string json = PartManager::toJson(exported);
		TEST_ASSERT_M(json.find(PartManager::bundleFormatMarker()) != std::string::npos,
			"the format marker must be in the file");

		PartManager::PartTypeBundle reloaded;
		std::string error;
		TEST_ASSERT_M(PartManager::fromJson(json, reloaded, error), error.c_str());

		TEST_COMPARE(reloaded.formatVersion, exported.formatVersion);
		TEST_COMPARE(reloaded.sourceToolVersion, exported.sourceToolVersion);
		TEST_COMPARE(reloaded.sourceDatabaseName, exported.sourceDatabaseName);
		TEST_COMPARE(reloaded.exportedAt, exported.exportedAt);
		TEST_COMPARE(reloaded.nodes.size(), exported.nodes.size());

		for (size_t i = 0; i < exported.nodes.size(); ++i)
		{
			const PartManager::PartTypeNodeBundle& a = exported.nodes[i];
			const PartManager::PartTypeNodeBundle& b = reloaded.nodes[i];
			TEST_COMPARE(b.path, a.path);
			TEST_COMPARE(b.type.name, a.type.name);
			TEST_COMPARE(b.type.domain, a.type.domain);
			TEST_COMPARE(b.type.kicadRelevant, a.type.kicadRelevant);
			TEST_COMPARE(b.type.kicadCategory, a.type.kicadCategory);
			TEST_COMPARE(b.type.description, a.type.description);
			TEST_COMPARE(b.type.searchKeywords, a.type.searchKeywords);
			TEST_COMPARE(b.type.excludedKeywords, a.type.excludedKeywords);
			TEST_COMPARE(b.type.nameTemplate, a.type.nameTemplate);
			TEST_COMPARE(b.defaultTagNames, a.defaultTagNames);

			TEST_COMPARE(b.ownAttributes.size(), a.ownAttributes.size());
			for (size_t k = 0; k < a.ownAttributes.size(); ++k)
			{
				TEST_COMPARE(b.ownAttributes[k].key, a.ownAttributes[k].key);
				TEST_COMPARE(b.ownAttributes[k].label, a.ownAttributes[k].label);
				TEST_COMPARE(b.ownAttributes[k].unit, a.ownAttributes[k].unit);
				TEST_ASSERT(b.ownAttributes[k].datatype == a.ownAttributes[k].datatype);
				TEST_COMPARE(b.ownAttributes[k].enumOptions, a.ownAttributes[k].enumOptions);
				TEST_COMPARE(b.ownAttributes[k].searchable, a.ownAttributes[k].searchable);
				TEST_COMPARE(b.ownAttributes[k].required, a.ownAttributes[k].required);
				TEST_COMPARE(b.ownAttributes[k].tooltip, a.ownAttributes[k].tooltip);
				TEST_COMPARE(b.ownAttributes[k].sortOrder, a.ownAttributes[k].sortOrder);
			}

			TEST_COMPARE(b.ownFileSlots.size(), a.ownFileSlots.size());
			for (size_t k = 0; k < a.ownFileSlots.size(); ++k)
			{
				TEST_COMPARE(b.ownFileSlots[k].role, a.ownFileSlots[k].role);
				TEST_COMPARE(b.ownFileSlots[k].label, a.ownFileSlots[k].label);
				TEST_COMPARE(b.ownFileSlots[k].required, a.ownFileSlots[k].required);
				TEST_COMPARE(b.ownFileSlots[k].tooltip, a.ownFileSlots[k].tooltip);
				TEST_COMPARE(b.ownFileSlots[k].sortOrder, a.ownFileSlots[k].sortOrder);
			}

			TEST_COMPARE(b.ownListColumns.size(), a.ownListColumns.size());
			for (size_t k = 0; k < a.ownListColumns.size(); ++k)
			{
				TEST_COMPARE(b.ownListColumns[k].columnKey, a.ownListColumns[k].columnKey);
				TEST_COMPARE(b.ownListColumns[k].labelOverride, a.ownListColumns[k].labelOverride);
				TEST_COMPARE(b.ownListColumns[k].visible, a.ownListColumns[k].visible);
				TEST_COMPARE(b.ownListColumns[k].sortOrder, a.ownListColumns[k].sortOrder);
				TEST_COMPARE(b.ownListColumns[k].widthPx, a.ownListColumns[k].widthPx);
			}
		}
	}

	// §1c's stance on a file: an unknown marker is not ours, and a newer version is refused
	// rather than read half-understood.
	TEST_FUNCTION(fromJsonRefusesABadMarkerAndAFutureVersion)
	{
		TEST_START;

		PartManager::PartTypeBundle bundle;
		std::string error;

		TEST_ASSERT_M(!PartManager::fromJson("not json at all", bundle, error), "garbage must be refused");
		TEST_ASSERT(!error.empty());

		TEST_ASSERT_M(!PartManager::fromJson(
			"{\"format\":\"some-other-tool\",\"formatVersion\":1,\"nodes\":[]}", bundle, error),
			"a foreign format marker must be refused");
		TEST_ASSERT(!error.empty());

		TEST_ASSERT_M(!PartManager::fromJson("{\"formatVersion\":1,\"nodes\":[]}", bundle, error),
			"a missing format marker must be refused");

		const std::string future = std::string("{\"format\":\"") + PartManager::bundleFormatMarker()
			+ "\",\"formatVersion\":" + std::to_string(PartManager::CurrentBundleFormatVersion + 1)
			+ ",\"nodes\":[]}";
		TEST_ASSERT_M(!PartManager::fromJson(future, bundle, error), "a newer bundle must be refused");
		TEST_ASSERT_M(error.find("newer") != std::string::npos,
			"the refusal must say the bundle is newer, the way §1c's schema refusal does");

		const std::string good = std::string("{\"format\":\"") + PartManager::bundleFormatMarker()
			+ "\",\"formatVersion\":" + std::to_string(PartManager::CurrentBundleFormatVersion)
			+ ",\"nodes\":[]}";
		TEST_ASSERT_M(PartManager::fromJson(good, bundle, error), error.c_str());
		TEST_COMPARE(bundle.nodes.size(), static_cast<size_t>(0));
	}

	// §2b: a child is not importable without the parent it inherits from, so asking for the child
	// asks for the chain above it too.
	TEST_FUNCTION(subsetExportDragsItsAncestorsAlong)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_subset.db");
		const int passiveId = addType(*db, "Passive");
		const int capacitorId = addType(*db, "Capacitor", passiveId);
		const int ceramicId = addType(*db, "Ceramic Capacitor", capacitorId);
		addType(*db, "Resistor", passiveId);

		const PartManager::PartTypeBundle bundle = PartManager::bundleFromDatabase(*db, { ceramicId });
		TEST_COMPARE(bundle.nodes.size(), static_cast<size_t>(3));
		TEST_COMPARE(bundle.nodes[0].path, std::vector<std::string>({ "Passive" }));
		TEST_COMPARE(bundle.nodes[1].path, std::vector<std::string>({ "Passive", "Capacitor" }));
		TEST_COMPARE(bundle.nodes[2].path, std::vector<std::string>({ "Passive", "Capacitor", "Ceramic Capacitor" }));
		// The sibling nobody asked for stays behind.
		TEST_ASSERT(entryPathAbsent(bundle, std::vector<std::string>({ "Passive", "Resistor" })));
	}

	// Rule 1: the same path, however it is cased or spaced, is the same category.
	TEST_FUNCTION(exactPathMatchMergesRatherThanDuplicating)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_exact.db");
		const int capacitorId = addType(*db, "Capacitor", 0, "local text");
		addType(*db, "Ceramic Capacitor", capacitorId);

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle parent;
		parent.path = { " capacitor " };
		parent.type.name = "capacitor";
		parent.type.domain = "electronic";
		parent.type.description = "incoming text";
		bundle.nodes.push_back(parent);
		PartManager::PartTypeNodeBundle child;
		child.path = { "CAPACITOR", "ceramic capacitor" };
		child.type.name = "ceramic capacitor";
		child.type.domain = "electronic";
		bundle.nodes.push_back(child);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		TEST_COMPARE(plan.entries.size(), static_cast<size_t>(2));
		TEST_ASSERT(plan.entries[0].action == PartManager::TypeMergeAction::MergeInto);
		TEST_ASSERT(plan.entries[1].action == PartManager::TypeMergeAction::MergeInto);
		TEST_COMPARE(plan.unmatchedLocalTypeIds.size(), static_cast<size_t>(0));

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());
		// Two categories in, two categories out — no duplicate "capacitor" next to "Capacitor".
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(2));

		PartManager::PartType merged;
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, capacitorId, merged));
		TEST_COMPARE(merged.description, std::string("incoming text"));
	}

	// Rule 2, the real-world case: a flat `Resistor` here, a `Passive/Resistor` there. One
	// category, filed differently — merge and re-parent, never duplicate.
	TEST_FUNCTION(uniqueLeafNameFallbackMergesAndReparents)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_fallback.db");
		const int resistorId = addType(*db, "Resistor");
		addAttribute(*db, resistorId, "resistance", "Resistance", PartManager::AttributeDataType::Dimension);

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle passive;
		passive.path = { "Passive" };
		passive.type.name = "Passive";
		passive.type.domain = "electronic";
		bundle.nodes.push_back(passive);
		PartManager::PartTypeNodeBundle resistor;
		resistor.path = { "Passive", "Resistor" };
		resistor.type.name = "Resistor";
		resistor.type.domain = "electronic";
		bundle.nodes.push_back(resistor);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		const PartManager::TypeMergeEntry* passiveEntry = entryFor(plan, std::vector<std::string>({ "Passive" }));
		const PartManager::TypeMergeEntry* resistorEntry = entryFor(plan, std::vector<std::string>({ "Passive", "Resistor" }));
		TEST_ASSERT(passiveEntry != nullptr && resistorEntry != nullptr);
		TEST_ASSERT_M(passiveEntry->action == PartManager::TypeMergeAction::AddNew,
			"nothing local is called Passive");
		TEST_ASSERT_M(resistorEntry->action == PartManager::TypeMergeAction::MergeInto,
			"the unique local Resistor is the same category");
		TEST_COMPARE(resistorEntry->localTypeId, resistorId);

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());

		std::vector<PartManager::PartType> types = PartManager::PartTypeRepository::listTypes(*db);
		TEST_COMPARE(types.size(), static_cast<size_t>(2));   // Passive + the one Resistor
		const PartManager::PartType* passiveType = typeNamed(types, "Passive");
		PartManager::PartType movedResistor;
		TEST_ASSERT(passiveType != nullptr);
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, resistorId, movedResistor));
		TEST_COMPARE(movedResistor.parentTypeId, passiveType->id);
		TEST_COMPARE(PartManager::partTypePathOf(*db, resistorId),
			std::vector<std::string>({ "Passive", "Resistor" }));
	}

	// Rule 3: an unsure match refuses rather than guesses (DECISIONS.md, 2026-09-26).
	TEST_FUNCTION(anAmbiguousLeafNameIsNeverGuessed)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_ambiguous.db");
		const int smdId = addType(*db, "SMD");
		const int thtId = addType(*db, "THT");
		addType(*db, "Resistor", smdId);
		addType(*db, "Resistor", thtId);

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle resistor;
		resistor.path = { "Resistor" };
		resistor.type.name = "Resistor";
		resistor.type.domain = "electronic";
		bundle.nodes.push_back(resistor);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		TEST_COMPARE(plan.entries.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(plan.entries[0].action == PartManager::TypeMergeAction::AddNew,
			"two candidates must produce no match at all, not the first one");
		TEST_COMPARE(plan.entries[0].localTypeId, PartManager::NoParentType);
		TEST_ASSERT_M(!plan.warnings.empty(), "the ambiguity must be reported");
		// " › " and not "/": a path is only ever joined to be read by a person now, and it is
		// joined with the same glyph the type picker uses (displayPartTypePath()).
		TEST_ASSERT_M(plan.warnings[0].find("SMD \xE2\x80\xBA Resistor") != std::string::npos
			&& plan.warnings[0].find("THT \xE2\x80\xBA Resistor") != std::string::npos,
			"the warning must name both candidates");

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());
		// A third, root-level Resistor — the honest outcome of "I do not know which one".
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(5));
		PM_UNUSED(thtId);
	}

	TEST_FUNCTION(keepLocalKeepsAndTakeIncomingWrites)
	{
		TEST_START;

		auto build = [&](const std::string& name, int& outTypeId)
		{
			auto db = freshDb(name);
			outTypeId = addType(*db, "Resistor", 0, "local description");
			PartManager::PartType type;
			PartManager::PartTypeRepository::findType(*db, outTypeId, type);
			type.kicadCategory = "LocalCategory";
			PartManager::PartTypeRepository::updateType(*db, type);
			addAttribute(*db, outTypeId, "resistance", "Local Label",
				PartManager::AttributeDataType::Dimension);
			return db;
		};

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle node;
		node.path = { "Resistor" };
		node.type.name = "Resistor";
		node.type.domain = "electronic";
		node.type.description = "incoming description";
		node.type.kicadCategory = "IncomingCategory";
		PartManager::PartTypeAttribute incomingAttribute;
		incomingAttribute.key = "resistance";
		incomingAttribute.label = "Incoming Label";
		incomingAttribute.datatype = PartManager::AttributeDataType::Dimension;
		node.ownAttributes.push_back(incomingAttribute);
		bundle.nodes.push_back(node);

		// --- KeepLocal --------------------------------------------------------------------
		{
			int typeId = 0;
			auto db = build("PartManager_TST_PartTypeTransfer_keeplocal.db", typeId);
			PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
			TEST_COMPARE(plan.entries.size(), static_cast<size_t>(1));
			const PartManager::FieldConflict* description = conflictFor(plan.entries[0], "description");
			TEST_ASSERT_M(description != nullptr, "a differing description must be a conflict");
			TEST_COMPARE(description->localValue, std::string("local description"));
			TEST_COMPARE(description->incomingValue, std::string("incoming description"));
			TEST_ASSERT_M(description->resolution == PartManager::FieldResolution::TakeIncoming,
				"the default leans incoming");
			TEST_ASSERT_M(conflictFor(plan.entries[0], "attribute:resistance:label") != nullptr,
				"a differing attribute label must be a conflict");

			for (PartManager::FieldConflict& conflict : plan.entries[0].conflicts)
			{
				conflict.resolution = PartManager::FieldResolution::KeepLocal;
			}
			std::string error;
			TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());

			PartManager::PartType after;
			TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, typeId, after));
			TEST_COMPARE(after.description, std::string("local description"));
			TEST_COMPARE(after.kicadCategory, std::string("LocalCategory"));
			std::vector<PartManager::PartTypeAttribute> attributes =
				PartManager::PartTypeRepository::listOwnAttributes(*db, typeId);
			TEST_COMPARE(attributes.size(), static_cast<size_t>(1));
			TEST_COMPARE(attributes[0].label, std::string("Local Label"));
		}

		// --- TakeIncoming (the default, left untouched) --------------------------------------
		{
			int typeId = 0;
			auto db = build("PartManager_TST_PartTypeTransfer_takeincoming.db", typeId);
			const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
			std::string error;
			TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());

			PartManager::PartType after;
			TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, typeId, after));
			TEST_COMPARE(after.description, std::string("incoming description"));
			TEST_COMPARE(after.kicadCategory, std::string("IncomingCategory"));
			std::vector<PartManager::PartTypeAttribute> attributes =
				PartManager::PartTypeRepository::listOwnAttributes(*db, typeId);
			TEST_COMPARE(attributes.size(), static_cast<size_t>(1));
			TEST_COMPARE(attributes[0].label, std::string("Incoming Label"));
		}
	}

	// §11: a required attribute that arrives on a category which already holds parts makes every
	// one of those parts incomplete. That belongs on screen before the merge, not after it.
	TEST_FUNCTION(anIncomingOnlyRequiredAttributeIsFlagged)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_required.db");
		const int resistorId = addType(*db, "Resistor");
		addAttribute(*db, resistorId, "resistance", "Resistance", PartManager::AttributeDataType::Dimension);
		addPart(*db, resistorId, "R1", "{}");
		addPart(*db, resistorId, "R2", "{}");
		addPart(*db, resistorId, "R3", "{}");
		// A part of another category must not be counted.
		const int otherId = addType(*db, "Capacitor");
		addPart(*db, otherId, "C1", "{}");

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle node;
		node.path = { "Resistor" };
		node.type.name = "Resistor";
		node.type.domain = "electronic";
		PartManager::PartTypeAttribute resistance;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.datatype = PartManager::AttributeDataType::Dimension;
		node.ownAttributes.push_back(resistance);
		PartManager::PartTypeAttribute tolerance;
		tolerance.key = "tolerance";
		tolerance.label = "Tolerance";
		tolerance.datatype = PartManager::AttributeDataType::Number;
		tolerance.required = true;
		node.ownAttributes.push_back(tolerance);
		PartManager::PartTypeAttribute package;
		package.key = "package";
		package.label = "Package";
		package.datatype = PartManager::AttributeDataType::Text;
		node.ownAttributes.push_back(package);
		bundle.nodes.push_back(node);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		const PartManager::TypeMergeEntry* entry = entryFor(plan, std::vector<std::string>({ "Resistor" }));
		TEST_ASSERT(entry != nullptr);
		TEST_ASSERT(entry->action == PartManager::TypeMergeAction::MergeInto);
		TEST_COMPARE(entry->affectedPartCount, 3);
		TEST_COMPARE(entry->addedAttributeKeys.size(), static_cast<size_t>(2));
		TEST_ASSERT(hasString(entry->addedAttributeKeys, "tolerance"));
		TEST_ASSERT(hasString(entry->addedAttributeKeys, "package"));
		TEST_COMPARE(entry->newRequiredAttributeKeys.size(), static_cast<size_t>(1));
		TEST_COMPARE(entry->newRequiredAttributeKeys[0], std::string("tolerance"));
		// An attribute present on both sides and identical is neither added nor a conflict.
		TEST_ASSERT(!hasString(entry->addedAttributeKeys, "resistance"));
	}

	// The load-bearing invariant: a merge reshapes the template and never the parts filed under
	// it. Every `part.attributes` and every `part.part_type_id` must come out byte-identical.
	TEST_FUNCTION(applyMergeNeverTouchesAnyPart)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_parts.db");
		const int resistorId = addType(*db, "Resistor", 0, "local");
		addAttribute(*db, resistorId, "resistance", "Resistance", PartManager::AttributeDataType::Text);
		addAttribute(*db, resistorId, "legacy", "Legacy", PartManager::AttributeDataType::Text);
		addPart(*db, resistorId, "R1", "{\"resistance\":{\"value\":4700,\"unit\":\"\xE2\x84\xA6\"},\"legacy\":\"keep me\"}");
		addPart(*db, resistorId, "R2", "{\"legacy\":\"also keep me\"}");

		std::map<int, std::pair<int, std::string>> before;
		for (const PartManager::Part& part : PartManager::PartRepository::listParts(*db))
		{
			before[part.id] = { part.partTypeId, part.attributes };
		}
		TEST_COMPARE(before.size(), static_cast<size_t>(2));

		// A merge that renames the category, changes an attribute's data type, adds a required
		// attribute and drops `legacy` from the template entirely.
		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle passive;
		passive.path = { "Passive" };
		passive.type.name = "Passive";
		passive.type.domain = "electronic";
		bundle.nodes.push_back(passive);
		PartManager::PartTypeNodeBundle node;
		node.path = { "Passive", "Resistor" };
		node.type.name = "Resistor";
		node.type.domain = "electronic";
		node.type.description = "incoming";
		PartManager::PartTypeAttribute resistance;
		resistance.key = "resistance";
		resistance.label = "Resistance";
		resistance.unit = "\xE2\x84\xA6";
		resistance.datatype = PartManager::AttributeDataType::Dimension;   // was Text
		resistance.searchable = true;                                      // forces the attr_ column
		node.ownAttributes.push_back(resistance);
		PartManager::PartTypeAttribute tolerance;
		tolerance.key = "tolerance";
		tolerance.label = "Tolerance";
		tolerance.datatype = PartManager::AttributeDataType::Number;
		tolerance.required = true;
		node.ownAttributes.push_back(tolerance);
		bundle.nodes.push_back(node);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		const PartManager::TypeMergeEntry* entry = entryFor(plan, std::vector<std::string>({ "Passive", "Resistor" }));
		TEST_ASSERT(entry != nullptr);
		TEST_ASSERT(entry->action == PartManager::TypeMergeAction::MergeInto);
		// The dangerous change has to be called out by name, key and part count.
		bool sawDatatypeWarning = false;
		for (const std::string& warning : plan.warnings)
		{
			if (warning.find("resistance") != std::string::npos
				&& warning.find("data type") != std::string::npos
				&& warning.find("2 part(s)") != std::string::npos)
			{
				sawDatatypeWarning = true;
			}
		}
		TEST_ASSERT_M(sawDatatypeWarning, "a datatype change on an attribute with parts must warn");

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());

		// The template really did change...
		std::vector<PartManager::PartTypeAttribute> attributes =
			PartManager::PartTypeRepository::listOwnAttributes(*db, resistorId);
		TEST_COMPARE(attributes.size(), static_cast<size_t>(3));   // resistance, legacy, tolerance

		// ...and not one part moved or lost a character of what the user typed.
		std::vector<PartManager::Part> after = PartManager::PartRepository::listParts(*db);
		TEST_COMPARE(after.size(), before.size());
		for (const PartManager::Part& part : after)
		{
			auto hit = before.find(part.id);
			TEST_ASSERT_M(hit != before.end(), "applyMerge must not invent or delete a part");
			TEST_COMPARE(part.partTypeId, hit->second.first);
			TEST_COMPARE(part.attributes, hit->second.second);
		}
	}

	// §2d: tags travel by name, because a tag id means nothing in another database.
	TEST_FUNCTION(defaultTagsImportByName)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_tags.db");
		const int resistorId = addType(*db, "Resistor");
		PartManager::Tag smd;
		smd.name = "SMD";
		smd.color = "#2196F3";
		const int localSmdId = PartManager::TagRepository::insertTag(*db, smd);
		TEST_ASSERT(localSmdId != PartManager::NoTagId);
		TEST_COMPARE(PartManager::TagRepository::listTags(*db).size(), static_cast<size_t>(1));

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle node;
		node.path = { "Resistor" };
		node.type.name = "Resistor";
		node.type.domain = "electronic";
		node.defaultTagNames = { "smd", "Precision" };   // one known locally, one not
		bundle.nodes.push_back(node);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());

		// "smd" reused the existing tag rather than creating a second spelling of it.
		std::vector<PartManager::Tag> tags = PartManager::TagRepository::listTags(*db);
		TEST_COMPARE(tags.size(), static_cast<size_t>(2));

		std::vector<PartManager::Tag> defaults =
			PartManager::TagRepository::listTypeDefaultTags(*db, resistorId);
		TEST_COMPARE(defaults.size(), static_cast<size_t>(2));
		bool sawLocalSmd = false;
		bool sawCreatedPrecision = false;
		for (const PartManager::Tag& tag : defaults)
		{
			if (tag.id == localSmdId && tag.name == "SMD") { sawLocalSmd = true; }
			if (tag.name == "Precision") { sawCreatedPrecision = true; }
		}
		TEST_ASSERT_M(sawLocalSmd, "the local SMD tag must be reused, id and spelling intact");
		TEST_ASSERT_M(sawCreatedPrecision, "a tag the local vocabulary lacks must be created");
	}

	// The reason Deliverable A exists: a merge that fails halfway leaves nothing behind.
	TEST_FUNCTION(aFailureRollsTheWholeMergeBack)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_rollback.db");
		const int resistorId = addType(*db, "Resistor", 0, "untouched");
		PM_UNUSED(resistorId);
		const size_t typesBefore = PartManager::PartTypeRepository::listTypes(*db).size();

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle alpha;
		alpha.path = { "Alpha" };
		alpha.type.name = "Alpha";
		alpha.type.domain = "electronic";
		bundle.nodes.push_back(alpha);
		// An orphan: its parent is neither local nor in this bundle, so the apply must bail.
		PartManager::PartTypeNodeBundle orphan;
		orphan.path = { "NoSuchParent", "Child" };
		orphan.type.name = "Child";
		orphan.type.domain = "electronic";
		bundle.nodes.push_back(orphan);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		std::string error;
		TEST_ASSERT_M(!PartManager::applyMerge(*db, bundle, plan, error), "the merge must fail");
		TEST_ASSERT_M(!error.empty(), "a failed merge must say why");

		// "Alpha" was inserted before the failure and must be gone again.
		std::vector<PartManager::PartType> types = PartManager::PartTypeRepository::listTypes(*db);
		TEST_COMPARE(types.size(), typesBefore);
		TEST_ASSERT_M(typeNamed(types, "Alpha") == nullptr,
			"a write from before the failure must have been rolled back");
		PartManager::PartType resistor;
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, resistorId, resistor));
		TEST_COMPARE(resistor.description, std::string("untouched"));
	}

	// A cycle in parent_type_id breaks every §2b walk in the app. Refuse, do not corrupt.
	TEST_FUNCTION(aReparentThatWouldCloseACycleIsRefused)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_PartTypeTransfer_cycle.db");
		const int parentId = addType(*db, "Alpha");
		const int childId = addType(*db, "Beta", parentId);

		// The incoming tree says the opposite: Alpha lives under Beta.
		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle beta;
		beta.path = { "Beta" };
		beta.type.name = "Beta";
		beta.type.domain = "electronic";
		bundle.nodes.push_back(beta);
		PartManager::PartTypeNodeBundle alpha;
		alpha.path = { "Beta", "Alpha" };
		alpha.type.name = "Alpha";
		alpha.type.domain = "electronic";
		bundle.nodes.push_back(alpha);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		std::string error;
		TEST_ASSERT_M(!PartManager::applyMerge(*db, bundle, plan, error),
			"a re-parent that closes a cycle must be refused");
		TEST_ASSERT_M(error.find("ancestor") != std::string::npos, error.c_str());

		// The tree is exactly as it was.
		PartManager::PartType alphaAfter;
		PartManager::PartType betaAfter;
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, parentId, alphaAfter));
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, childId, betaAfter));
		TEST_COMPARE(alphaAfter.parentTypeId, PartManager::NoParentType);
		TEST_COMPARE(betaAfter.parentTypeId, parentId);
	}

	// B4: the other database, opened read-only through its .pmdb entry file (§1a) and refused
	// when its schema is newer than this build (§1c).
	TEST_FUNCTION(bundleFromDatabaseFileReadsAPmdbAndRefusesANewerSchema)
	{
		TEST_START;

		std::filesystem::path folder =
			std::filesystem::temp_directory_path() / "PartManager_TST_PartTypeTransfer_file";
		std::error_code errorCode;
		std::filesystem::remove_all(folder, errorCode);
		std::filesystem::create_directories(folder, errorCode);

		const std::filesystem::path dbFile = folder / "partmanager.db";
		const std::filesystem::path pmdbFile = folder / "Source.pmdb";

		PartManager::DatabaseMetadataValues values;
		values.schemaVersion = PartManager::CurrentSchemaVersion;
		values.toolVersionLastSaved = "9.9.9";
		values.createdAt = "2026-01-01T00:00:00";
		values.lastOpenedAt = "2026-01-01T00:00:00";

		{
			SQLiteWrapper::SQLite source(dbFile.string());
			TEST_ASSERT(source.open());
			PartManager::PartTypeRepository::createSchema(source);
			PartManager::ListColumnRepository::createSchema(source);
			PartManager::TagRepository::createSchema(source);
			const int capacitorId = addType(source, "Capacitor");
			addType(source, "Ceramic Capacitor", capacitorId);
			TEST_ASSERT(PartManager::DatabaseMetadata::writeDbMeta(source, values));
			source.close();
		}
		TEST_ASSERT(PartManager::DatabaseMetadata::writePmdbFile(pmdbFile.string(), values));

		// The .pmdb entry file resolves to its sibling partmanager.db, exactly as DatabaseHandle does.
		PartManager::PartTypeBundle bundle;
		std::string error;
		TEST_ASSERT_M(PartManager::bundleFromDatabaseFile(pmdbFile.string(), bundle, error), error.c_str());
		TEST_COMPARE(bundle.nodes.size(), static_cast<size_t>(2));
		TEST_COMPARE(bundle.nodes[1].path, std::vector<std::string>({ "Capacitor", "Ceramic Capacitor" }));

		// ...and the .db path works just as well.
		PartManager::PartTypeBundle direct;
		TEST_ASSERT_M(PartManager::bundleFromDatabaseFile(dbFile.string(), direct, error), error.c_str());
		TEST_COMPARE(direct.nodes.size(), static_cast<size_t>(2));

		// A source written by a newer build is refused, not read half-understood (§1c).
		{
			SQLiteWrapper::SQLite source(dbFile.string());
			TEST_ASSERT(source.open());
			values.schemaVersion = PartManager::CurrentSchemaVersion + 1;
			TEST_ASSERT(PartManager::DatabaseMetadata::writeDbMeta(source, values));
			source.close();
		}
		PartManager::PartTypeBundle refused;
		TEST_ASSERT_M(!PartManager::bundleFromDatabaseFile(pmdbFile.string(), refused, error),
			"a newer schema_version must be refused");
		TEST_ASSERT_M(error.find("newer") != std::string::npos, error.c_str());
	}

	// The reported bug, end to end. A user had a category called exactly `Crystal / Oscilator` —
	// the slash is part of the name they typed. With a '/'-joined path, importing it split the one
	// name into a phantom parent `Crystal` and a child `Oscilator` and then refused the import
	// because that parent does not exist. It must cross as ONE root category, name intact.
	TEST_FUNCTION(aNameContainingTheSeparatorStaysOneCategory)
	{
		TEST_START;

		const std::string awkward = "Crystal / Oscilator";

		auto source = freshDb("PartManager_TST_PartTypeTransfer_slash_source.db");
		const int sourceId = addType(*source, awkward, 0, "from the other database");
		addAttribute(*source, sourceId, "frequency", "Frequency",
			PartManager::AttributeDataType::Dimension);

		const PartManager::PartTypeBundle exported = PartManager::bundleFromDatabase(*source);
		TEST_COMPARE(exported.nodes.size(), static_cast<size_t>(1));
		// One segment, not two: the name is the segment, separator and all.
		TEST_COMPARE(exported.nodes[0].path, std::vector<std::string>({ awkward }));

		// Through the file, because that is the route the user's bundle actually took.
		PartManager::PartTypeBundle reloaded;
		std::string error;
		TEST_ASSERT_M(PartManager::fromJson(PartManager::toJson(exported), reloaded, error),
			error.c_str());
		TEST_COMPARE(reloaded.nodes.size(), static_cast<size_t>(1));
		TEST_COMPARE(reloaded.nodes[0].path, std::vector<std::string>({ awkward }));

		auto target = freshDb("PartManager_TST_PartTypeTransfer_slash_target.db");
		const PartManager::MergePlan plan = PartManager::planMerge(*target, reloaded);
		TEST_COMPARE(plan.entries.size(), static_cast<size_t>(1));
		TEST_ASSERT(plan.entries[0].action == PartManager::TypeMergeAction::AddNew);

		// The failure the user saw was here: "its parent category \"crystal\" is neither ...".
		TEST_ASSERT_M(PartManager::applyMerge(*target, reloaded, plan, error), error.c_str());

		std::vector<PartManager::PartType> types =
			PartManager::PartTypeRepository::listTypes(*target);
		TEST_COMPARE(types.size(), static_cast<size_t>(1));
		TEST_COMPARE(types[0].name, awkward);
		TEST_COMPARE(types[0].parentTypeId, PartManager::NoParentType);
		TEST_ASSERT_M(typeNamed(types, "Crystal") == nullptr,
			"no phantom parent may be invented out of a name that contains a separator");
		TEST_ASSERT_M(typeNamed(types, "Oscilator") == nullptr,
			"the name must not be split into a child either");
		TEST_COMPARE(PartManager::partTypePathOf(*target, types[0].id),
			std::vector<std::string>({ awkward }));
	}

	// The same category on both sides is one category: a path match, not a second copy.
	TEST_FUNCTION(aNameContainingTheSeparatorMergesRatherThanDuplicating)
	{
		TEST_START;

		const std::string awkward = "Crystal / Oscilator";

		auto db = freshDb("PartManager_TST_PartTypeTransfer_slash_merge.db");
		const int localId = addType(*db, awkward, 0, "local text");

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle node;
		node.path = { awkward };
		node.type.name = awkward;
		node.type.domain = "electronic";
		node.type.description = "incoming text";
		bundle.nodes.push_back(node);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		const PartManager::TypeMergeEntry* entry = entryFor(plan, { awkward });
		TEST_ASSERT(entry != nullptr);
		TEST_ASSERT_M(entry->action == PartManager::TypeMergeAction::MergeInto,
			"the same path on both sides must match, separator in the name or not");
		TEST_COMPARE(entry->localTypeId, localId);

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(1));

		PartManager::PartType merged;
		TEST_ASSERT(PartManager::PartTypeRepository::findType(*db, localId, merged));
		TEST_COMPARE(merged.description, std::string("incoming text"));
	}

	// The nastier shape: the awkward name is a *child*. Its parent has to resolve to `Passive` and
	// to nothing else — the child's own name must never be read as part of the parent chain.
	TEST_FUNCTION(aNestedNameContainingTheSeparatorFindsItsParent)
	{
		TEST_START;

		const std::string awkward = "Crystal / Oscilator";

		auto source = freshDb("PartManager_TST_PartTypeTransfer_slash_nested_source.db");
		const int passiveId = addType(*source, "Passive");
		addType(*source, awkward, passiveId);

		const PartManager::PartTypeBundle exported = PartManager::bundleFromDatabase(*source);
		TEST_COMPARE(exported.nodes.size(), static_cast<size_t>(2));
		TEST_COMPARE(exported.nodes[1].path, std::vector<std::string>({ "Passive", awkward }));

		PartManager::PartTypeBundle reloaded;
		std::string error;
		TEST_ASSERT_M(PartManager::fromJson(PartManager::toJson(exported), reloaded, error),
			error.c_str());
		TEST_COMPARE(reloaded.nodes[1].path, std::vector<std::string>({ "Passive", awkward }));

		auto target = freshDb("PartManager_TST_PartTypeTransfer_slash_nested_target.db");
		const PartManager::MergePlan plan = PartManager::planMerge(*target, reloaded);
		TEST_ASSERT_M(PartManager::applyMerge(*target, reloaded, plan, error), error.c_str());

		std::vector<PartManager::PartType> types =
			PartManager::PartTypeRepository::listTypes(*target);
		TEST_COMPARE(types.size(), static_cast<size_t>(2));
		const PartManager::PartType* passive = typeNamed(types, "Passive");
		const PartManager::PartType* child = typeNamed(types, awkward);
		TEST_ASSERT(passive != nullptr && child != nullptr);
		TEST_COMPARE(child->parentTypeId, passive->id);
		TEST_COMPARE(PartManager::partTypePathOf(*target, child->id),
			std::vector<std::string>({ "Passive", awkward }));
		PM_UNUSED(passiveId);
	}

	// A category name is arbitrary user text: quotes, backslashes, newlines, per-cent signs, a
	// unit symbol, an emoji. JSON escapes all of it and none of it is ever a key, a pattern or a
	// piece of SQL, so the name that goes out is the name that comes back — and it matches itself.
	TEST_FUNCTION(anArbitraryCategoryNameSurvivesAndMatchesItself)
	{
		TEST_START;

		// "Cap \"X\" \ 50% Ω / Über\n🙂" — written as escapes so the file stays ascii.
		const std::string wild = "Cap \"X\" \\ 50% \xE2\x84\xA6 / " "\xC3\x9C" "ber\n\xF0\x9F\x99\x82";

		auto db = freshDb("PartManager_TST_PartTypeTransfer_wildname.db");
		const int localId = addType(*db, wild, 0, "local text");

		const PartManager::PartTypeBundle exported = PartManager::bundleFromDatabase(*db);
		TEST_COMPARE(exported.nodes.size(), static_cast<size_t>(1));
		TEST_COMPARE(exported.nodes[0].path, std::vector<std::string>({ wild }));

		PartManager::PartTypeBundle reloaded;
		std::string error;
		TEST_ASSERT_M(PartManager::fromJson(PartManager::toJson(exported), reloaded, error),
			error.c_str());
		TEST_COMPARE(reloaded.nodes[0].path, std::vector<std::string>({ wild }));
		TEST_COMPARE(reloaded.nodes[0].type.name, wild);

		// Back into the database it came from: one category, matched by path, not a second copy.
		const PartManager::MergePlan plan = PartManager::planMerge(*db, reloaded);
		TEST_COMPARE(plan.entries.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(plan.entries[0].action == PartManager::TypeMergeAction::MergeInto,
			"a category must match itself whatever its name is made of");
		TEST_COMPARE(plan.entries[0].localTypeId, localId);
		TEST_ASSERT_M(PartManager::applyMerge(*db, reloaded, plan, error), error.c_str());
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(1));
	}

	// Matching is case-insensitive, and "case" has to mean Unicode's idea of it. std::tolower
	// works one byte at a time and would leave `Widerstände` and `WIDERSTÄNDE` as two categories —
	// which, for a user whose categories are in German, is the ordinary case and not a corner one.
	TEST_FUNCTION(caseFoldingIsUnicodeAwareNotByteWise)
	{
		TEST_START;

		const std::string lower = "Widerst" "\xC3\xA4" "nde";   // Widerstände
		const std::string upper = "WIDERST" "\xC3\x84" "NDE";   // WIDERSTÄNDE

		auto db = freshDb("PartManager_TST_PartTypeTransfer_umlaut.db");
		const int localId = addType(*db, lower, 0, "local text");

		PartManager::PartTypeBundle bundle;
		PartManager::PartTypeNodeBundle node;
		node.path = { upper };
		node.type.name = upper;
		node.type.domain = "electronic";
		node.type.description = "incoming text";
		bundle.nodes.push_back(node);

		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		TEST_COMPARE(plan.entries.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(plan.entries[0].action == PartManager::TypeMergeAction::MergeInto,
			"an umlaut in a different case is the same letter and the same category");
		TEST_COMPARE(plan.entries[0].localTypeId, localId);

		std::string error;
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*db).size(), static_cast<size_t>(1));

		// The nested form too: the fallback and the path match share one normalization helper, so
		// neither may fold differently from the other.
		auto nested = freshDb("PartManager_TST_PartTypeTransfer_umlaut_nested.db");
		const int passiveId = addType(*nested, "Passiv");
		const int childId = addType(*nested, lower, passiveId);
		PartManager::PartTypeBundle nestedBundle;
		PartManager::PartTypeNodeBundle parentNode;
		parentNode.path = { "PASSIV" };
		parentNode.type.name = "PASSIV";
		parentNode.type.domain = "electronic";
		nestedBundle.nodes.push_back(parentNode);
		PartManager::PartTypeNodeBundle childNode;
		childNode.path = { "PASSIV", upper };
		childNode.type.name = upper;
		childNode.type.domain = "electronic";
		nestedBundle.nodes.push_back(childNode);

		const PartManager::MergePlan nestedPlan = PartManager::planMerge(*nested, nestedBundle);
		const PartManager::TypeMergeEntry* childEntry = entryFor(nestedPlan, { "PASSIV", upper });
		TEST_ASSERT(childEntry != nullptr);
		TEST_ASSERT(childEntry->action == PartManager::TypeMergeAction::MergeInto);
		TEST_COMPARE(childEntry->localTypeId, childId);
		TEST_ASSERT_M(PartManager::applyMerge(*nested, nestedBundle, nestedPlan, error),
			error.c_str());
		TEST_COMPARE(PartManager::PartTypeRepository::listTypes(*nested).size(),
			static_cast<size_t>(2));
		PM_UNUSED(passiveId);
	}

	// A .pmcat exported before the path became a list still imports: v1 wrote one '/'-joined
	// string and is split back on the way in. Lossy for a name that held a '/' — there is nothing
	// in such a file to recover — but a user's existing export must not simply stop working. The
	// refusal of a version this build does not know has to keep working at the same time (§1c).
	TEST_FUNCTION(aFormatVersionOneBundleStillLoads)
	{
		TEST_START;

		const std::string legacy = std::string("{\"format\":\"") + PartManager::bundleFormatMarker()
			+ "\",\"formatVersion\":1,"
			  "\"sourceToolVersion\":\"0.9.0\",\"sourceDatabaseName\":\"Old\","
			  "\"exportedAt\":\"2026-01-01T00:00:00\","
			  "\"nodes\":[{\"path\":\"Passive\",\"name\":\"Passive\",\"domain\":\"electronic\"},"
			  "{\"path\":\"Passive/Resistor\",\"name\":\"Resistor\",\"domain\":\"electronic\"}]}";

		PartManager::PartTypeBundle bundle;
		std::string error;
		TEST_ASSERT_M(PartManager::fromJson(legacy, bundle, error), error.c_str());
		TEST_COMPARE(bundle.formatVersion, 1);
		TEST_COMPARE(bundle.nodes.size(), static_cast<size_t>(2));
		TEST_COMPARE(bundle.nodes[0].path, std::vector<std::string>({ "Passive" }));
		TEST_COMPARE(bundle.nodes[1].path, std::vector<std::string>({ "Passive", "Resistor" }));

		// It imports, parent and all.
		auto db = freshDb("PartManager_TST_PartTypeTransfer_v1.db");
		const PartManager::MergePlan plan = PartManager::planMerge(*db, bundle);
		TEST_ASSERT_M(PartManager::applyMerge(*db, bundle, plan, error), error.c_str());
		std::vector<PartManager::PartType> types = PartManager::PartTypeRepository::listTypes(*db);
		TEST_COMPARE(types.size(), static_cast<size_t>(2));

		// Written back out it is a v2 file — a path array, and a version that says so.
		const std::string rewritten = PartManager::toJson(bundle);
		TEST_ASSERT_M(rewritten.find("\"formatVersion\": 2") != std::string::npos,
			"a bundle is always written at the current format version");
		PartManager::PartTypeBundle again;
		TEST_ASSERT_M(PartManager::fromJson(rewritten, again, error), error.c_str());
		TEST_COMPARE(again.formatVersion, 2);
		TEST_COMPARE(again.nodes[1].path, std::vector<std::string>({ "Passive", "Resistor" }));

		// And v3 is still refused, exactly as §1c says.
		const std::string future = std::string("{\"format\":\"") + PartManager::bundleFormatMarker()
			+ "\",\"formatVersion\":3,\"nodes\":[]}";
		PartManager::PartTypeBundle refused;
		TEST_ASSERT_M(!PartManager::fromJson(future, refused, error),
			"a bundle format this build does not know must be refused, never guessed at");
		TEST_ASSERT_M(error.find("newer") != std::string::npos, error.c_str());
	}
#endif

};

TEST_INSTANTIATE(TST_PartTypeTransfer);
