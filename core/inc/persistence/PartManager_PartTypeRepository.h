// @file PartManager_PartTypeRepository.h
// @brief CRUD for `part_type`/`part_type_attribute`/`part_type_file_slot` + §2b inheritance resolution.
//
// Static utility class operating on an already-open `SQLiteWrapper::SQLite`
// connection (see PartManager_DatabaseHandle.h::connection()) — mirrors the
// DatabaseMetadata/SchemaMigrator style from core/database. `effectiveAttributes()`/
// `effectiveFileSlots()`/`effectiveKicadCategory()`/`effectiveDomain()` implement
// the §2b resolution rule: walk root ancestor -> target type, child rows
// override an ancestor's same-key row, ordering = ancestor's own sort_order
// first then the type's own new/overridden rows.
//
// When an attribute is saved with `searchable = true` and a numeric datatype
// (Number/Dimension), `insertAttribute()`/`updateAttribute()` add the matching
// `attr_<key>` fast-filter column to `part` via ALTER TABLE right then (chosen
// over doing it lazily in PartRepository, so the column always exists by the
// time any part of that type is saved — see DECISIONS.md).
// @see docs/design/ARCHITECTURE.md §2, §2b
// @see PartManager_PartType.h, PartManager_PartTypeAttribute.h, PartManager_PartTypeFileSlot.h
#pragma once

#include "PartManager_global.h"
#include "domain/PartManager_PartType.h"
#include "domain/PartManager_PartTypeAttribute.h"
#include "domain/PartManager_PartTypeFileSlot.h"
#include <string>
#include <vector>

#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
namespace SQLiteWrapper { class SQLite; }
#endif

namespace PartManager
{

	class PART_MANAGER_API PartTypeRepository
	{
		PartTypeRepository() = delete;
	public:
#if SQLITEWRAPPER_LIBRARY_AVAILABLE == 1
		// Creates part_type/part_type_attribute/part_type_file_slot if missing. Idempotent.
		static bool createSchema(SQLiteWrapper::SQLite& db);

		// part_type CRUD
		// Inserts a new type, returns its new id (NoParentType on failure).
		static int insertType(SQLiteWrapper::SQLite& db, const PartType& type);
		static bool updateType(SQLiteWrapper::SQLite& db, const PartType& type);
		static bool deleteType(SQLiteWrapper::SQLite& db, int typeId);
		// Looks up a single type by id. Returns false if not found.
		static bool findType(SQLiteWrapper::SQLite& db, int typeId, PartType& outType);
		static std::vector<PartType> listTypes(SQLiteWrapper::SQLite& db);

		// part_type_attribute CRUD (own rows only, not inherited — see effectiveAttributes()).
		// Adds the attr_<key> column to `part` when searchable + numeric (see header note).
		static int insertAttribute(SQLiteWrapper::SQLite& db, const PartTypeAttribute& attribute);
		static bool updateAttribute(SQLiteWrapper::SQLite& db, const PartTypeAttribute& attribute);
		static bool deleteAttribute(SQLiteWrapper::SQLite& db, int attributeId);
		static std::vector<PartTypeAttribute> listOwnAttributes(SQLiteWrapper::SQLite& db, int typeId);

		// part_type_file_slot CRUD (own rows only, not inherited — see effectiveFileSlots()).
		static int insertFileSlot(SQLiteWrapper::SQLite& db, const PartTypeFileSlot& slot);
		static bool updateFileSlot(SQLiteWrapper::SQLite& db, const PartTypeFileSlot& slot);
		static bool deleteFileSlot(SQLiteWrapper::SQLite& db, int fileSlotId);
		static std::vector<PartTypeFileSlot> listOwnFileSlots(SQLiteWrapper::SQLite& db, int typeId);

		// §2b resolution: root ancestor -> typeId, ancestor rows first (by their own sort_order),
		// then typeId's own new/overridden rows; a same-key row on typeId replaces the ancestor's.
		static std::vector<PartTypeAttribute> effectiveAttributes(SQLiteWrapper::SQLite& db, int typeId);
		static std::vector<PartTypeFileSlot> effectiveFileSlots(SQLiteWrapper::SQLite& db, int typeId);
		// Nearest ancestor (starting at typeId itself) with a non-empty kicad_category.
		static std::string effectiveKicadCategory(SQLiteWrapper::SQLite& db, int typeId);
		// Nearest ancestor (starting at typeId itself) with a non-empty domain.
		static std::string effectiveDomain(SQLiteWrapper::SQLite& db, int typeId);

		// Seeds the 18 built-in type templates (Resistor, Capacitor, Ceramic Capacitor, Inductor,
		// Power Regulator, Transistor, MOSFET, Diode, LED, Connector, Crystal / Oscillator,
		// Microcontroller, Op-Amp, Logic IC, Switch, Relay, Fuse, Sensor) on a fresh database. No-op (returns true) if part_type already has rows.
		static bool seedDefaultTypes(SQLiteWrapper::SQLite& db);
#endif

	};

}

