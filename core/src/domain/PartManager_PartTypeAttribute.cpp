#include "domain/PartManager_PartTypeAttribute.h"

namespace PartManager
{

	// AttributeDataType -> the TEXT value stored in part_type_attribute.datatype.
	std::string toString(AttributeDataType type)
	{
		switch (type)
		{
		case AttributeDataType::Number:    return "number";
		case AttributeDataType::Dimension: return "dimension";
		case AttributeDataType::Text:      return "text";
		case AttributeDataType::Bool:      return "bool";
		case AttributeDataType::Enum:      return "enum";
		}
		return "text";
	}

	// The TEXT value stored in part_type_attribute.datatype -> AttributeDataType. Unknown text defaults to Text.
	AttributeDataType attributeDataTypeFromString(const std::string& text)
	{
		if (text == "number")    return AttributeDataType::Number;
		if (text == "dimension") return AttributeDataType::Dimension;
		if (text == "bool")      return AttributeDataType::Bool;
		if (text == "enum")      return AttributeDataType::Enum;
		return AttributeDataType::Text;
	}

}
