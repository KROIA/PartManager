#include "domain/PartManager_PartFileRole.h"

namespace PartManager
{

	// PartFileRole -> the TEXT value stored in part_file.role / part_type_file_slot.role.
	std::string toString(PartFileRole role)
	{
		switch (role)
		{
		case PartFileRole::Datasheet:      return "datasheet";
		case PartFileRole::KicadSymbol:    return "kicad_symbol";
		case PartFileRole::KicadFootprint: return "kicad_footprint";
		case PartFileRole::Kicad3DModel:   return "kicad_3dmodel";
		case PartFileRole::Image:          return "image";
		case PartFileRole::Other:          return "other";
		}
		return "other";
	}

	// The TEXT value stored in part_file.role / part_type_file_slot.role -> PartFileRole. Unknown text defaults to Other.
	PartFileRole partFileRoleFromString(const std::string& text)
	{
		if (text == "datasheet")       return PartFileRole::Datasheet;
		if (text == "kicad_symbol")    return PartFileRole::KicadSymbol;
		if (text == "kicad_footprint") return PartFileRole::KicadFootprint;
		if (text == "kicad_3dmodel")   return PartFileRole::Kicad3DModel;
		if (text == "image")           return PartFileRole::Image;
		return PartFileRole::Other;
	}

}
