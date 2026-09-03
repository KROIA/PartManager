#pragma once

#include "UnitTest.h"
#include "controllers/PartManager_PartEditorController.h"
#include "persistence/PartManager_PartRepository.h"
#include "filestore/PartManager_FileStore.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

// The part editor's widget-free logic: the §2a `attributes` JSON read/write, §11 required-field
// validation, the datatype -> widget mapping and the §2d tag set arithmetic. The widgets built
// on top need a live QApplication and are not exercised here.
//
// The §3 datasheet case is the one that needs a real database — it runs against a throwaway
// one created in %TEMP%, never the user's own.
class TST_PartEditorController : public UnitTest::Test
{
	TEST_CLASS(TST_PartEditorController)
public:
	TST_PartEditorController()
		: Test("TST_PartEditorController")
	{
		ADD_TEST(TST_PartEditorController::attributesJsonRoundTrips);
		ADD_TEST(TST_PartEditorController::emptyFieldsAreOmittedNotNulled);
		ADD_TEST(TST_PartEditorController::dimensionKeepsBaseSiValueAndFieldUnit);
		ADD_TEST(TST_PartEditorController::requiredKeysBlockCreation);
		ADD_TEST(TST_PartEditorController::widgetKindFollowsDatatype);
		ADD_TEST(TST_PartEditorController::availableTagsExcludeCarriedOnes);
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		ADD_TEST(TST_PartEditorController::datasheetAttachReplaceAndDetach);
		ADD_TEST(TST_PartEditorController::anImportedDatasheetIsFoundWithoutTheCachedId);
		ADD_TEST(TST_PartEditorController::imageSlotReplacesInPlaceAndDeleteTakesEverythingWithIt);
#endif
	}

private:

	static PartManager::PartTypeAttribute makeAttribute(const std::string& key,
		PartManager::AttributeDataType datatype, const std::string& unit = std::string())
	{
		PartManager::PartTypeAttribute attribute;
		attribute.key = key;
		attribute.label = key;
		attribute.unit = unit;
		attribute.datatype = datatype;
		return attribute;
	}

	// One attribute of every datatype, which is what a round trip has to survive.
	static std::vector<PartManager::PartTypeAttribute> everyDatatype()
	{
		PartManager::PartTypeAttribute choice =
			makeAttribute("mounting", PartManager::AttributeDataType::Enum);
		choice.enumOptions = { "SMD", "THT" };

		return {
			makeAttribute("resistance", PartManager::AttributeDataType::Dimension, "\xCE\xA9"),
			makeAttribute("pins", PartManager::AttributeDataType::Number),
			makeAttribute("note", PartManager::AttributeDataType::Text),
			makeAttribute("rohs", PartManager::AttributeDataType::Bool),
			choice
		};
	}

	static PartManager::Tag makeTag(int id, const std::string& name)
	{
		PartManager::Tag tag;
		tag.id = id;
		tag.name = name;
		tag.color = "#E53935";
		return tag;
	}

	// Tests
	TEST_FUNCTION(attributesJsonRoundTrips)
	{
		TEST_START;

		const std::vector<PartManager::PartTypeAttribute> attributes = everyDatatype();
		const QString stored = QString::fromUtf8(
			"{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"},"
			"\"pins\":8,\"note\":\"thin film\",\"rohs\":true,\"mounting\":\"SMD\"}");

		std::map<std::string, PartManager::AttributeValue> values =
			PartManager::readAttributesJson(stored, attributes);

		TEST_ASSERT(values["resistance"].present);
		TEST_COMPARE(values["resistance"].number, 4700.0);
		TEST_COMPARE(values["pins"].number, 8.0);
		TEST_COMPARE(values["note"].text.toStdString(), std::string("thin film"));
		TEST_ASSERT(values["rohs"].present && values["rohs"].flag);
		TEST_COMPARE(values["mounting"].text.toStdString(), std::string("SMD"));

		// The whole point of this suite: saving then loading must not change the shape.
		const QString written = PartManager::writeAttributesJson(attributes, values);
		std::map<std::string, PartManager::AttributeValue> reread =
			PartManager::readAttributesJson(written, attributes);

		TEST_COMPARE(reread["resistance"].number, 4700.0);
		TEST_COMPARE(reread["pins"].number, 8.0);
		TEST_COMPARE(reread["note"].text.toStdString(), std::string("thin film"));
		TEST_ASSERT(reread["rohs"].flag);
		TEST_COMPARE(reread["mounting"].text.toStdString(), std::string("SMD"));
		// And the text itself must be stable, or the record churns on every save.
		TEST_COMPARE(PartManager::writeAttributesJson(attributes, reread).toStdString(),
			written.toStdString());
	}

	TEST_FUNCTION(emptyFieldsAreOmittedNotNulled)
	{
		TEST_START;

		const std::vector<PartManager::PartTypeAttribute> attributes = everyDatatype();

		// Nothing filled in at all — a Bool still writes, since a checkbox always has an answer.
		std::map<std::string, PartManager::AttributeValue> values =
			PartManager::readAttributesJson("{}", attributes);
		for (const auto& entry : values)
		{
			TEST_ASSERT_M(!entry.second.present, "an absent key must not read as a value");
		}

		const std::string written = PartManager::writeAttributesJson(attributes, values).toStdString();
		TEST_COMPARE(written, std::string("{}"));
		TEST_ASSERT_M(written.find("null") == std::string::npos, "empty fields must not be stored as null");

		// Broken JSON is treated as "nothing filled in" rather than throwing away the form.
		TEST_ASSERT(!PartManager::readAttributesJson("not json at all", attributes)["pins"].present);
	}

	TEST_FUNCTION(dimensionKeepsBaseSiValueAndFieldUnit)
	{
		TEST_START;

		const std::string ohm = "\xCE\xA9";
		const std::vector<PartManager::PartTypeAttribute> attributes{
			makeAttribute("resistance", PartManager::AttributeDataType::Dimension, ohm) };

		PartManager::AttributeValue value;
		value.present = true;
		value.number = 4700.0;    // what the DimensionLineEdit parsed out of "4k7"

		// The chosen interpretation: base-SI number + the field's dropdown unit, which is
		// exactly what PartRepository reads back out for the attr_* column.
		const std::string written =
			PartManager::writeAttributesJson(attributes, { { "resistance", value } }).toStdString();
		TEST_COMPARE(written, "{\"resistance\":{\"unit\":\"" + ohm + "\",\"value\":4700}}");

		// A dimension someone wrote as a bare number is read as already base-SI, not dropped.
		std::map<std::string, PartManager::AttributeValue> lenient =
			PartManager::readAttributesJson("{\"resistance\":4700}", attributes);
		TEST_ASSERT(lenient["resistance"].present);
		TEST_COMPARE(lenient["resistance"].number, 4700.0);
	}

	TEST_FUNCTION(requiredKeysBlockCreation)
	{
		TEST_START;

		PartManager::PartTypeAttribute resistance =
			makeAttribute("resistance", PartManager::AttributeDataType::Dimension, "\xCE\xA9");
		resistance.required = true;
		PartManager::PartTypeAttribute rohs = makeAttribute("rohs", PartManager::AttributeDataType::Bool);
		rohs.required = true;
		PartManager::PartTypeAttribute note = makeAttribute("note", PartManager::AttributeDataType::Text);

		const std::vector<PartManager::PartTypeAttribute> attributes{ resistance, rohs, note };

		// Empty form: only the dimension blocks — a required checkbox always has an answer,
		// and an optional field never blocks.
		std::vector<std::string> missing =
			PartManager::missingRequiredKeys(attributes, PartManager::readAttributesJson("{}", attributes));
		TEST_COMPARE(missing.size(), static_cast<size_t>(1));
		TEST_COMPARE(missing[0], std::string("resistance"));

		// Filled in, and nothing blocks creation anymore.
		TEST_ASSERT(PartManager::missingRequiredKeys(attributes,
			PartManager::readAttributesJson(QString::fromUtf8(
				"{\"resistance\":{\"value\":4700,\"unit\":\"\xCE\xA9\"}}"), attributes)).empty());
	}

	TEST_FUNCTION(widgetKindFollowsDatatype)
	{
		TEST_START;

		TEST_ASSERT(PartManager::widgetKindFor(makeAttribute("a", PartManager::AttributeDataType::Dimension))
			== PartManager::AttributeWidgetKind::Dimension);
		TEST_ASSERT(PartManager::widgetKindFor(makeAttribute("a", PartManager::AttributeDataType::Number))
			== PartManager::AttributeWidgetKind::Number);
		TEST_ASSERT(PartManager::widgetKindFor(makeAttribute("a", PartManager::AttributeDataType::Text))
			== PartManager::AttributeWidgetKind::Text);
		TEST_ASSERT(PartManager::widgetKindFor(makeAttribute("a", PartManager::AttributeDataType::Bool))
			== PartManager::AttributeWidgetKind::Bool);

		PartManager::PartTypeAttribute choice = makeAttribute("a", PartManager::AttributeDataType::Enum);
		// An option-less enum would render a combo box with nothing to pick.
		TEST_ASSERT(PartManager::widgetKindFor(choice) == PartManager::AttributeWidgetKind::Text);
		choice.enumOptions = { "SMD" };
		TEST_ASSERT(PartManager::widgetKindFor(choice) == PartManager::AttributeWidgetKind::Enum);
	}

	TEST_FUNCTION(availableTagsExcludeCarriedOnes)
	{
		TEST_START;

		const std::vector<PartManager::Tag> all{
			makeTag(1, "SMD"), makeTag(2, "THT"), makeTag(3, "Red") };

		std::vector<PartManager::Tag> available =
			PartManager::availableTagsToAdd(all, { makeTag(2, "THT") });
		TEST_COMPARE(available.size(), static_cast<size_t>(2));
		TEST_COMPARE(available[0].name, std::string("SMD"));
		TEST_COMPARE(available[1].name, std::string("Red"));

		// Carrying everything leaves nothing to offer; carrying nothing offers the whole list.
		TEST_ASSERT(PartManager::availableTagsToAdd(all, all).empty());
		TEST_COMPARE(PartManager::availableTagsToAdd(all, {}).size(), static_cast<size_t>(3));
		// Matching is by id, so a renamed-but-same-id tag is still "already carried".
		TEST_COMPARE(PartManager::availableTagsToAdd(all, { makeTag(1, "renamed") }).size(),
			static_cast<size_t>(2));
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	// The §3 datasheet slot end to end on a throwaway database: what the editor's Attach /
	// Replace / Remove buttons call, minus the widgets. No network — the download path is
	// exercised only through its "there is no URL" rejection.
	TEST_FUNCTION(datasheetAttachReplaceAndDetach)
	{
		TEST_START;

		std::filesystem::path parent =
			std::filesystem::temp_directory_path() / "PartManager_TST_PartEditorController_files";
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Files", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		std::vector<PartManager::PartType> types = controller.types();
		TEST_ASSERT_M(!types.empty(), "createNew must seed at least one type template");

		PartManager::Part part;
		part.partTypeId = types.front().id;
		part.name = "LM358";
		part.id = controller.createPart(part);
		TEST_ASSERT_M(part.id != 0, "the part under test could not be created");
		TEST_COMPARE(part.datasheetFileId, 0);

		// Attach.
		const std::filesystem::path source = parent / "lm358.pdf";
		std::ofstream(source, std::ios::binary) << "%PDF-1.4 first datasheet";
		const int fileId = controller.attachDatasheet(part, source.string(), &error);
		TEST_ASSERT_M(fileId != 0, "attachDatasheet failed: " + error);
		TEST_COMPARE(part.datasheetFileId, fileId);

		PartManager::PartFile row;
		TEST_ASSERT(controller.datasheetFile(part, row));
		TEST_COMPARE(row.role, std::string("datasheet"));
		TEST_COMPARE(row.originalFilename, std::string("lm358.pdf"));
		TEST_COMPARE(row.mimeType, std::string("application/pdf"));

		const std::string storedPath = controller.datasheetPath(part);
		TEST_ASSERT_M(!storedPath.empty(), "the attached datasheet must be on disk");

		// The id has to survive the §10 autosave write, or reopening the editor loses the file.
		TEST_ASSERT(controller.savePart(part));
		PartManager::Part reloaded;
		TEST_ASSERT(controller.loadPart(part.id, reloaded));
		TEST_COMPARE(reloaded.datasheetFileId, fileId);

		// Replace: new row, new file, and the old one is gone rather than orphaned.
		const std::filesystem::path replacement = parent / "lm358-revB.pdf";
		std::ofstream(replacement, std::ios::binary) << "%PDF-1.4 second datasheet";
		const int replacedId = controller.attachDatasheet(reloaded, replacement.string(), &error);
		TEST_ASSERT_M(replacedId != 0, "replacing the datasheet failed: " + error);
		TEST_ASSERT_M(replacedId != fileId, "a replacement must be its own part_file row");
		TEST_ASSERT_M(!std::filesystem::exists(storedPath), "the replaced file must not stay behind");
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(1));

		const std::string replacedPath = controller.datasheetPath(reloaded);
		TEST_ASSERT_M(!replacedPath.empty(), "the replacement must be on disk");

		// An empty URL is the normal Mouser answer (§6): it must fail, and change nothing.
		TEST_COMPARE(controller.downloadDatasheet(reloaded, "", &error), 0);
		TEST_ASSERT_M(!error.empty(), "a failed download must carry a reason");
		TEST_COMPARE(reloaded.datasheetFileId, replacedId);

		// Detach.
		TEST_ASSERT(controller.detachDatasheet(reloaded));
		TEST_COMPARE(reloaded.datasheetFileId, 0);
		TEST_ASSERT_M(!std::filesystem::exists(replacedPath), "the last reference must remove the file");
		TEST_ASSERT(controller.savePart(reloaded));
		TEST_ASSERT(controller.loadPart(part.id, reloaded));
		TEST_COMPARE(reloaded.datasheetFileId, 0);
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(0));

		// Nothing attached: detaching again is a no-op, and there is no path to open.
		TEST_ASSERT(!controller.detachDatasheet(reloaded));
		TEST_ASSERT(controller.datasheetPath(reloaded).empty());
	}

	// A datasheet attached the way PartImport attaches one: a `part_file` row and no
	// `part.datasheet_file_id`. The part table's Files column counts rows and showed the glyph
	// while the editor read only the column and said there was none — reported from the running
	// app on 2026-09-03, on every one of the 38 imported parts.
	TEST_FUNCTION(anImportedDatasheetIsFoundWithoutTheCachedId)
	{
		TEST_START;

		std::filesystem::path parent =
			std::filesystem::temp_directory_path() / "PartManager_TST_PartEditorController_import";
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Imported", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		PartManager::Part part;
		part.partTypeId = controller.types().front().id;
		part.name = "DMG1013T-7";
		part.id = controller.createPart(part);
		TEST_ASSERT(part.id != 0);

		// Straight through FileStore, exactly as the importer does — no column written.
		const std::filesystem::path source = parent / "DMG1013T.pdf";
		std::ofstream(source, std::ios::binary) << "%PDF-1.4 imported datasheet";
		PartManager::FileStore store(handle->filestorePath());
		const int rowId = store.attachFile(handle->connection(), part.id,
			PartManager::PartFileRole::Datasheet, source.string(), &error);
		TEST_ASSERT_M(rowId != 0, error);
		TEST_COMPARE(part.datasheetFileId, 0);

		PartManager::PartFile row;
		TEST_ASSERT_M(controller.datasheetFile(part, row),
			"the editor must see the datasheet the list is already showing a glyph for");
		TEST_COMPARE(row.id, rowId);
		TEST_ASSERT_M(!controller.datasheetPath(part).empty(), "and be able to open it");

		// Replacing over it must not leave the invisible row behind — that is how the same part
		// ended up with two datasheet rows in a single-slot role.
		const std::filesystem::path replacement = parent / "DMG1013T-revB.pdf";
		std::ofstream(replacement, std::ios::binary) << "%PDF-1.4 replacement";
		const int replacedId = controller.attachDatasheet(part, replacement.string(), &error);
		TEST_ASSERT_M(replacedId != 0, error);
		TEST_ASSERT_M(replacedId != rowId, "a replacement must be its own row");
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(1));
		// And the new one must still be readable, rather than the row that was just deleted.
		TEST_ASSERT_M(!controller.datasheetPath(part).empty(),
			"the replacement must be the row the part now points at");

		handle.reset();
		std::filesystem::remove_all(parent, ec);
	}

	// The two single-slot roles that have no column on `part` pointing at them, plus the delete
	// path that has to clear everything hanging off a part — a leftover seller link would hand
	// the next part to reuse the id somebody else's Mouser article number.
	TEST_FUNCTION(imageSlotReplacesInPlaceAndDeleteTakesEverythingWithIt)
	{
		TEST_START;

		std::filesystem::path parent =
			std::filesystem::temp_directory_path() / "PartManager_TST_PartEditorController_roles";
		std::error_code ec;
		std::filesystem::remove_all(parent, ec);
		std::filesystem::create_directories(parent, ec);

		std::string error;
		std::unique_ptr<PartManager::DatabaseHandle> handle =
			PartManager::DatabaseHandle::createNew(parent.string(), "Roles", error);
		TEST_ASSERT_M(handle != nullptr, "createNew failed: " + error);

		PartManager::PartEditorController controller(handle.get());
		std::vector<PartManager::PartType> types = controller.types();
		TEST_ASSERT_M(!types.empty(), "createNew must seed at least one type template");

		PartManager::Part part;
		part.partTypeId = types.front().id;
		part.name = "DRV5053";
		part.mpn = "DRV5053CAQLPGM";
		part.id = controller.createPart(part);
		TEST_ASSERT_M(part.id != 0, "the part under test could not be created");

		const std::filesystem::path photo = parent / "drv5053.png";
		std::ofstream(photo, std::ios::binary) << "\x89PNG not really an image";
		const int imageId = controller.attachRoleFile(part.id, PartManager::PartFileRole::Image,
			photo.string(), &error);
		TEST_ASSERT_M(imageId != 0, "attaching the image failed: " + error);

		PartManager::PartFile row;
		TEST_ASSERT(controller.roleFile(part.id, PartManager::PartFileRole::Image, row));
		TEST_COMPARE(row.role, std::string("image"));
		const std::string firstPath = controller.roleFilePath(part.id, PartManager::PartFileRole::Image);
		TEST_ASSERT_M(!firstPath.empty(), "the attached image must be on disk");

		// Replacing keeps exactly one row for the slot — two would make roleFile() a coin flip.
		const std::filesystem::path better = parent / "drv5053-hires.png";
		std::ofstream(better, std::ios::binary) << "\x89PNG a different not-really-an-image";
		const int replacedId = controller.attachRoleFile(part.id, PartManager::PartFileRole::Image,
			better.string(), &error);
		TEST_ASSERT_M(replacedId != 0 && replacedId != imageId,
			"a replacement must be its own part_file row: " + error);
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(1));
		TEST_ASSERT_M(!std::filesystem::exists(firstPath), "the replaced image must not stay behind");

		// A 3D model is a separate slot, so attaching one must not disturb the image.
		const std::filesystem::path model = parent / "drv5053.stl";
		std::ofstream(model, std::ios::binary) << "solid drv5053\nendsolid drv5053\n";
		TEST_ASSERT(controller.attachModel3D(part.id, model.string(), &error) != 0);
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(2));
		TEST_ASSERT(!controller.roleFilePath(part.id, PartManager::PartFileRole::Image).empty());

		// The table's thumbnail query has to see it without knowing the part id up front.
		TEST_COMPARE(PartManager::PartRepository::listFilesWithRole(handle->connection(),
			PartManager::PartFileRole::Image).size(), static_cast<size_t>(1));

		TEST_ASSERT(controller.linkToMouser(part.id, "595-DRV5053CAQLPGM", "", {}) != 0);
		TEST_COMPARE(controller.mouserPartNumber(part.id), std::string("595-DRV5053CAQLPGM"));

		// Delete: the part, its files and its seller link all go.
		TEST_ASSERT(controller.deletePart(part.id));
		PartManager::Part gone;
		TEST_ASSERT_M(!controller.loadPart(part.id, gone), "the deleted part must not load");
		TEST_COMPARE(PartManager::PartRepository::listFiles(handle->connection(), part.id).size(),
			static_cast<size_t>(0));
		TEST_COMPARE(controller.sellerLinks(part.id).size(), static_cast<size_t>(0));
		TEST_COMPARE(PartManager::PartRepository::listFilesWithRole(handle->connection(),
			PartManager::PartFileRole::Image).size(), static_cast<size_t>(0));
		// Deleting twice is a no-op, not a crash — the button is still there after the first one.
		TEST_ASSERT(!controller.deletePart(0));
	}
#endif

};

TEST_INSTANTIATE(TST_PartEditorController);
