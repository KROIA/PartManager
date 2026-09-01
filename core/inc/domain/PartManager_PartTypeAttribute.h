// @file PartManager_PartTypeAttribute.h
// @brief Plain data class for a `part_type_attribute` row (§2) — one data field on a type template.
//
// Zero Qt, zero SQL, zero business logic. `unit` is a plain fixed-vocabulary
// string from the §2a dropdown table (e.g. "Ω", "F", "mm") — parsing SI
// prefixes out of a typed value is a separate later module (`core/units`),
// not this class's job.
// @see docs/design/ARCHITECTURE.md §2, §2a
// @see PartManager_PartType.h
#pragma once

#include "PartManager_global.h"
#include <string>
#include <vector>

namespace PartManager
{

	// One `part_type_attribute.datatype` value.
	enum class PART_MANAGER_API AttributeDataType
	{
		Number,     // plain number, no unit
		Dimension,  // number + unit, e.g. {value: 4700, unit: "Ohm"} (§2a)
		Text,
		Bool,
		Enum        // fixed set, see PartTypeAttribute::enumOptions
	};

	// AttributeDataType <-> the TEXT value stored in part_type_attribute.datatype.
	PART_MANAGER_API std::string toString(AttributeDataType type);
	PART_MANAGER_API AttributeDataType attributeDataTypeFromString(const std::string& text);

	// One `part_type_attribute` row — a single data field defined on a type template.
	struct PART_MANAGER_API PartTypeAttribute
	{
		int id = 0;                        // SQLite primary key; 0 = not yet inserted
		int partTypeId = 0;                 // FK -> PartType::id
		std::string key;                  // 'resistance', 'thread_size' — stable, used as attributes JSON key
		std::string label;                 // 'Resistance'
		std::string unit;                  // §2a dropdown value, e.g. "Ω"; empty = "(no unit)"
		AttributeDataType datatype = AttributeDataType::Text; // field's value kind, see AttributeDataType above
		std::vector<std::string> enumOptions; // only meaningful when datatype == Enum
		bool searchable = false;           // 1 => gets an app-maintained attr_<key> fast-filter column
		bool required = false;             // blocks part creation until filled
		std::string tooltip;               // (i)-hint text shown next to the field
		int sortOrder = 0;                  // display ordering
	};

}

