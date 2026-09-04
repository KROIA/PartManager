#include "persistence/PartManager_PartRepository.h"
#include "persistence/PartManager_PartTypeRepository.h"
#include "persistence/PartManager_TagRepository.h"
#include "PartManager_global.h"

#include <algorithm>
#include <cstdio>
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
			part.searchKeywords = row.size() > 15 ? row[15] : std::string();
			part.excludedKeywords = row.size() > 16 ? row[16] : std::string();
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

		// std::to_string() on a double is fixed 6-decimal notation, which silently rounds every
		// sub-micro value to "0.000000" — a 100 nF capacitance would reach attr_capacitance as 0
		// and never match a search again. 17 significant digits round-trips any IEEE double.
		std::string toSqlNumber(double value)
		{
			char buffer[64] = { 0 };
			std::snprintf(buffer, sizeof(buffer), "%.17g", value);
			return buffer;
		}

		// (Re)writes every searchable numeric attr_<key> column for partId, from its type's effective
		// attribute list and the part's own attributes JSON. Every other attr_* column on the row is
		// set back to NULL.
		//
		// **The NULLing is not tidiness, it is correctness.** These columns are the §7a fast filter,
		// and nothing else re-reads the JSON to check them. Leaving a stale one behind means a part
		// whose value was cleared — or, since a part can be moved between categories, one whose type
		// does not declare that attribute at all any more — still answers `resistance>1k` with the
		// number it used to have.
		void writeSearchableAttrColumns(SQLiteWrapper::SQLite& db, int partId, int partTypeId, const std::string& attributesJson)
		{
			std::vector<std::string> written;
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
					{ toSqlNumber(value), std::to_string(partId) });
				written.push_back("attr_" + attribute.key);
			}

			for (const std::vector<std::string>& row : db.fetchAll("PRAGMA table_info(part);"))
			{
				if (row.size() < 2 || row[1].rfind("attr_", 0) != 0
					|| std::find(written.begin(), written.end(), row[1]) != written.end())
				{
					continue;
				}
				// Safe to concatenate: the name came out of the schema, and only ensureAttrColumn()
				// ever puts one there, from a key already restricted to identifier characters.
				db.executeWithParams("UPDATE part SET " + row[1] + "=NULL WHERE id=?;",
					{ std::to_string(partId) });
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
			"updated_at TEXT NOT NULL DEFAULT (datetime('now')),"
			"search_keywords TEXT,"
			"excluded_keywords TEXT"
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
			"datasheet_file_id, stock_qty, stock_min_qty, storage_location, is_active, search_keywords, "
			"excluded_keywords) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
			{ std::to_string(part.partTypeId), part.name, part.manufacturer, part.mpn, part.description,
			  part.package, part.attributes, std::to_string(part.datasheetFileId), std::to_string(part.stockQty),
			  std::to_string(part.stockMinQty), part.storageLocation, part.isActive ? "1" : "0",
			  part.searchKeywords, part.excludedKeywords });
		if (!ok)
		{
			return 0;
		}
		int newId = static_cast<int>(db.getLastInsertRowId());
		writeSearchableAttrColumns(db, newId, part.partTypeId, part.attributes);
		// §2d one-time seed of the type's default tags. Done here rather than at the call site so no
		// caller can forget it; updatePart() deliberately does not, tags are independent after creation.
		TagRepository::seedTagsForNewPart(db, newId, part.partTypeId);
		return newId;
	}

	bool PartRepository::updatePart(SQLiteWrapper::SQLite& db, const Part& part)
	{
		bool ok = db.executeWithParams(
			"UPDATE part SET part_type_id=?, name=?, manufacturer=?, mpn=?, description=?, package=?, attributes=?, "
			"datasheet_file_id=?, stock_qty=?, stock_min_qty=?, storage_location=?, is_active=?, search_keywords=?, "
			"excluded_keywords=?, updated_at=datetime('now') WHERE id=?;",
			{ std::to_string(part.partTypeId), part.name, part.manufacturer, part.mpn, part.description,
			  part.package, part.attributes, std::to_string(part.datasheetFileId), std::to_string(part.stockQty),
			  std::to_string(part.stockMinQty), part.storageLocation, part.isActive ? "1" : "0",
			  part.searchKeywords, part.excludedKeywords, std::to_string(part.id) });
		if (ok)
		{
			writeSearchableAttrColumns(db, part.id, part.partTypeId, part.attributes);
		}
		return ok;
	}

	bool PartRepository::deletePart(SQLiteWrapper::SQLite& db, int partId)
	{
		// Nothing in this codebase sets PRAGMA foreign_keys=ON, so the REFERENCES part(id)
		// clauses on part_file, part_tag and stock_transaction enforce nothing — every child row
		// has to go by hand. Done here rather than at the call site so no caller can forget, and
		// so the orphans cannot come back through a second delete path later.
		// The tables belong to other repositories and an older database may predate them; a
		// DELETE against a table that is not there simply fails and is ignored.
		// ponytail: the part_file rows go, the files they name stay in filestore/. Deleting those
		// needs FileStore, which persistence must not depend on (§12a); upgrade path is a sweep
		// that drops any stored file no part_file row points at. Not transactional either — the
		// whole codebase runs without explicit transactions, so a crash mid-delete leaves the
		// part row behind with fewer children, which the next delete finishes off.
		const std::string id = std::to_string(partId);
		db.executeWithParams("DELETE FROM part_file WHERE part_id=?;", { id });
		db.executeWithParams("DELETE FROM part_tag WHERE part_id=?;", { id });
		db.executeWithParams("DELETE FROM stock_transaction WHERE part_id=?;", { id });
		return db.executeWithParams("DELETE FROM part WHERE id=?;", { id });
	}

	bool PartRepository::findPart(SQLiteWrapper::SQLite& db, int partId, Part& outPart)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,part_type_id,name,manufacturer,mpn,description,package,attributes,datasheet_file_id,"
			"stock_qty,stock_min_qty,storage_location,is_active,created_at,updated_at,search_keywords,excluded_keywords "
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
			"stock_qty,stock_min_qty,storage_location,is_active,created_at,updated_at,search_keywords,excluded_keywords FROM part";
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

	bool PartRepository::findFile(SQLiteWrapper::SQLite& db, int fileId, PartFile& outFile)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,part_id,role,relative_path,content_hash,size_bytes,mime_type,original_filename,added_at "
			"FROM part_file WHERE id=" + std::to_string(fileId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outFile = rowToFile(rows.front());
		return true;
	}

	int PartRepository::countFilesWithPath(SQLiteWrapper::SQLite& db, const std::string& relativePath)
	{
		// fetchAll() takes no bind parameters (executeWithParams() is write-only), so the one
		// string that reaches this query gets its quotes doubled by hand.
		std::string quoted;
		for (char c : relativePath)
		{
			quoted += c;
			if (c == '\'')
			{
				quoted += c;
			}
		}
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT COUNT(*) FROM part_file WHERE relative_path='" + quoted + "';");
		if (rows.empty() || rows.front().empty())
		{
			return 0;
		}
		return std::atoi(rows.front().front().c_str());
	}

	std::vector<std::string> PartRepository::allFilePaths(SQLiteWrapper::SQLite& db)
	{
		std::vector<std::string> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT DISTINCT relative_path FROM part_file;"))
		{
			if (!row.empty() && !row.front().empty())
			{
				result.push_back(row.front());
			}
		}
		return result;
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

	std::vector<PartFile> PartRepository::listFilesWithRole(SQLiteWrapper::SQLite& db, PartFileRole role)
	{
		std::vector<PartFile> result;
		// The role vocabulary is a fixed enum, so toString() can never produce a quote and the
		// literal needs no escaping — but fetchAll() takes no bind parameters either way.
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_id,role,relative_path,content_hash,size_bytes,mime_type,original_filename,added_at "
			"FROM part_file WHERE role='" + toString(role) + "' ORDER BY id;"))
		{
			result.push_back(rowToFile(row));
		}
		return result;
	}

#endif

}
