#include "PartManager_Utf8Path.h"

namespace PartManager
{

	std::filesystem::path utf8Path(const std::string& utf8)
	{
		// u8path is the only standard spelling that means "these bytes are UTF-8". It is
		// deprecated in C++20 in favour of char8_t, which C++17 does not have — when this
		// project moves standard, this one function is the only thing that has to move with it.
		return std::filesystem::u8path(utf8);
	}

	std::string pathToUtf8(const std::filesystem::path& path)
	{
		// In C++17 this returns std::string; in C++20 it returns std::u8string, which is the
		// other half of the migration the comment above describes.
		return path.u8string();
	}

}
