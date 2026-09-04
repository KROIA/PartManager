#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_MainWindowController.h"
#include "controllers/PartManager_PartEditorController.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include <filesystem>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
#include "SQLite.h"
#endif

// The rules behind the §2/§11 type template editor, not the widgets: what makes a key
// unusable, what stops a type being deleted, which types the Parent combo must refuse,
// and how an inherited row is told apart from the type's own.
class TST_TypeTemplateEditing : public UnitTest::Test
{
	TEST_CLASS(TST_TypeTemplateEditing)
public:
	TST_TypeTemplateEditing()
		: Test("TST_TypeTemplateEditing")
	{
		ADD_TEST(TST_TypeTemplateEditing::keyMustBeLowercaseIdentifier);
		ADD_TEST(TST_TypeTemplateEditing::parentComboExcludesSelfAndDescendants);
		ADD_TEST(TST_TypeTemplateEditing::deletionIsBlockedByPartsAndByChildren);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_TypeTemplateEditing::keyCollidesWithAnInheritedKey);
		ADD_TEST(TST_TypeTemplateEditing::anInheritedRowIsNotTheTypesOwn);
#endif
	}

private:

	// Tests
	TEST_FUNCTION(keyMustBeLowercaseIdentifier)
	{
		TEST_START;

		const std::vector<PartManager::PartTypeAttribute> none;
		TEST_ASSERT_M(PartManager::attributeKeyProblem("resistance", none)
			== PartManager::AttributeKeyProblem::None, "a plain lowercase key must be accepted");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("vds_max2", none)
			== PartManager::AttributeKeyProblem::None, "digits and underscores must be accepted");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("", none)
			== PartManager::AttributeKeyProblem::Empty, "an empty key must be rejected as empty");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("Resistance", none)
			== PartManager::AttributeKeyProblem::BadFormat, "an uppercase letter must be rejected");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("2pin", none)
			== PartManager::AttributeKeyProblem::BadFormat, "a key must not start with a digit");
		// The key is concatenated into an `attr_<key>` column name, so anything outside
		// [a-z0-9_] has to be refused before it ever reaches an ALTER TABLE.
		TEST_ASSERT_M(PartManager::attributeKeyProblem("res-value", none)
			== PartManager::AttributeKeyProblem::BadFormat, "a hyphen must be rejected");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("res value", none)
			== PartManager::AttributeKeyProblem::BadFormat, "a space must be rejected");

		PartManager::PartTypeAttribute existing;
		existing.key = "resistance";
		TEST_ASSERT_M(PartManager::attributeKeyProblem("resistance", { existing })
			== PartManager::AttributeKeyProblem::Duplicate, "a key already on the type must be rejected");

		// A file slot's role comes from the fixed vocabulary, so only duplication can go wrong.
		PartManager::PartTypeFileSlot slot;
		slot.role = "datasheet";
		TEST_ASSERT_M(PartManager::fileSlotRoleProblem("datasheet", { slot })
			== PartManager::AttributeKeyProblem::Duplicate, "a role already on the type must be rejected");
		TEST_ASSERT_M(PartManager::fileSlotRoleProblem("image", { slot })
			== PartManager::AttributeKeyProblem::None, "an unused role must be accepted");
	}

	TEST_FUNCTION(parentComboExcludesSelfAndDescendants)
	{
		TEST_START;

		// Capacitor -> Ceramic Capacitor -> X7R, plus an unrelated Resistor.
		std::vector<PartManager::PartType> types;
		PartManager::PartType capacitor;  capacitor.id = 1; capacitor.name = "Capacitor";
		PartManager::PartType ceramic;    ceramic.id = 2;   ceramic.name = "Ceramic Capacitor"; ceramic.parentTypeId = 1;
		PartManager::PartType x7r;        x7r.id = 3;       x7r.name = "X7R";                   x7r.parentTypeId = 2;
		PartManager::PartType resistor;   resistor.id = 4;  resistor.name = "Resistor";
		types = { capacitor, ceramic, x7r, resistor };

		// Making Capacitor a child of its own grandchild would leave the §2b walk with no root.
		const std::vector<int> forbidden = PartManager::typeIdWithDescendants(types, 1);
		auto excluded = [&forbidden](int id)
		{
			return std::find(forbidden.begin(), forbidden.end(), id) != forbidden.end();
		};
		TEST_ASSERT_M(excluded(1), "a type must not be offered as its own parent");
		TEST_ASSERT_M(excluded(2), "a direct child must not be offered as a parent");
		TEST_ASSERT_M(excluded(3), "an indirect descendant must not be offered as a parent");
		TEST_ASSERT_M(!excluded(4), "an unrelated type must stay available as a parent");
	}

	TEST_FUNCTION(deletionIsBlockedByPartsAndByChildren)
	{
		TEST_START;

		std::vector<PartManager::PartType> types;
		PartManager::PartType capacitor;  capacitor.id = 1; capacitor.name = "Capacitor";
		PartManager::PartType ceramic;    ceramic.id = 2;   ceramic.name = "Ceramic Capacitor"; ceramic.parentTypeId = 1;
		PartManager::PartType resistor;   resistor.id = 4;  resistor.name = "Resistor";
		types = { capacitor, ceramic, resistor };

		// A child type: its inheritance chain would break.
		const PartManager::TypeDeletionBlock hasChildren = PartManager::typeDeletionBlock(types, 1, 0);
		TEST_ASSERT_M(hasChildren.blocked(), "a type with a child type must not be deletable");
		TEST_COMPARE(hasChildren.childTypeCount, 1);
		TEST_COMPARE(hasChildren.partCount, 0);

		// Parts of the type: their `attributes` JSON is only readable against the template.
		const PartManager::TypeDeletionBlock hasParts = PartManager::typeDeletionBlock(types, 4, 12);
		TEST_ASSERT_M(hasParts.blocked(), "a type with parts must not be deletable");
		TEST_COMPARE(hasParts.partCount, 12);
		TEST_COMPARE(hasParts.childTypeCount, 0);

		// A leaf type nothing points at.
		const PartManager::TypeDeletionBlock free = PartManager::typeDeletionBlock(types, 2, 0);
		TEST_ASSERT_M(!free.blocked(), "a leaf type with no parts must be deletable");
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// Opens a fresh temp SQLite db with the type-template schema.
	static std::unique_ptr<SQLiteWrapper::SQLite> freshDb(const std::string& name)
	{
		std::filesystem::path path = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove(path);
		auto db = std::make_unique<SQLiteWrapper::SQLite>(path.string());
		db->open();
		PartManager::PartTypeRepository::createSchema(*db);
		return db;
	}

	TEST_FUNCTION(keyCollidesWithAnInheritedKey)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TypeTemplateEditing_inherited.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		const int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);
		PartManager::PartType ceramic;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = capacitorId;
		const int ceramicId = PartManager::PartTypeRepository::insertType(*db, ceramic);

		PartManager::PartTypeAttribute capacitance;
		capacitance.partTypeId = capacitorId;
		capacitance.key = "capacitance";
		capacitance.label = "Capacitance";
		PartManager::PartTypeRepository::insertAttribute(*db, capacitance);

		// The child declares nothing of its own, so its own rows are empty — yet "capacitance" is
		// taken. Checking against the type's own rows would let a shadowing row through, and the
		// ancestor's would silently stop being the one the New Part form renders.
		const std::vector<PartManager::PartTypeAttribute> own =
			PartManager::PartTypeRepository::listOwnAttributes(*db, ceramicId);
		TEST_COMPARE(own.size(), static_cast<size_t>(0));

		const std::vector<PartManager::PartTypeAttribute> effective =
			PartManager::PartTypeRepository::effectiveAttributes(*db, ceramicId);
		TEST_ASSERT_M(PartManager::attributeKeyProblem("capacitance", effective)
			== PartManager::AttributeKeyProblem::Duplicate,
			"a key inherited from an ancestor must be reported as a duplicate");
		TEST_ASSERT_M(PartManager::attributeKeyProblem("dielectric", effective)
			== PartManager::AttributeKeyProblem::None,
			"a key nothing in the chain declares must be accepted");
	}

	TEST_FUNCTION(anInheritedRowIsNotTheTypesOwn)
	{
		TEST_START;

		auto db = freshDb("PartManager_TST_TypeTemplateEditing_ownership.db");

		PartManager::PartType capacitor;
		capacitor.name = "Capacitor";
		const int capacitorId = PartManager::PartTypeRepository::insertType(*db, capacitor);
		PartManager::PartType ceramic;
		ceramic.name = "Ceramic Capacitor";
		ceramic.parentTypeId = capacitorId;
		const int ceramicId = PartManager::PartTypeRepository::insertType(*db, ceramic);

		PartManager::PartTypeAttribute capacitance;
		capacitance.partTypeId = capacitorId;
		capacitance.key = "capacitance";
		capacitance.label = "Capacitance";
		PartManager::PartTypeRepository::insertAttribute(*db, capacitance);

		PartManager::PartTypeAttribute dielectric;
		dielectric.partTypeId = ceramicId;
		dielectric.key = "dielectric";
		dielectric.label = "Dielectric";
		PartManager::PartTypeRepository::insertAttribute(*db, dielectric);

		// `partTypeId != the selected type` is the whole test for "inherited" — it is what the
		// editor greys a row on, and what decides whether Remove/Move up/Move down apply to it.
		const std::vector<PartManager::PartTypeAttribute> effective =
			PartManager::PartTypeRepository::effectiveAttributes(*db, ceramicId);
		TEST_COMPARE(effective.size(), static_cast<size_t>(2));
		TEST_COMPARE(effective[0].key, std::string("capacitance"));
		TEST_ASSERT_M(effective[0].partTypeId == capacitorId,
			"the ancestor's row must still name the ancestor as its owner");
		TEST_COMPARE(effective[1].key, std::string("dielectric"));
		TEST_ASSERT_M(effective[1].partTypeId == ceramicId,
			"the type's own row must name the type itself");

		// The same file slot rule, since the two tables get identical treatment in the editor.
		PartManager::PartTypeFileSlot datasheet;
		datasheet.partTypeId = capacitorId;
		datasheet.role = "datasheet";
		datasheet.label = "Datasheet";
		PartManager::PartTypeRepository::insertFileSlot(*db, datasheet);

		// Not `slots` — that is a Qt macro, and the error it produces blames this line's type.
		const std::vector<PartManager::PartTypeFileSlot> slotRows =
			PartManager::PartTypeRepository::effectiveFileSlots(*db, ceramicId);
		TEST_COMPARE(slotRows.size(), static_cast<size_t>(1));
		TEST_ASSERT_M(slotRows[0].partTypeId == capacitorId,
			"an inherited file slot must still name the ancestor as its owner");
	}
#endif

};

TEST_INSTANTIATE(TST_TypeTemplateEditing);
