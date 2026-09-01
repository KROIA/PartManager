#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"

#include <cstdlib>
#include <algorithm>
#include <unordered_set>

#if QT_ENABLED
	#include <QJsonArray>
	#include <QJsonDocument>
	#include <QString>
#endif

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
	#include "SQLite.h"
#endif

namespace PartManager
{

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1

	namespace
	{
		// enum_options round-trips a plain JSON array of strings, e.g. ["Linear","Switching"].
		// ponytail: no general JSON dependency in this project (see PartManager_DatabaseMetadata.cpp) —
		// Qt path uses QJsonArray, hand-rolled fallback below covers the flat string-array shape only.
#if QT_ENABLED
		std::string serializeEnumOptions(const std::vector<std::string>& options)
		{
			QJsonArray array;
			for (const std::string& option : options)
			{
				array.append(QString::fromStdString(option));
			}
			return QJsonDocument(array).toJson(QJsonDocument::Compact).toStdString();
		}

		std::vector<std::string> parseEnumOptions(const std::string& json)
		{
			std::vector<std::string> result;
			QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(json));
			if (!doc.isArray())
			{
				return result;
			}
			for (const QJsonValue& value : doc.array())
			{
				result.push_back(value.toString().toStdString());
			}
			return result;
		}
#else
		std::string serializeEnumOptions(const std::vector<std::string>& options)
		{
			std::string out = "[";
			for (size_t i = 0; i < options.size(); ++i)
			{
				if (i > 0) out += ",";
				out += "\"" + options[i] + "\"";
			}
			out += "]";
			return out;
		}

		std::vector<std::string> parseEnumOptions(const std::string& json)
		{
			std::vector<std::string> result;
			size_t pos = 0;
			while ((pos = json.find('"', pos)) != std::string::npos)
			{
				size_t end = json.find('"', pos + 1);
				if (end == std::string::npos) break;
				result.push_back(json.substr(pos + 1, end - pos - 1));
				pos = end + 1;
			}
			return result;
		}
#endif

		// Adds the attr_<key> fast-filter column to `part` if it isn't already there. REAL storage,
		// app-maintained (§2). ponytail: skips the §2 partial index (`CREATE INDEX ... WHERE part_type_id=`)
		// — add it once core/search actually needs the query to be fast, not needed for correctness here.
		bool ensureAttrColumn(SQLiteWrapper::SQLite& db, const std::string& key)
		{
			std::string columnName = "attr_" + key;
			for (const std::vector<std::string>& row : db.fetchAll("PRAGMA table_info(part);"))
			{
				if (row.size() > 1 && row[1] == columnName)
				{
					return true; // already exists
				}
			}
			return db.execute("ALTER TABLE part ADD COLUMN " + columnName + " REAL;");
		}

		bool isNumericDatatype(AttributeDataType type)
		{
			return type == AttributeDataType::Number || type == AttributeDataType::Dimension;
		}

		PartType rowToType(const std::vector<std::string>& row)
		{
			PartType type;
			type.id = std::atoi(row[0].c_str());
			type.name = row[1];
			type.domain = row[2];
			type.kicadRelevant = std::atoi(row[3].c_str()) != 0;
			type.kicadCategory = row[4];
			type.parentTypeId = std::atoi(row[5].c_str());
			type.description = row[6];
			return type;
		}

		PartTypeAttribute rowToAttribute(const std::vector<std::string>& row)
		{
			PartTypeAttribute attribute;
			attribute.id = std::atoi(row[0].c_str());
			attribute.partTypeId = std::atoi(row[1].c_str());
			attribute.key = row[2];
			attribute.label = row[3];
			attribute.unit = row[4];
			attribute.datatype = attributeDataTypeFromString(row[5]);
			attribute.enumOptions = parseEnumOptions(row[6]);
			attribute.searchable = std::atoi(row[7].c_str()) != 0;
			attribute.required = std::atoi(row[8].c_str()) != 0;
			attribute.tooltip = row[9];
			attribute.sortOrder = std::atoi(row[10].c_str());
			return attribute;
		}

		PartTypeFileSlot rowToFileSlot(const std::vector<std::string>& row)
		{
			PartTypeFileSlot slot;
			slot.id = std::atoi(row[0].c_str());
			slot.partTypeId = std::atoi(row[1].c_str());
			slot.role = row[2];
			slot.label = row[3];
			slot.required = std::atoi(row[4].c_str()) != 0;
			slot.tooltip = row[5];
			slot.sortOrder = std::atoi(row[6].c_str());
			return slot;
		}

		// Root ancestor -> typeId, typeId last. Stops early / drops the rest if a cycle is found.
		std::vector<int> ancestorChainRootFirst(SQLiteWrapper::SQLite& db, int typeId)
		{
			std::vector<int> chain;
			std::unordered_set<int> visited;
			int current = typeId;
			while (current != NoParentType && visited.find(current) == visited.end())
			{
				visited.insert(current);
				chain.push_back(current);
				PartType type;
				if (!PartTypeRepository::findType(db, current, type))
				{
					break;
				}
				current = type.parentTypeId;
			}
			std::reverse(chain.begin(), chain.end());
			return chain;
		}
	}

	bool PartTypeRepository::createSchema(SQLiteWrapper::SQLite& db)
	{
		bool ok = true;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_type ("
			"id INTEGER PRIMARY KEY,"
			"name TEXT NOT NULL,"
			"domain TEXT NOT NULL,"
			"kicad_relevant INTEGER NOT NULL DEFAULT 0,"
			"kicad_category TEXT,"
			"parent_type_id INTEGER REFERENCES part_type(id),"
			"description TEXT"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_type_attribute ("
			"id INTEGER PRIMARY KEY,"
			"part_type_id INTEGER NOT NULL REFERENCES part_type(id),"
			"key TEXT NOT NULL,"
			"label TEXT NOT NULL,"
			"unit TEXT,"
			"datatype TEXT NOT NULL,"
			"enum_options TEXT,"
			"searchable INTEGER NOT NULL DEFAULT 0,"
			"required INTEGER NOT NULL DEFAULT 0,"
			"tooltip TEXT,"
			"sort_order INTEGER NOT NULL DEFAULT 0,"
			"UNIQUE(part_type_id, key)"
			");") && ok;
		ok = db.execute(
			"CREATE TABLE IF NOT EXISTS part_type_file_slot ("
			"id INTEGER PRIMARY KEY,"
			"part_type_id INTEGER NOT NULL REFERENCES part_type(id),"
			"role TEXT NOT NULL,"
			"label TEXT NOT NULL,"
			"required INTEGER NOT NULL DEFAULT 0,"
			"tooltip TEXT,"
			"sort_order INTEGER NOT NULL DEFAULT 0,"
			"UNIQUE(part_type_id, role)"
			");") && ok;
		return ok;
	}

	int PartTypeRepository::insertType(SQLiteWrapper::SQLite& db, const PartType& type)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO part_type (name, domain, kicad_relevant, kicad_category, parent_type_id, description) "
			"VALUES (?, ?, ?, ?, ?, ?);",
			{ type.name, type.domain, type.kicadRelevant ? "1" : "0", type.kicadCategory,
			  std::to_string(type.parentTypeId), type.description });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoParentType;
	}

	bool PartTypeRepository::updateType(SQLiteWrapper::SQLite& db, const PartType& type)
	{
		return db.executeWithParams(
			"UPDATE part_type SET name=?, domain=?, kicad_relevant=?, kicad_category=?, parent_type_id=?, description=? "
			"WHERE id=?;",
			{ type.name, type.domain, type.kicadRelevant ? "1" : "0", type.kicadCategory,
			  std::to_string(type.parentTypeId), type.description, std::to_string(type.id) });
	}

	bool PartTypeRepository::deleteType(SQLiteWrapper::SQLite& db, int typeId)
	{
		return db.executeWithParams("DELETE FROM part_type WHERE id=?;", { std::to_string(typeId) });
	}

	bool PartTypeRepository::findType(SQLiteWrapper::SQLite& db, int typeId, PartType& outType)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,name,domain,kicad_relevant,kicad_category,parent_type_id,description "
			"FROM part_type WHERE id=" + std::to_string(typeId) + ";");
		if (rows.empty())
		{
			return false;
		}
		outType = rowToType(rows.front());
		return true;
	}

	std::vector<PartType> PartTypeRepository::listTypes(SQLiteWrapper::SQLite& db)
	{
		std::vector<PartType> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,name,domain,kicad_relevant,kicad_category,parent_type_id,description FROM part_type ORDER BY id;"))
		{
			result.push_back(rowToType(row));
		}
		return result;
	}

	int PartTypeRepository::insertAttribute(SQLiteWrapper::SQLite& db, const PartTypeAttribute& attribute)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO part_type_attribute (part_type_id, key, label, unit, datatype, enum_options, "
			"searchable, required, tooltip, sort_order) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
			{ std::to_string(attribute.partTypeId), attribute.key, attribute.label, attribute.unit,
			  toString(attribute.datatype), serializeEnumOptions(attribute.enumOptions),
			  attribute.searchable ? "1" : "0", attribute.required ? "1" : "0", attribute.tooltip,
			  std::to_string(attribute.sortOrder) });
		if (!ok)
		{
			return 0;
		}
		if (attribute.searchable && isNumericDatatype(attribute.datatype))
		{
			ensureAttrColumn(db, attribute.key);
		}
		return static_cast<int>(db.getLastInsertRowId());
	}

	bool PartTypeRepository::updateAttribute(SQLiteWrapper::SQLite& db, const PartTypeAttribute& attribute)
	{
		bool ok = db.executeWithParams(
			"UPDATE part_type_attribute SET part_type_id=?, key=?, label=?, unit=?, datatype=?, enum_options=?, "
			"searchable=?, required=?, tooltip=?, sort_order=? WHERE id=?;",
			{ std::to_string(attribute.partTypeId), attribute.key, attribute.label, attribute.unit,
			  toString(attribute.datatype), serializeEnumOptions(attribute.enumOptions),
			  attribute.searchable ? "1" : "0", attribute.required ? "1" : "0", attribute.tooltip,
			  std::to_string(attribute.sortOrder), std::to_string(attribute.id) });
		if (ok && attribute.searchable && isNumericDatatype(attribute.datatype))
		{
			ensureAttrColumn(db, attribute.key);
		}
		return ok;
	}

	bool PartTypeRepository::deleteAttribute(SQLiteWrapper::SQLite& db, int attributeId)
	{
		return db.executeWithParams("DELETE FROM part_type_attribute WHERE id=?;", { std::to_string(attributeId) });
	}

	std::vector<PartTypeAttribute> PartTypeRepository::listOwnAttributes(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeAttribute> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_type_id,key,label,unit,datatype,enum_options,searchable,required,tooltip,sort_order "
			"FROM part_type_attribute WHERE part_type_id=" + std::to_string(typeId) + " ORDER BY sort_order,id;"))
		{
			result.push_back(rowToAttribute(row));
		}
		return result;
	}

	int PartTypeRepository::insertFileSlot(SQLiteWrapper::SQLite& db, const PartTypeFileSlot& slot)
	{
		bool ok = db.executeWithParams(
			"INSERT INTO part_type_file_slot (part_type_id, role, label, required, tooltip, sort_order) "
			"VALUES (?, ?, ?, ?, ?, ?);",
			{ std::to_string(slot.partTypeId), slot.role, slot.label, slot.required ? "1" : "0", slot.tooltip,
			  std::to_string(slot.sortOrder) });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : 0;
	}

	bool PartTypeRepository::updateFileSlot(SQLiteWrapper::SQLite& db, const PartTypeFileSlot& slot)
	{
		return db.executeWithParams(
			"UPDATE part_type_file_slot SET part_type_id=?, role=?, label=?, required=?, tooltip=?, sort_order=? "
			"WHERE id=?;",
			{ std::to_string(slot.partTypeId), slot.role, slot.label, slot.required ? "1" : "0", slot.tooltip,
			  std::to_string(slot.sortOrder), std::to_string(slot.id) });
	}

	bool PartTypeRepository::deleteFileSlot(SQLiteWrapper::SQLite& db, int fileSlotId)
	{
		return db.executeWithParams("DELETE FROM part_type_file_slot WHERE id=?;", { std::to_string(fileSlotId) });
	}

	std::vector<PartTypeFileSlot> PartTypeRepository::listOwnFileSlots(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeFileSlot> result;
		for (const std::vector<std::string>& row : db.fetchAll(
			"SELECT id,part_type_id,role,label,required,tooltip,sort_order "
			"FROM part_type_file_slot WHERE part_type_id=" + std::to_string(typeId) + " ORDER BY sort_order,id;"))
		{
			result.push_back(rowToFileSlot(row));
		}
		return result;
	}

	std::vector<PartTypeAttribute> PartTypeRepository::effectiveAttributes(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeAttribute> accumulated;
		for (int level : ancestorChainRootFirst(db, typeId))
		{
			std::vector<PartTypeAttribute> ownRows = listOwnAttributes(db, level);
			std::unordered_set<std::string> overriddenKeys;
			for (const PartTypeAttribute& row : ownRows)
			{
				overriddenKeys.insert(row.key);
			}
			std::vector<PartTypeAttribute> merged;
			for (const PartTypeAttribute& existing : accumulated)
			{
				if (overriddenKeys.find(existing.key) == overriddenKeys.end())
				{
					merged.push_back(existing);
				}
			}
			for (const PartTypeAttribute& row : ownRows)
			{
				merged.push_back(row);
			}
			accumulated = std::move(merged);
		}
		return accumulated;
	}

	std::vector<PartTypeFileSlot> PartTypeRepository::effectiveFileSlots(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<PartTypeFileSlot> accumulated;
		for (int level : ancestorChainRootFirst(db, typeId))
		{
			std::vector<PartTypeFileSlot> ownRows = listOwnFileSlots(db, level);
			std::unordered_set<std::string> overriddenRoles;
			for (const PartTypeFileSlot& row : ownRows)
			{
				overriddenRoles.insert(row.role);
			}
			std::vector<PartTypeFileSlot> merged;
			for (const PartTypeFileSlot& existing : accumulated)
			{
				if (overriddenRoles.find(existing.role) == overriddenRoles.end())
				{
					merged.push_back(existing);
				}
			}
			for (const PartTypeFileSlot& row : ownRows)
			{
				merged.push_back(row);
			}
			accumulated = std::move(merged);
		}
		return accumulated;
	}

	std::string PartTypeRepository::effectiveKicadCategory(SQLiteWrapper::SQLite& db, int typeId)
	{
		int current = typeId;
		std::unordered_set<int> visited;
		while (current != NoParentType && visited.find(current) == visited.end())
		{
			visited.insert(current);
			PartType type;
			if (!findType(db, current, type))
			{
				break;
			}
			if (!type.kicadCategory.empty())
			{
				return type.kicadCategory;
			}
			current = type.parentTypeId;
		}
		return std::string();
	}

	std::string PartTypeRepository::effectiveDomain(SQLiteWrapper::SQLite& db, int typeId)
	{
		int current = typeId;
		std::unordered_set<int> visited;
		while (current != NoParentType && visited.find(current) == visited.end())
		{
			visited.insert(current);
			PartType type;
			if (!findType(db, current, type))
			{
				break;
			}
			if (!type.domain.empty())
			{
				return type.domain;
			}
			current = type.parentTypeId;
		}
		return std::string();
	}

	bool PartTypeRepository::seedDefaultTypes(SQLiteWrapper::SQLite& db)
	{
		if (!db.fetchAll("SELECT id FROM part_type LIMIT 1;").empty())
		{
			return true; // already seeded / has data, don't touch it
		}

		auto addAttr = [&db](int typeId, const std::string& key, const std::string& label,
			const std::string& unit, AttributeDataType datatype, bool searchable,
			const std::vector<std::string>& enumOptions = {})
		{
			PartTypeAttribute attribute;
			attribute.partTypeId = typeId;
			attribute.key = key;
			attribute.label = label;
			attribute.unit = unit;
			attribute.datatype = datatype;
			attribute.enumOptions = enumOptions;
			attribute.searchable = searchable;
			insertAttribute(db, attribute);
		};

		PartType resistor;
		resistor.name = "Resistor";
		resistor.domain = "electronic";
		resistor.kicadRelevant = true;
		resistor.kicadCategory = "Resistors";
		int resistorId = insertType(db, resistor);
		addAttr(resistorId, "resistance", "Resistance", "\xCE\xA9", AttributeDataType::Dimension, true);
		addAttr(resistorId, "tolerance", "Tolerance", "%", AttributeDataType::Dimension, false);
		addAttr(resistorId, "power", "Power", "W", AttributeDataType::Dimension, false);

		PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		capacitor.kicadRelevant = true;
		capacitor.kicadCategory = "Capacitors";
		int capacitorId = insertType(db, capacitor);
		addAttr(capacitorId, "capacitance", "Capacitance", "F", AttributeDataType::Dimension, true);
		addAttr(capacitorId, "voltage", "Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(capacitorId, "tolerance", "Tolerance", "%", AttributeDataType::Dimension, false);

		// §2b inheritance test case: Ceramic Capacitor extends Capacitor, per the architecture doc's
		// own example — adds one attribute of its own, inherits Capacitance/Voltage/Tolerance as-is.
		PartType ceramicCapacitor;
		ceramicCapacitor.name = "Ceramic Capacitor";
		ceramicCapacitor.domain = "electronic";
		ceramicCapacitor.kicadRelevant = true;
		ceramicCapacitor.parentTypeId = capacitorId;
		int ceramicCapacitorId = insertType(db, ceramicCapacitor);
		addAttr(ceramicCapacitorId, "dielectric", "Dielectric", "", AttributeDataType::Text, false);

		PartType inductor;
		inductor.name = "Inductor";
		inductor.domain = "electronic";
		inductor.kicadRelevant = true;
		inductor.kicadCategory = "Inductors";
		int inductorId = insertType(db, inductor);
		addAttr(inductorId, "inductance", "Inductance", "H", AttributeDataType::Dimension, true);
		addAttr(inductorId, "current_rating", "Current rating", "A", AttributeDataType::Dimension, false);

		PartType powerRegulator;
		powerRegulator.name = "Power Regulator";
		powerRegulator.domain = "electronic";
		powerRegulator.kicadRelevant = true;
		powerRegulator.kicadCategory = "ICs";
		int powerRegulatorId = insertType(db, powerRegulator);
		addAttr(powerRegulatorId, "output_voltage", "Output Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(powerRegulatorId, "max_current", "Max Current", "A", AttributeDataType::Dimension, false);
		addAttr(powerRegulatorId, "regulator_type", "Regulator Type", "", AttributeDataType::Enum, false,
			{ "Linear", "Switching" });

		PartType transistor;
		transistor.name = "Transistor";
		transistor.domain = "electronic";
		transistor.kicadRelevant = true;
		transistor.kicadCategory = "Transistors";
		int transistorId = insertType(db, transistor);

		PartType mosfet;
		mosfet.name = "MOSFET";
		mosfet.domain = "electronic";
		mosfet.kicadRelevant = true;
		mosfet.kicadCategory = "Transistors";
		mosfet.parentTypeId = transistorId;
		int mosfetId = insertType(db, mosfet);
		addAttr(mosfetId, "vds_max", "Vds Max", "V", AttributeDataType::Dimension, false);
		addAttr(mosfetId, "id_max", "Id Max", "A", AttributeDataType::Dimension, false);
		addAttr(mosfetId, "channel_type", "Channel Type", "", AttributeDataType::Enum, false,
			{ "N-Channel", "P-Channel" });

		return true;
	}

#endif

}
