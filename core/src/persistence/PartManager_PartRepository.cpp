#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <cstdlib>

#if QT_ENABLED
	#include <QJsonDocument>
	#include <QJsonObject>
#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// Pulls attributes[key].value out of the raw `attributes` JSON, e.g. {"resistance":{"value":4700,"unit":"Ω"}}.
		// ponytail: this is the pass-through the value is trusted to already be base-SI (§2a) — the real
		// SI-prefix entry parser lands in core/units, this just reads the number back out for attr_* columns.
#if QT_ENABLED
		bool extractAttributeValue(const std::string& attributesJson, const std::string& key, double& outValue)
		{
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(attributesJson));
			if (!doc.isObject())
			{
				return false;
			}
			QJsonValue entry = doc.object().value(QString::fromStdString(key));
			if (!entry.isObject() || !entry.toObject().contains("value"))
			{
				return false;
			}
			outValue = entry.toObject().value("value").toDouble();
			return true;
		}
#else
		bool extractAttributeValue(const std::string& attributesJson, const std::string& key, double& outValue)
		{
			std::string needle = "\"" + key + "\"";
			size_t pos = attributesJson.find(needle);
			if (pos == std::string::npos)
			{
				return false;
			}
			size_t objStart = attributesJson.find('{', pos);
			size_t objEnd = attributesJson.find('}', objStart == std::string::npos ? pos : objStart);
			if (objStart == std::string::npos || objEnd == std::string::npos)
			{
				return false;
			}
			std::string nested = attributesJson.substr(objStart, objEnd - objStart);
			size_t valuePos = nested.find("\"value\"");
			if (valuePos == std::string::npos)
			{
				return false;
			}
			valuePos = nested.find(':', valuePos);
			if (valuePos == std::string::npos)
			{
				return false;
			}
			outValue = std::atof(nested.c_str() + valuePos + 1);
			return true;
		}
#endif

		Part rowToPart(const std::vector<std::string>& row)
		{
			Part part;
			part.id = std::atoi(row[0].c_str());
			part.partTypeId = std::atoi(row[1].c_str());
			part.name = row[2];
			part.manufacturer = row[3];
			part.mpn = row[4];
			part.description = row[5];
			part.package = row[6];
			part.attributes = row[7].empty() ? "{}" : row[7];
			part.datasheetFileId = std::atoi(row[8].c_str());
			part.stockQty = std::atoi(row[9].c_str());
			part.stockMinQty = std::atoi(row[10].c_str());
			part.storageLocation = row[11];
			part.isActive = std::atoi(row[12].c_str()) != 0;
			part.createdAt = row[13];
			part.updatedAt = row[14];
			return part;
		}

		PartFile rowToFile(const std::vector<std::string>& row)
		{
			PartFile file;
			file.id = std::atoi(row[0].c_str());
			file.partId = std::atoi(row[1].c_str());
			file.role = row[2];
			file.relativePath = row[3];
			file.contentHash = row[4];
			file.sizeBytes = std::atoi(row[5].c_str());
			file.mimeType = row[6];
			file.originalFilename = row[7];
			file.addedAt = row[8];
			return file;
		}

		// (Re)writes every searchable numeric attr_<key> column for partId, from its type's effective
		// attribute list and the part's own attributes JSON. Missing/non-numeric values are left untouched.
		void writeSearchableAttrColumns(SQLiteWrapper::SQLite& db, int partId, int partTypeId, const std::string& attributesJson)
		{
			for (const PartTypeAttribute& attribute : PartTypeRepository::effectiveAttributes(db, partTypeId))
			{
				if (!attribute.searchable ||
					(attribute.datatype != AttributeDataType::Dimension && attribute.datatype != AttributeDataType::Number))
				{
					continue;
				}
				double value = 0.0;
				if (!extractAttributeValue(attributesJson, attribute.key, value))
				{
					continue;
				}
				db.executeWithParams(
					"UPDATE part SET attr_" + attribute.key + "=? WHERE id=?;",
					{ std::to_string(value), std::to_string(partId) });
			}
		}
	}

	bool PartRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = true;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part ("
			"id INTEGER PRIMARY KEY,"
			"part_type_id INTEGER NOT NULL REFERENCES part_type(id),"
			"name TEXT NOT NULL,"
			"manufacturer TEXT,"
			"mpn TEXT,"
			"description TEXT,"
			"package TEXT,"
			"attributes TEXT NOT NULL DEFAULT '{}',"
			"datasheet_file_id INTEGER REFERENCES part_file(id),"
			"stock_qty INTEGER NOT NULL DEFAULT 0,"
			"stock_min_qty INTEGER NOT NULL DEFAULT 0,"
			"storage_location TEXT,"
			"is_active INTEGER NOT NULL DEFAULT 1,"
			"created_at TEXT NOT NULL DEFAULT (datetime('now')),"
			"updated_at TEXT NOT NULL DEFAULT (datetime('now'))"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_file ("
			"id INTEGER PRIMARY KEY,"
			"part_id INTEGER NOT NULL REFERENCES part(id),"
			"role TEXT NOT NULL,"
			"relative_path TEXT NOT NULL,"
			"content_hash TEXT NOT NULL,"
			"size_bytes INTEGER NOT NULL,"
			"mime_type TEXT,"
			"original_filename TEXT,"
			"added_at TEXT NOT NULL DEFAULT (datetime('now'))"
			");") && ok;
		return ok;
	}

	int PartRepository::insertPart(SQLiteWrapper::SQLite& db, const Part& part)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO part (part_type_id, name, manufacturer, mpn, description, package, attributes, "
			"datasheet_file_id, stock_qty, stock_min_qty, storage_location, is_active) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
			{ std::to_string(part.partTypeId), part.name, part.manufacturer, part.mpn, part.description,
			  part.package, part.attributes, std::to_string(part.datasheetFileId), std::to_string(part.stockQty),
			  std::to_string(part.stockMinQty), part.storageLocation, part.isActive ? "1" : "0" });
		if (!ok)
		{
			return 0;
		}
		int newId = static_cast<int>(db.getLastInsertRowId());
		writeSearchableAttrColumns(db, newId, part.partTypeId, part.attributes);
		return newId;
	}

	bool PartRepository::updatePart(SQLiteWrapper::SQLite& db, const Part& part)
	{
		bool ok = db.executeWithParams(
			"UPDATE part SET part_type_id=?, name=?, manufacturer=?, mpn=?, description=?, package=?, attributes=?, "
			"datasheet_file_id=?, stock_qty=?, stock_min_qty=?, storage_location=?, is_active=?, "
			"updated_at=datetime('now') WHERE id=?;",
			{ std::to_string(part.partTypeId), part.name, part.manufacturer, part.mpn, part.description,
			  part.package, part.attributes, std::to_string(part.datasheetFileId), std::to_string(part.stockQty),
			  std::to_string(part.stockMinQty), part.storageLocation, part.isActive ? "1" : "0",
			  std::to_string(part.id) });
		if (ok)
		{
			writeSearchableAttrColumns(db, part.id, part.partTypeId, part.attributes);
		}
		return ok;
	}

	bool PartRepository::deletePart(SQLiteWrapper::SQLite& db, int partId)
	{
		return db.executeWithParams("DELETE FROM part WHERE id=?;", { std::to_string(partId) });
	}

	bool PartRepository::findPart(SQLiteWrapper::SQLite& db, int partId, Part& outPart)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,part_type_id,name,manufacturer,mpn,description,package,attributes,datasheet_file_id,"
			"stock_qty,stock_min_qty,storage_location,is_active,created_at,updated_at "
			"FROM part WHERE id=" + std::to_string(partId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outPart = rowToPart(rows.front());
		return true;
	}

	std::vector<Part> PartRepository::listParts(SQLiteWrapper::SQLite& db, int partTypeId)
	{
		std::string query =
			"SELECT id,part_type_id,name,manufacturer,mpn,description,package,attributes,datasheet_file_id,"
			"stock_qty,stock_min_qty,storage_location,is_active,created_at,updated_at FROM part";
		if (partTypeId != 0)
		{
			query += " WHERE part_type_id=" + std::to_string(partTypeId);
		}
		query += " ORDER BY id;";

		std::vector<Part> result;
		for (const std::vector<std::string>& row : db.fetchAll(query))
		{
			result.push_back(rowToPart(row));
		}
		return result;
	}

	int PartRepository::insertFile(SQLiteWrapper::SQLite& db, const PartFile& file)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO part_file (part_id, role, relative_path, content_hash, size_bytes, mime_type, original_filename) "
			"VALUES (?, ?, ?, ?, ?, ?, ?);",
			{ std::to_string(file.partId), file.role, file.relativePath, file.contentHash,
			  std::to_string(file.sizeBytes), file.mimeType, file.originalFilename });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : 0;
	}

	bool PartRepository::deleteFile(SQLiteWrapper::SQLite& db, int fileId)
	{
		return db.executeWithParams("DELETE FROM part_file WHERE id=?;", { std::to_string(fileId) });
	}

	std::vector<PartFile> PartRepository::listFiles(SQLiteWrapper::SQLite& db, int partId)
	{
		std::vector<PartFile> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_id,role,relative_path,content_hash,size_bytes,mime_type,original_filename,added_at "
			"FROM part_file WHERE part_id=" + std::to_string(partId) + " ORDER BY id;"))
		{
			result.push_back(rowToFile(row));
		}
		return result;
	}

#endif

}
