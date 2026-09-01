#include "database/PartManager_DatabaseMetadata.h"
#include "PartManager_debug.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#if QT_ENABLED
	#include <QFile>
	#include <QIODevice>
	#include <QJsonDocument>
	#include <QJsonObject>
	#include <QString>
#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if QT_ENABLED
	// Qt path: QJsonDocument round-trips the flat 4-key object.
	bool DatabaseMetadata::readPmdbFile(const std::string& pmdbPath, DatabaseMetadataValues& outValues)
	{
		QFile file(QString::fromStdString(pmdbPath));
		if (!file.open(QIODevice::ReadOnly))
		{
			return false;
		}
		QJsonParseError error;
		QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
		file.close();
		if (error.error != QJsonParseError::NoError || !doc.isObject())
		{
			return false;
		}
		QJsonObject obj = doc.object();
		outValues.schemaVersion = obj.value("schema_version").toInt(0);
		outValues.toolVersionLastSaved = obj.value("tool_version_last_saved").toString().toStdString();
		outValues.createdAt = obj.value("created_at").toString().toStdString();
		outValues.lastOpenedAt = obj.value("last_opened_at").toString().toStdString();
		return true;
	}

	bool DatabaseMetadata::writePmdbFile(const std::string& pmdbPath, const DatabaseMetadataValues& values)
	{
		QJsonObject obj;
		obj["schema_version"] = values.schemaVersion;
		obj["tool_version_last_saved"] = QString::fromStdString(values.toolVersionLastSaved);
		obj["created_at"] = QString::fromStdString(values.createdAt);
		obj["last_opened_at"] = QString::fromStdString(values.lastOpenedAt);

		QFile file(QString::fromStdString(pmdbPath));
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		{
			return false;
		}
		file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
		file.close();
		return true;
	}
#else
	// ponytail: no Qt available here — hand-rolled writer/parser for this one fixed flat 4-key
	// object rather than a JSON dependency. Upgrade to a real parser if the .pmdb shape ever
	// grows past 4 flat string/int fields.
	namespace
	{
		std::string jsonEscape(const std::string& s)
		{
			std::string out;
			for (char c : s)
			{
				if (c == '"' || c == '\\')
				{
					out += '\\';
				}
				out += c;
			}
			return out;
		}

		// Extracts the value for "key": "..." or "key": <number> from a flat JSON object.
		bool extractField(const std::string& json, const std::string& key, std::string& outValue)
		{
			std::string needle = "\"" + key + "\"";
			size_t pos = json.find(needle);
			if (pos == std::string::npos)
			{
				return false;
			}
			pos = json.find(':', pos);
			if (pos == std::string::npos)
			{
				return false;
			}
			pos++;
			while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
			{
				pos++;
			}
			if (pos >= json.size())
			{
				return false;
			}
			if (json[pos] == '"')
			{
				pos++;
				std::string value;
				while (pos < json.size() && json[pos] != '"')
				{
					if (json[pos] == '\\' && pos + 1 < json.size())
					{
						pos++;
					}
					value += json[pos];
					pos++;
				}
				outValue = value;
			}
			else
			{
				size_t end = json.find_first_of(",}\n", pos);
				outValue = json.substr(pos, end - pos);
				while (!outValue.empty() && (outValue.back() == ' ' || outValue.back() == '\t' || outValue.back() == '\r'))
				{
					outValue.pop_back();
				}
			}
			return true;
		}
	}

	bool DatabaseMetadata::readPmdbFile(const std::string& pmdbPath, DatabaseMetadataValues& outValues)
	{
		std::ifstream file(pmdbPath, std::ios::binary);
		if (!file.is_open())
		{
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();
		std::string json = buffer.str();

		std::string field;
		if (extractField(json, "schema_version", field))
		{
			outValues.schemaVersion = std::atoi(field.c_str());
		}
		extractField(json, "tool_version_last_saved", outValues.toolVersionLastSaved);
		extractField(json, "created_at", outValues.createdAt);
		extractField(json, "last_opened_at", outValues.lastOpenedAt);
		return true;
	}

	bool DatabaseMetadata::writePmdbFile(const std::string& pmdbPath, const DatabaseMetadataValues& values)
	{
		std::ofstream file(pmdbPath, std::ios::binary | std::ios::trunc);
		if (!file.is_open())
		{
			return false;
		}
		file << "{\n"
			<< "  \"schema_version\": " << values.schemaVersion << ",\n"
			<< "  \"tool_version_last_saved\": \"" << jsonEscape(values.toolVersionLastSaved) << "\",\n"
			<< "  \"created_at\": \"" << jsonEscape(values.createdAt) << "\",\n"
			<< "  \"last_opened_at\": \"" << jsonEscape(values.lastOpenedAt) << "\"\n"
			<< "}\n";
		return true;
	}
#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	bool DatabaseMetadata::readDbMeta(SQLiteWrapper::SQLite& db, DatabaseMetadataValues& outValues)
	{
		if (!db.tableExists("db_meta"))
		{
			return false;
		}
		std::vector<std::vector<std::string>> rows = db.fetchAll("SELECT key, value FROM db_meta;");
		for (const std::vector<std::string>& row : rows)
		{
			if (row.size() < 2)
			{
				continue;
			}
			const std::string& key = row[0];
			const std::string& value = row[1];
			if (key == "schema_version")
			{
				outValues.schemaVersion = std::atoi(value.c_str());
			}
			else if (key == "tool_version_last_saved")
			{
				outValues.toolVersionLastSaved = value;
			}
			else if (key == "created_at")
			{
				outValues.createdAt = value;
			}
			else if (key == "last_opened_at")
			{
				outValues.lastOpenedAt = value;
			}
		}
		return true;
	}

	bool DatabaseMetadata::writeDbMeta(SQLiteWrapper::SQLite& db, const DatabaseMetadataValues& values)
	{
		if (!db.tableExists("db_meta"))
		{
			if (!db.execute("CREATE TABLE db_meta (key TEXT PRIMARY KEY, value TEXT);"))
			{
				return false;
			}
		}
		static const std::string upsert =
			"INSERT INTO db_meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
		bool ok = true;
		ok = db.executeWithParams(upsert, { "schema_version", std::to_string(values.schemaVersion) }) && ok;
		ok = db.executeWithParams(upsert, { "tool_version_last_saved", values.toolVersionLastSaved }) && ok;
		ok = db.executeWithParams(upsert, { "created_at", values.createdAt }) && ok;
		ok = db.executeWithParams(upsert, { "last_opened_at", values.lastOpenedAt }) && ok;
		return ok;
	}
#endif

}
