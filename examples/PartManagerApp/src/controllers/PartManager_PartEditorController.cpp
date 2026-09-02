#include "controllers/PartManager_PartEditorController.h"

#include "filestore/PartManager_FileStore.h"
#include "import/PartManager_EcadArchive.h"
#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_SellerRepository.h"
#include "persistence/PartManager_TagRepository.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <algorithm>
#include <cstdio>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

	AttributeWidgetKind widgetKindFor(const PartTypeAttribute& attribute)
	{
		switch (attribute.datatype)
		{
		case AttributeDataType::Dimension:
			return AttributeWidgetKind::Dimension;
		case AttributeDataType::Number:
			return AttributeWidgetKind::Number;
		case AttributeDataType::Bool:
			return AttributeWidgetKind::Bool;
		case AttributeDataType::Enum:
			// An option-less enum would render a combo the user cannot pick anything from.
			return attribute.enumOptions.empty() ? AttributeWidgetKind::Text : AttributeWidgetKind::Enum;
		case AttributeDataType::Text:
		default:
			return AttributeWidgetKind::Text;
		}
	}

	std::map<std::string, AttributeValue> readAttributesJson(const QString& attributesJson,
		const std::vector<PartTypeAttribute>& attributes)
	{
		QJsonObject root = QJsonDocument::fromJson(attributesJson.toUtf8()).object();

		std::map<std::string, AttributeValue> values;
		for (const PartTypeAttribute& attribute : attributes)
		{
			AttributeValue value;
			QJsonValue stored = root.value(QString::fromStdString(attribute.key));

			// A dimension is an object; tolerate a bare number too, reading it as already base-SI.
			if (stored.isObject())
			{
				stored = stored.toObject().value("value");
			}

			switch (widgetKindFor(attribute))
			{
			case AttributeWidgetKind::Dimension:
			case AttributeWidgetKind::Number:
				if (stored.isDouble())
				{
					value.present = true;
					value.number = stored.toDouble();
				}
				break;
			case AttributeWidgetKind::Bool:
				if (stored.isBool())
				{
					value.present = true;
					value.flag = stored.toBool();
				}
				break;
			case AttributeWidgetKind::Text:
			case AttributeWidgetKind::Enum:
				if (stored.isString() && !stored.toString().isEmpty())
				{
					value.present = true;
					value.text = stored.toString(); // user data
				}
				break;
			}

			values[attribute.key] = value;
		}
		return values;
	}

	QString writeAttributesJson(const std::vector<PartTypeAttribute>& attributes,
		const std::map<std::string, AttributeValue>& values)
	{
		QJsonObject root;
		for (const PartTypeAttribute& attribute : attributes)
		{
			auto it = values.find(attribute.key);
			if (it == values.end() || !it->second.present)
			{
				continue;   // an empty field is absent from the JSON, not null
			}
			const AttributeValue& value = it->second;
			const QString key = QString::fromStdString(attribute.key);

			switch (widgetKindFor(attribute))
			{
			case AttributeWidgetKind::Dimension:
			{
				// The one shape PartRepository reads back for the attr_* column (see header note).
				QJsonObject entry;
				entry.insert("value", value.number);
				entry.insert("unit", QString::fromStdString(attribute.unit));
				root.insert(key, entry);
				break;
			}
			case AttributeWidgetKind::Number:
				root.insert(key, value.number);
				break;
			case AttributeWidgetKind::Bool:
				root.insert(key, value.flag);
				break;
			case AttributeWidgetKind::Text:
			case AttributeWidgetKind::Enum:
				root.insert(key, value.text);
				break;
			}
		}
		return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
	}

	std::vector<std::string> missingRequiredKeys(const std::vector<PartTypeAttribute>& attributes,
		const std::map<std::string, AttributeValue>& values)
	{
		std::vector<std::string> missing;
		for (const PartTypeAttribute& attribute : attributes)
		{
			if (!attribute.required)
			{
				continue;
			}
			// A checkbox is never "unanswered", so a required Bool can never block creation.
			if (widgetKindFor(attribute) == AttributeWidgetKind::Bool)
			{
				continue;
			}
			auto it = values.find(attribute.key);
			if (it == values.end() || !it->second.present)
			{
				missing.push_back(attribute.key);
			}
		}
		return missing;
	}

	std::vector<Tag> availableTagsToAdd(const std::vector<Tag>& allTags, const std::vector<Tag>& partTags)
	{
		std::vector<Tag> available;
		for (const Tag& tag : allTags)
		{
			auto sameId = [&tag](const Tag& carried) { return carried.id == tag.id; };
			if (std::find_if(partTags.begin(), partTags.end(), sameId) == partTags.end())
			{
				available.push_back(tag);
			}
		}
		return available;
	}

	PartEditorController::PartEditorController(DatabaseHandle* handle)
		: m_handle(handle)
	{
	}

	std::string PartEditorController::mouserPageUrl(const std::string& mouserPartNumber,
		const std::string& storedUrl)
	{
		if (!storedUrl.empty())
		{
			// The exact page the part was created from (§6's ProductDetailUrl). Always better
			// than a search, which can land on a packaging variant.
			return storedUrl;
		}
		if (mouserPartNumber.empty())
		{
			return std::string();
		}
		// A hand-typed number has no product page recorded, so search for it. Percent-encoding
		// the term because a Mouser article number may contain '#' and '/', both of which would
		// otherwise truncate the URL.
		std::string encoded;
		for (unsigned char c : mouserPartNumber)
		{
			const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
			if (safe)
			{
				encoded += static_cast<char>(c);
			}
			else
			{
				char buffer[4] = { 0 };
				std::snprintf(buffer, sizeof(buffer), "%%%02X", c);
				encoded += buffer;
			}
		}
		return "https://www.mouser.com/c/?q=" + encoded;
	}

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// Every method below is a no-op on a closed/absent handle rather than a crash —
		// the dialogs are constructed from the main window, which can outlive a close.
		SQLiteWrapper::SQLite* connectionOf(DatabaseHandle* handle)
		{
			return (handle && handle->isOpen()) ? &handle->connection() : nullptr;
		}
	}

	std::vector<PartType> PartEditorController::types() const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? PartTypeRepository::listTypes(*db) : std::vector<PartType>();
	}

	std::vector<PartTypeAttribute> PartEditorController::attributesFor(int typeId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? PartTypeRepository::effectiveAttributes(*db, typeId) : std::vector<PartTypeAttribute>();
	}

	std::vector<PartTypeFileSlot> PartEditorController::fileSlotsFor(int typeId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? PartTypeRepository::effectiveFileSlots(*db, typeId) : std::vector<PartTypeFileSlot>();
	}

	bool PartEditorController::loadPart(int partId, Part& outPart) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && PartRepository::findPart(*db, partId, outPart);
	}

	bool PartEditorController::savePart(const Part& part) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && PartRepository::updatePart(*db, part);
	}

	int PartEditorController::createPart(const Part& part) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? PartRepository::insertPart(*db, part) : 0;
	}

	namespace
	{
		// FileStore is a root path plus a timeout — cheap enough to build per call, and building
		// it fresh means a database switch can never leave a store pointing at the old folder.
		FileStore storeOf(DatabaseHandle* handle)
		{
			return FileStore(handle ? handle->filestorePath() : std::string());
		}
	}

	int PartEditorController::attachRoleFile(int partId, PartFileRole role,
		const std::string& sourcePath, std::string* outError) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0)
		{
			if (outError)
			{
				*outError = "No open database.";
			}
			return 0;
		}

		// The single-slot rule lives in FileStore, so the library generator — which writes these
		// same slots when it syncs a KiCad edit back — cannot disagree with the editor about it.
		FileStore store = storeOf(m_handle);
		return store.replaceRoleFile(*db, partId, role, sourcePath, outError);
	}

	int PartEditorController::attachRoleBytes(int partId, PartFileRole role,
		const std::string& bytes, const std::string& filename, std::string* outError) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0)
		{
			if (outError)
			{
				*outError = "No open database.";
			}
			return 0;
		}
		if (bytes.empty())
		{
			if (outError)
			{
				*outError = "Nothing to attach.";
			}
			return 0;
		}

		FileStore store = storeOf(m_handle);
		return store.replaceRoleFileBytes(*db, partId, role, bytes, filename, outError);
	}

	int PartEditorController::downloadRoleFile(int partId, PartFileRole role,
		const std::string& url, std::string* outError) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0 || url.empty())
		{
			if (outError)
			{
				*outError = url.empty() ? "No URL to download from." : "No open database.";
			}
			return 0;
		}

		FileStore store = storeOf(m_handle);
		// The download itself is not a file yet, so it cannot go through replaceRoleFile() —
		// but the bytes it produces can, which is what keeps the slot rule in one place.
		const FileStoreResult downloaded = store.downloadFile(url);
		if (!downloaded.ok)
		{
			if (outError)
			{
				*outError = downloaded.errorMessage;
			}
			return 0;
		}
		return store.adoptStoredFile(*db, partId, role, downloaded, outError);
	}

	bool PartEditorController::roleFile(int partId, PartFileRole role, PartFile& outFile) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && FileStore::roleFile(*db, partId, role, outFile);
	}

	std::string PartEditorController::roleFilePath(int partId, PartFileRole role) const
	{
		PartFile file;
		if (!roleFile(partId, role, file))
		{
			return std::string();
		}
		// absolutePath() returns empty when the file is gone from disk, which is exactly what
		// the viewer needs in order to say so rather than draw nothing.
		return storeOf(m_handle).absolutePath(file.relativePath);
	}

	bool PartEditorController::detachRoleFile(int partId, PartFileRole role) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		PartFile file;
		if (!db || !roleFile(partId, role, file))
		{
			return false;
		}
		return storeOf(m_handle).detachFile(*db, file.id);
	}

	PartEditorController::EcadImportSummary PartEditorController::importEcadArchive(int partId,
		const std::string& zipPath) const
	{
		EcadImportSummary summary;
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0)
		{
			summary.errorMessage = "No open database.";
			return summary;
		}

		const EcadArchivePayload payload = EcadArchive::read(zipPath);
		if (!payload.contents.errorMessage.empty())
		{
			summary.errorMessage = payload.contents.errorMessage;
			return summary;
		}
		summary.legacyKicadOnly = payload.contents.legacyKicadOnly;
		summary.ignoredEntries = payload.contents.ignoredEntries;

		FileStore store = storeOf(m_handle);
		auto attach = [&](const std::string& bytes, const std::string& name, PartFileRole role)
		{
			return !bytes.empty()
				&& store.replaceRoleFileBytes(*db, partId, role, bytes, name) != 0;
		};
		summary.symbolAttached =
			attach(payload.symbolBytes, payload.symbolName, PartFileRole::KicadSymbol);
		summary.footprintAttached =
			attach(payload.footprintBytes, payload.footprintName, PartFileRole::KicadFootprint);
		summary.modelAttached =
			attach(payload.modelBytes, payload.modelName, PartFileRole::Kicad3DModel);

		summary.ok = true;
		return summary;
	}

	bool PartEditorController::deletePart(int partId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0)
		{
			return false;
		}
		// The seller links first: PartRepository::deletePart() predates §3's seller tables and
		// does not know about them, and foreign keys are never enforced on this connection, so
		// nothing else would clear them.
		for (const PartSellerLink& link : SellerRepository::linksForPart(*db, partId))
		{
			SellerRepository::removeLink(*db, link.id);
		}
		// The stored files stay on disk — they are content-addressed and may back another part's
		// row. Only the part_file rows go, which is what deletePart() already does.
		return PartRepository::deletePart(*db, partId);
	}

	int PartEditorController::linkToMouser(int partId, const std::string& mouserPartNumber,
		const std::string& url, const std::vector<PriceObservation>& quote) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0 || mouserPartNumber.empty())
		{
			// No article number means no link worth writing: a row with an empty
			// seller_part_number would satisfy "has a Mouser link" while still being unorderable.
			return NoPartSellerLinkId;
		}

		PartSellerLink link;
		link.partId = partId;
		link.sellerId = SellerRepository::ensureMouserSeller(*db);
		link.sellerPartNumber = mouserPartNumber;
		link.url = url;
		// The first (usually only) seller a part gets is the one the order path should use.
		link.isPrimary = true;
		const int linkId = SellerRepository::linkPart(*db, link);
		if (linkId != NoPartSellerLinkId && !quote.empty())
		{
			SellerRepository::recordQuote(*db, linkId, quote);
		}
		return linkId;
	}

	std::vector<PartSellerLink> PartEditorController::sellerLinks(int partId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? SellerRepository::linksForPart(*db, partId) : std::vector<PartSellerLink>();
	}

	std::string PartEditorController::mouserPartNumber(int partId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? SellerRepository::mouserPartNumber(*db, partId) : std::string();
	}

	std::string PartEditorController::mouserUrl(int partId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db)
		{
			return std::string();
		}
		const int mouserSeller = SellerRepository::ensureMouserSeller(*db);
		for (const PartSellerLink& link : SellerRepository::linksForPart(*db, partId))
		{
			if (link.sellerId == mouserSeller && !link.url.empty())
			{
				return link.url;
			}
		}
		return std::string();
	}

	bool PartEditorController::setMouserPartNumber(int partId, const std::string& number,
		const std::string& url) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || partId == 0)
		{
			return false;
		}

		const int mouserSeller = SellerRepository::ensureMouserSeller(*db);
		const std::string existing = SellerRepository::mouserPartNumber(*db, partId);
		if (existing == number)
		{
			// Nothing changed. Rewriting would clear a stored ProductDetailUrl that the caller
			// (an editor field carrying only the number) has no way to supply again.
			return true;
		}

		// The old link goes whether or not a new one replaces it: a part has one Mouser article
		// number, and leaving the previous one behind would make mouserPartNumber() ambiguous.
		for (const PartSellerLink& link : SellerRepository::linksForPart(*db, partId))
		{
			if (link.sellerId == mouserSeller)
			{
				SellerRepository::removeLink(*db, link.id);
			}
		}
		if (number.empty())
		{
			return true;
		}
		return linkToMouser(partId, number, url, std::vector<PriceObservation>())
			!= NoPartSellerLinkId;
	}

	std::vector<Part> PartEditorController::partsWithMpn(const std::string& mpn,
		int exceptPartId) const
	{
		std::vector<Part> matches;
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || mpn.empty())
		{
			// A part with no MPN duplicates nothing — otherwise every blank-MPN part would
			// "duplicate" every other one.
			return matches;
		}
		for (const Part& part : PartRepository::listParts(*db))
		{
			if (part.id != exceptPartId && part.mpn == mpn)
			{
				matches.push_back(part);
			}
		}
		return matches;
	}


	int PartEditorController::attachDatasheet(Part& part, const std::string& sourcePath, std::string* outError) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || part.id == 0)
		{
			if (outError)
			{
				*outError = "No open database.";
			}
			return 0;
		}

		FileStore store = storeOf(m_handle);
		// Import first, detach second: a failed import then leaves the old datasheet in place.
		const int fileId = store.attachFile(*db, part.id, PartFileRole::Datasheet, sourcePath, outError);
		if (fileId == 0)
		{
			return 0;
		}
		detachDatasheet(part);
		part.datasheetFileId = fileId;
		return fileId;
	}

	int PartEditorController::downloadDatasheet(Part& part, const std::string& url, std::string* outError) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || part.id == 0)
		{
			if (outError)
			{
				*outError = "No open database.";
			}
			return 0;
		}

		FileStore store = storeOf(m_handle);
		const FileStoreResult downloaded = store.downloadFile(url);
		if (!downloaded.ok)
		{
			if (outError)
			{
				*outError = downloaded.errorMessage;
			}
			return 0;
		}

		// FileStore::attachFile() only takes a local source path, so the row for downloaded
		// content is written here from the same fields it would have used.
		PartFile file;
		file.partId = part.id;
		file.role = toString(PartFileRole::Datasheet);
		file.relativePath = downloaded.relativePath;
		file.contentHash = downloaded.contentHash;
		file.sizeBytes = downloaded.sizeBytes;
		file.mimeType = downloaded.mimeType;
		file.originalFilename = downloaded.originalFilename;

		const int fileId = PartRepository::insertFile(*db, file);
		if (fileId == 0)
		{
			if (outError)
			{
				*outError = "Could not insert the part_file row for " + downloaded.originalFilename + ".";
			}
			return 0;
		}
		detachDatasheet(part);
		part.datasheetFileId = fileId;
		return fileId;
	}

	bool PartEditorController::detachDatasheet(Part& part) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		if (!db || part.datasheetFileId == 0)
		{
			return false;
		}
		FileStore store = storeOf(m_handle);
		const bool removed = store.detachFile(*db, part.datasheetFileId);
		// The pointer is cleared either way: a row that is already gone must not stay referenced.
		part.datasheetFileId = 0;
		return removed;
	}

	bool PartEditorController::datasheetFile(const Part& part, PartFile& outFile) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && part.datasheetFileId != 0 && PartRepository::findFile(*db, part.datasheetFileId, outFile);
	}

	std::string PartEditorController::datasheetPath(const Part& part) const
	{
		PartFile file;
		if (!datasheetFile(part, file))
		{
			return std::string();
		}
		return storeOf(m_handle).absolutePath(file.relativePath);
	}

	std::vector<Tag> PartEditorController::allTags() const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? TagRepository::listTags(*db) : std::vector<Tag>();
	}

	std::vector<Tag> PartEditorController::partTags(int partId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? TagRepository::listPartTags(*db, partId) : std::vector<Tag>();
	}

	bool PartEditorController::addPartTag(int partId, int tagId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::addPartTag(*db, partId, tagId);
	}

	bool PartEditorController::removePartTag(int partId, int tagId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::removePartTag(*db, partId, tagId);
	}

	int PartEditorController::createTag(const Tag& tag) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? TagRepository::insertTag(*db, tag) : NoTagId;
	}

	bool PartEditorController::updateTag(const Tag& tag) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::updateTag(*db, tag);
	}

	bool PartEditorController::deleteTag(int tagId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::deleteTag(*db, tagId);
	}

	std::vector<TagCategory> PartEditorController::tagCategories() const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? TagRepository::listCategories(*db) : std::vector<TagCategory>();
	}

	int PartEditorController::createTagCategory(const TagCategory& category) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db ? TagRepository::insertCategory(*db, category) : NoTagCategoryId;
	}

	bool PartEditorController::updateTagCategory(const TagCategory& category) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::updateCategory(*db, category);
	}

	bool PartEditorController::deleteTagCategory(int categoryId) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::deleteCategory(*db, categoryId);
	}

	bool PartEditorController::setTagCategory(int tagId, int categoryId, bool recolour) const
	{
		SQLiteWrapper::SQLite* db = connectionOf(m_handle);
		return db && TagRepository::setTagCategory(*db, tagId, categoryId, recolour);
	}

#else

	std::vector<PartType> PartEditorController::types() const { return std::vector<PartType>(); }
	std::vector<PartTypeAttribute> PartEditorController::attributesFor(int) const { return std::vector<PartTypeAttribute>(); }
	std::vector<PartTypeFileSlot> PartEditorController::fileSlotsFor(int) const { return std::vector<PartTypeFileSlot>(); }
	bool PartEditorController::loadPart(int, Part&) const { return false; }
	bool PartEditorController::savePart(const Part&) const { return false; }
	int PartEditorController::createPart(const Part&) const { return 0; }
	int PartEditorController::attachModel3D(int, const std::string&, std::string*) const { return 0; }
	bool PartEditorController::model3DFile(int, PartFile&) const { return false; }
	std::string PartEditorController::model3DPath(int) const { return std::string(); }
	bool PartEditorController::detachModel3D(int) const { return false; }
	int PartEditorController::linkToMouser(int, const std::string&, const std::string&,
		const std::vector<PriceObservation>&) const { return NoPartSellerLinkId; }
	std::vector<PartSellerLink> PartEditorController::sellerLinks(int) const
	{ return std::vector<PartSellerLink>(); }
	std::string PartEditorController::mouserPartNumber(int) const { return std::string(); }
	std::string PartEditorController::mouserUrl(int) const { return std::string(); }
	bool PartEditorController::setMouserPartNumber(int, const std::string&, const std::string&) const
	{ return false; }
	std::vector<Part> PartEditorController::partsWithMpn(const std::string&, int) const
	{ return std::vector<Part>(); }
	int PartEditorController::attachDatasheet(Part&, const std::string&, std::string*) const { return 0; }
	int PartEditorController::downloadDatasheet(Part&, const std::string&, std::string*) const { return 0; }
	bool PartEditorController::detachDatasheet(Part&) const { return false; }
	bool PartEditorController::datasheetFile(const Part&, PartFile&) const { return false; }
	std::string PartEditorController::datasheetPath(const Part&) const { return std::string(); }
	std::vector<Tag> PartEditorController::allTags() const { return std::vector<Tag>(); }
	std::vector<Tag> PartEditorController::partTags(int) const { return std::vector<Tag>(); }
	bool PartEditorController::addPartTag(int, int) const { return false; }
	bool PartEditorController::removePartTag(int, int) const { return false; }
	int PartEditorController::createTag(const Tag&) const { return NoTagId; }
	bool PartEditorController::updateTag(const Tag&) const { return false; }
	bool PartEditorController::deleteTag(int) const { return false; }
	std::vector<TagCategory> PartEditorController::tagCategories() const { return std::vector<TagCategory>(); }
	int PartEditorController::createTagCategory(const TagCategory&) const { return NoTagCategoryId; }
	bool PartEditorController::updateTagCategory(const TagCategory&) const { return false; }
	bool PartEditorController::deleteTagCategory(int) const { return false; }
	bool PartEditorController::setTagCategory(int, int, bool) const { return false; }

#endif

}
