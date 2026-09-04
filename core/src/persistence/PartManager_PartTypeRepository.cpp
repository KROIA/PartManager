#include "persistence/PartManager_PartTypeRepository.h"
#include "PartManager_global.h"
#include "domain/PartManager_PartFileRole.h"

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
			type.searchKeywords = row.size() > 7 ? row[7] : std::string();
			type.excludedKeywords = row.size() > 8 ? row[8] : std::string();
			type.nameTemplate = row.size() > 9 ? row[9] : std::string();
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
	}

	// Root ancestor -> typeId, typeId last. Stops early / drops the rest if a cycle is found.
	std::vector<int> PartTypeRepository::ancestorChainRootFirst(SQLiteWrapper::SQLite& db, int typeId)
	{
		std::vector<int> chain;
		std::unordered_set<int> visited;
		int current = typeId;
		while (current != NoParentType && visited.find(current) == visited.end())
		{
			visited.insert(current);
			chain.push_back(current);
			PartType type;
			if (!findType(db, current, type))
			{
				break;
			}
			current = type.parentTypeId;
		}
		std::reverse(chain.begin(), chain.end());
		return chain;
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
			"description TEXT,"
			"search_keywords TEXT,"
			"excluded_keywords TEXT,"
			"name_template TEXT"
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
			"INSERT INTO part_type (name, domain, kicad_relevant, kicad_category, parent_type_id, description, search_keywords, excluded_keywords, name_template) "
			"VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
			{ type.name, type.domain, type.kicadRelevant ? "1" : "0", type.kicadCategory,
			  std::to_string(type.parentTypeId), type.description, type.searchKeywords, type.excludedKeywords,
			  type.nameTemplate });
		return ok ? static_cast<int>(db.getLastInsertRowId()) : NoParentType;
	}

	bool PartTypeRepository::updateType(SQLiteWrapper::SQLite& db, const PartType& type)
	{
		return db.executeWithParams(
			"UPDATE part_type SET name=?, domain=?, kicad_relevant=?, kicad_category=?, parent_type_id=?, description=?, search_keywords=?, excluded_keywords=?, name_template=? "
			"WHERE id=?;",
			{ type.name, type.domain, type.kicadRelevant ? "1" : "0", type.kicadCategory,
			  std::to_string(type.parentTypeId), type.description, type.searchKeywords, type.excludedKeywords,
			  type.nameTemplate, std::to_string(type.id) });
	}

	bool PartTypeRepository::deleteType(SQLiteWrapper::SQLite& db, int typeId)
	{
		return db.executeWithParams("DELETE FROM part_type WHERE id=?;", { std::to_string(typeId) });
	}

	bool PartTypeRepository::findType(SQLiteWrapper::SQLite& db, int typeId, PartType& outType)
	{
		std::vector<std::vector<std::string>> rows = db.fetchAll(
			"SELECT id,name,domain,kicad_relevant,kicad_category,parent_type_id,description,search_keywords,excluded_keywords,name_template "
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
			"SELECT id,name,domain,kicad_relevant,kicad_category,parent_type_id,description,search_keywords,excluded_keywords,name_template FROM part_type ORDER BY id;"))
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
		// Per-name/per-key, not all-or-nothing: that makes this safe to re-run on a database that
		// was created before a template was added, which is the only way an existing database ever
		// gets a new built-in category. A type the user deleted stays deleted unless something calls
		// this again on purpose - open() does not.
		auto ensureType = [&db](const PartType& type) -> int
		{
			for (const PartType& existing : listTypes(db))
			{
				if (existing.name == type.name)
				{
					return existing.id;
				}
			}
			return insertType(db, type);
		};

		auto addAttr = [&db](int typeId, const std::string& key, const std::string& label,
			const std::string& unit, AttributeDataType datatype, bool searchable,
			const std::vector<std::string>& enumOptions = {})
		{
			if (typeId == NoParentType)
			{
				return;
			}
			for (const PartTypeAttribute& existing : listOwnAttributes(db, typeId))
			{
				if (existing.key == key)
				{
					return;   // already there, leave the user's edits to it alone
				}
			}
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
		int resistorId = ensureType(resistor);
		addAttr(resistorId, "resistance", "Resistance", "\xCE\xA9", AttributeDataType::Dimension, true);
		addAttr(resistorId, "tolerance", "Tolerance", "%", AttributeDataType::Dimension, false);
		addAttr(resistorId, "power", "Power", "W", AttributeDataType::Dimension, false);

		PartType capacitor;
		capacitor.name = "Capacitor";
		capacitor.domain = "electronic";
		capacitor.kicadRelevant = true;
		capacitor.kicadCategory = "Capacitors";
		int capacitorId = ensureType(capacitor);
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
		int ceramicCapacitorId = ensureType(ceramicCapacitor);
		addAttr(ceramicCapacitorId, "dielectric", "Dielectric", "", AttributeDataType::Text, false);

		PartType inductor;
		inductor.name = "Inductor";
		inductor.domain = "electronic";
		inductor.kicadRelevant = true;
		inductor.kicadCategory = "Inductors";
		int inductorId = ensureType(inductor);
		addAttr(inductorId, "inductance", "Inductance", "H", AttributeDataType::Dimension, true);
		addAttr(inductorId, "current_rating", "Current rating", "A", AttributeDataType::Dimension, false);

		PartType powerRegulator;
		powerRegulator.name = "Power Regulator";
		powerRegulator.domain = "electronic";
		powerRegulator.kicadRelevant = true;
		powerRegulator.kicadCategory = "ICs";
		int powerRegulatorId = ensureType(powerRegulator);
		addAttr(powerRegulatorId, "output_voltage", "Output Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(powerRegulatorId, "max_current", "Max Current", "A", AttributeDataType::Dimension, false);
		addAttr(powerRegulatorId, "regulator_type", "Regulator Type", "", AttributeDataType::Enum, false,
			{ "Linear", "Switching" });

		PartType transistor;
		transistor.name = "Transistor";
		transistor.domain = "electronic";
		transistor.kicadRelevant = true;
		transistor.kicadCategory = "Transistors";
		int transistorId = ensureType(transistor);

		PartType mosfet;
		mosfet.name = "MOSFET";
		mosfet.domain = "electronic";
		mosfet.kicadRelevant = true;
		mosfet.kicadCategory = "Transistors";
		mosfet.parentTypeId = transistorId;
		int mosfetId = ensureType(mosfet);
		addAttr(mosfetId, "vds_max", "Vds Max", "V", AttributeDataType::Dimension, false);
		addAttr(mosfetId, "id_max", "Id Max", "A", AttributeDataType::Dimension, false);
		addAttr(mosfetId, "channel_type", "Channel Type", "", AttributeDataType::Enum, false,
			{ "N-Channel", "P-Channel" });

		// The three templates the user's real stock (.claude/DefaultParts.csv) needs and the six
		// above do not cover. Mouser returns no parametric attributes at all for these parts (only
		// Packaging/Standard Pack Qty), so the attribute sets come from the datasheets' headline
		// ratings, not from the API — deliberately short; a missing attribute is cheap to add later,
		// a wrong one is already in every part row by then.
		PartType diode;
		diode.name = "Diode";
		diode.domain = "electronic";
		diode.kicadRelevant = true;
		diode.kicadCategory = "Diodes";
		int diodeId = ensureType(diode);
		addAttr(diodeId, "forward_voltage", "Forward Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(diodeId, "reverse_voltage", "Reverse Voltage", "V", AttributeDataType::Dimension, true);
		addAttr(diodeId, "forward_current", "Forward Current", "A", AttributeDataType::Dimension, true);
		addAttr(diodeId, "diode_type", "Diode Type", "", AttributeDataType::Enum, false,
			{ "Schottky", "Switching", "Rectifier", "Zener", "TVS" });

		// Not a child of Diode on purpose: it would inherit `diode_type`, which is meaningless for
		// an LED, and §2b has no way to drop an inherited attribute.
		PartType led;
		led.name = "LED";
		led.domain = "electronic";
		led.kicadRelevant = true;
		led.kicadCategory = "LEDs";
		int ledId = ensureType(led);
		addAttr(ledId, "color", "Colour", "", AttributeDataType::Enum, true,
			{ "Red", "Green", "Blue", "Yellow", "White", "Orange", "Infrared", "UV" });
		addAttr(ledId, "forward_voltage", "Forward Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(ledId, "forward_current", "Forward Current", "A", AttributeDataType::Dimension, false);

		// The rest of the everyday bench vocabulary. Same rule as above: two or three attributes each,
		// only the ones you would actually filter or order by. Anything rarer belongs in a template the
		// user adds themselves.
		PartType connector;
		connector.name = "Connector";
		connector.domain = "electronic";
		connector.kicadRelevant = true;
		connector.kicadCategory = "Connectors";
		int connectorId = ensureType(connector);
		addAttr(connectorId, "pin_count", "Pin Count", "", AttributeDataType::Number, true);
		addAttr(connectorId, "pitch", "Pitch", "mm", AttributeDataType::Dimension, true);
		addAttr(connectorId, "current_rating", "Current rating", "A", AttributeDataType::Dimension, false);

		PartType crystal;
		crystal.name = "Crystal / Oscillator";
		crystal.domain = "electronic";
		crystal.kicadRelevant = true;
		crystal.kicadCategory = "Crystals";
		int crystalId = ensureType(crystal);
		addAttr(crystalId, "frequency", "Frequency", "Hz", AttributeDataType::Dimension, true);
		addAttr(crystalId, "load_capacitance", "Load Capacitance", "F", AttributeDataType::Dimension, false);
		addAttr(crystalId, "tolerance", "Tolerance", "%", AttributeDataType::Dimension, false);

		PartType microcontroller;
		microcontroller.name = "Microcontroller";
		microcontroller.domain = "electronic";
		microcontroller.kicadRelevant = true;
		microcontroller.kicadCategory = "ICs";
		int microcontrollerId = ensureType(microcontroller);
		addAttr(microcontrollerId, "core", "Core", "", AttributeDataType::Text, false);
		addAttr(microcontrollerId, "flash_size", "Flash Size", "", AttributeDataType::Number, true);
		addAttr(microcontrollerId, "supply_voltage", "Supply Voltage", "V", AttributeDataType::Dimension, false);

		PartType opAmp;
		opAmp.name = "Op-Amp";
		opAmp.domain = "electronic";
		opAmp.kicadRelevant = true;
		opAmp.kicadCategory = "ICs";
		int opAmpId = ensureType(opAmp);
		addAttr(opAmpId, "channels", "Channels", "", AttributeDataType::Number, true);
		addAttr(opAmpId, "bandwidth", "Bandwidth", "Hz", AttributeDataType::Dimension, false);
		addAttr(opAmpId, "supply_voltage", "Supply Voltage", "V", AttributeDataType::Dimension, false);

		PartType logicIc;
		logicIc.name = "Logic IC";
		logicIc.domain = "electronic";
		logicIc.kicadRelevant = true;
		logicIc.kicadCategory = "ICs";
		int logicIcId = ensureType(logicIc);
		addAttr(logicIcId, "logic_family", "Logic Family", "", AttributeDataType::Text, false);
		addAttr(logicIcId, "channels", "Channels", "", AttributeDataType::Number, true);
		addAttr(logicIcId, "supply_voltage", "Supply Voltage", "V", AttributeDataType::Dimension, false);

		PartType switchType;
		switchType.name = "Switch";
		switchType.domain = "electronic";
		switchType.kicadRelevant = true;
		switchType.kicadCategory = "Switches";
		int switchId = ensureType(switchType);
		addAttr(switchId, "switch_type", "Switch Type", "", AttributeDataType::Enum, true,
			{ "Tactile", "Toggle", "Slide", "Rotary", "DIP", "Push-Button" });
		addAttr(switchId, "current_rating", "Current rating", "A", AttributeDataType::Dimension, false);
		addAttr(switchId, "voltage", "Voltage", "V", AttributeDataType::Dimension, false);

		PartType relay;
		relay.name = "Relay";
		relay.domain = "electronic";
		relay.kicadRelevant = true;
		relay.kicadCategory = "Relays";
		int relayId = ensureType(relay);
		addAttr(relayId, "coil_voltage", "Coil Voltage", "V", AttributeDataType::Dimension, true);
		addAttr(relayId, "current_rating", "Contact Current", "A", AttributeDataType::Dimension, false);
		addAttr(relayId, "relay_type", "Relay Type", "", AttributeDataType::Enum, false,
			{ "Mechanical", "Solid State", "Reed" });

		PartType fuse;
		fuse.name = "Fuse";
		fuse.domain = "electronic";
		fuse.kicadRelevant = true;
		fuse.kicadCategory = "Fuses";
		int fuseId = ensureType(fuse);
		addAttr(fuseId, "current_rating", "Current rating", "A", AttributeDataType::Dimension, true);
		addAttr(fuseId, "voltage", "Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(fuseId, "fuse_type", "Fuse Type", "", AttributeDataType::Enum, false,
			{ "Fast-Blow", "Slow-Blow", "PTC Resettable" });

		PartType sensor;
		sensor.name = "Sensor";
		sensor.domain = "electronic";
		sensor.kicadRelevant = true;
		sensor.kicadCategory = "Sensors";
		int sensorId = ensureType(sensor);
		addAttr(sensorId, "sensor_type", "Sensor Type", "", AttributeDataType::Enum, true,
			{ "Hall Effect", "Temperature", "Current", "Pressure", "Optical" });
		addAttr(sensorId, "supply_voltage", "Supply Voltage", "V", AttributeDataType::Dimension, false);
		addAttr(sensorId, "output_type", "Output Type", "", AttributeDataType::Enum, false,
			{ "Analog", "Digital", "PWM", "I2C", "SPI" });

		return seedDefaultFileSlots(db) && seedDefaultSearchKeywords(db);
	}

	bool PartTypeRepository::seedDefaultSearchKeywords(SQLiteWrapper::SQLite& db)
	{
		// The words you would actually type looking for one of these, in both languages the app
		// runs in — a German user hunting a resistor types "Widerstand" as readily as "R", and the
		// list is the only place either of those words exists. Short and deliberately not
		// exhaustive: a keyword that matches too much is worse than a missing one, and the list is
		// editable per type and per part.
		// Only *empty* lists are filled, so a list the user has edited is never overwritten.
		struct Defaults { const char* type; const char* keywords; };
		static const Defaults defaults[] = {
			{ "Resistor",            "R\nRes\nOhm\nWiderstand\nresistor" },
			{ "Capacitor",           "C\nCap\nFarad\nKondensator\ncapacitor" },
			{ "Ceramic Capacitor",   "MLCC\nceramic\nKeramik\nX7R\nC0G\nNP0" },
			{ "Inductor",            "L\nCoil\nSpule\nDrossel\nHenry\ninductor\nchoke" },
			{ "Power Regulator",     "LDO\nRegler\nSpannungsregler\nregulator\nbuck\nboost\nVREG" },
			{ "Transistor",          "Q\nTransistor\ntransistor" },
			{ "MOSFET",              "FET\nMOSFET\nN-Channel\nP-Channel" },
			{ "Diode",               "D\nDiode\ndiode\nrectifier\nGleichrichter\nSchottky" },
			{ "LED",                 "LED\nlight\nLicht\nLeuchtdiode\ncolour\ncolor\nFarbe" },
			{ "Connector",           "J\nConnector\nStecker\nBuchse\nheader\nPfostenleiste\nsocket" },
			{ "Crystal / Oscillator","Y\nQuarz\nCrystal\nXTAL\nOszillator\noscillator" },
			{ "Microcontroller",     "U\nMCU\nMicrocontroller\nMikrocontroller\ncontroller\nCPU" },
			{ "Op-Amp",              "U\nOpAmp\nOPV\nOperationsverstaerker\namplifier\nVerstaerker" },
			{ "Logic IC",            "U\nLogic\nLogik\ngate\nGatter\n74HC\nCMOS\nTTL" },
			{ "Switch",              "SW\nSwitch\nSchalter\nTaster\nbutton\nKnopf" },
			{ "Relay",               "K\nRelay\nRelais" },
			{ "Fuse",                "F\nFuse\nSicherung\nPTC" },
			{ "Sensor",              "Sensor\nsensor\nFuehler\ndetector\nDetektor" },
		};

		for (PartType type : listTypes(db))
		{
			if (!type.searchKeywords.empty())
			{
				continue;
			}
			for (const Defaults& row : defaults)
			{
				if (type.name == row.type)
				{
					type.searchKeywords = row.keywords;
					updateType(db, type);
					break;
				}
			}
		}
		return true;
	}

	bool PartTypeRepository::ensureLateAddedColumns(SQLiteWrapper::SQLite& db)
	{
		// CREATE TABLE IF NOT EXISTS does nothing to a table that is already there, so an existing
		// database needs the column added by hand — the same shape as ensureAttrColumn().
		auto ensureColumn = [&db](const char* table, const char* column)
		{
			for (const std::vector<std::string>& row :
				db.fetchAll(std::string("PRAGMA table_info(") + table + ");"))
			{
				if (row.size() > 1 && row[1] == column)
				{
					return true;
				}
			}
			return db.execute(std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " TEXT;");
		};
		bool ok = ensureColumn("part_type", "search_keywords");
		ok = ensureColumn("part", "search_keywords") && ok;
		ok = ensureColumn("part_type", "excluded_keywords") && ok;
		ok = ensureColumn("part", "excluded_keywords") && ok;
		return ensureColumn("part_type", "name_template") && ok;
	}

	bool PartTypeRepository::seedDefaultFileSlots(SQLiteWrapper::SQLite& db)
	{
		// The four files an electronic part is expected to carry, in the order the part editor
		// should ask for them. Only the datasheet is required: it is the one a part is useless
		// without, and it is the one that exists for every part whether or not it has ever been
		// near KiCad. Marking the KiCad three required would make every passive in the stock
		// incomplete, which turns the flag into noise nobody reads.
		struct Slot { PartFileRole role; const char* label; bool required; };
		// Not named `slots`: that is a Qt keyword macro, and the error it produces blames the line
		// after it. (core/ is Qt-free, but PartManager_global.h is not, and the macro travels.)
		static const Slot defaults[] = {
			{ PartFileRole::Datasheet,      "Datasheet",       true  },
			{ PartFileRole::KicadSymbol,    "KiCad Symbol",    false },
			{ PartFileRole::KicadFootprint, "KiCad Footprint", false },
			{ PartFileRole::Kicad3DModel,   "3D Model",        false },
		};

		for (const PartType& type : listTypes(db))
		{
			// Roots only. A subtype inherits every slot its parent declares (§2b), so declaring
			// them again on Ceramic Capacitor would be a duplicate row saying the same thing.
			if (type.domain != "electronic" || type.parentTypeId != NoParentType)
			{
				continue;
			}
			const std::vector<PartTypeFileSlot> existing = listOwnFileSlots(db, type.id);
			int order = 0;
			for (const Slot& wanted : defaults)
			{
				const std::string role = toString(wanted.role);
				bool alreadyThere = false;
				for (const PartTypeFileSlot& row : existing)
				{
					alreadyThere = alreadyThere || row.role == role;
				}
				if (!alreadyThere)
				{
					PartTypeFileSlot slot;
					slot.partTypeId = type.id;
					slot.role = role;
					slot.label = wanted.label;
					slot.required = wanted.required;
					slot.sortOrder = order;
					insertFileSlot(db, slot);
				}
				++order;
			}
		}
		return true;
	}

#endif

}
