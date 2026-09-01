#include "controllers/PartManager_PartEditorController.h"

#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <algorithm>

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

#else

	std::vector<PartType> PartEditorController::types() const { return std::vector<PartType>(); }
	std::vector<PartTypeAttribute> PartEditorController::attributesFor(int) const { return std::vector<PartTypeAttribute>(); }
	std::vector<PartTypeFileSlot> PartEditorController::fileSlotsFor(int) const { return std::vector<PartTypeFileSlot>(); }
	bool PartEditorController::loadPart(int, Part&) const { return false; }
	bool PartEditorController::savePart(const Part&) const { return false; }
	int PartEditorController::createPart(const Part&) const { return 0; }
	std::vector<Tag> PartEditorController::allTags() const { return std::vector<Tag>(); }
	std::vector<Tag> PartEditorController::partTags(int) const { return std::vector<Tag>(); }
	bool PartEditorController::addPartTag(int, int) const { return false; }
	bool PartEditorController::removePartTag(int, int) const { return false; }
	int PartEditorController::createTag(const Tag&) const { return NoTagId; }
	bool PartEditorController::updateTag(const Tag&) const { return false; }
	bool PartEditorController::deleteTag(int) const { return false; }

#endif

}
