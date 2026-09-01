// @file PartManager_info.h
// @brief Compile-time library metadata and version information.
//
// LibraryInfo exposes name, version, author, licence, compiler, and build
// type as constexpr values. It also provides helpers to print or display
// this information (including a Qt widget when Widgets are available).
// Version numbers are generated from LIBRARY_VERSION in CMakeLists.txt via
// the LibraryName_meta.h template.
#pragma once

/// USER_SECTION_START 1

/// USER_SECTION_END

#include "PartManager_global.h"
#include "PartManager_debug.h"
#include "PartManager_meta.h"

#include <sstream>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

/// USER_SECTION_START 2

/// USER_SECTION_END

class QWidget;

/// USER_SECTION_START 3

/// USER_SECTION_END

namespace PartManager
{

/// USER_SECTION_START 4

/// USER_SECTION_END

	// Compile-time metadata about the library (non-instantiable).
	class PART_MANAGER_API LibraryInfo
	{
		LibraryInfo() = delete;
		LibraryInfo(const LibraryInfo&) = delete;
	public:

		// Semantic version triplet with full comparison operators.
		struct Version
		{
			int major;
			int minor;
			int patch;

			bool operator<(const Version& other) const;
			bool operator==(const Version& other) const;
			bool operator!=(const Version& other) const;
			bool operator>(const Version& other) const;
			bool operator<=(const Version& other) const;
			bool operator>=(const Version& other) const;

			// Returns the version formatted as "MM.mm.pppp" with leading zeros.
			std::string toString() const;
		};


		// Version — auto-generated from LIBRARY_VERSION in CMakeLists.txt.
		static constexpr int versionMajor				= PartManager_VERSION_MAJOR;
		static constexpr int versionMinor				= PartManager_VERSION_MINOR;
		static constexpr int versionPatch				= PartManager_VERSION_PATCH;

		static constexpr Version version{ versionMajor, versionMinor, versionPatch };

		// Metadata — edit these fields to describe your library.
		static constexpr const char* name				= PartManager_LIBRARY_NAME;
		static constexpr const char* author				= "Alex Krieg";
		static constexpr const char* email				= "";
		static constexpr const char* website			= "";
		static constexpr const char* license			= "MIT";
		static constexpr const char* compilationDate	= __DATE__;
		static constexpr const char* compilationTime	= __TIME__;

		// Compiler detection (resolved at compile time).
#ifdef _MSC_VER
		static constexpr const char* compiler			= "MSVC";
		static constexpr const char* compilerVersion	= TOSTRING(_MSC_VER);
#elif defined(__GNUC__)
		static constexpr const char* compiler			= "GCC";
		static constexpr const char* compilerVersion	= __VERSION__;
#elif defined(__clang__)
		static constexpr const char* compiler			= "Clang";
		static constexpr const char* compilerVersion	= __clang_version__;
#else
		static constexpr const char* compiler			= "Unknown";
		static constexpr const char* compilerVersion	= "Unknown";
#endif

		// Build type (Debug / Release).
		enum class BuildType
		{
			debug,
			release
		};
#ifdef NDEBUG
		static constexpr const char* buildTypeStr		= "Release";
		static constexpr const BuildType buildType		= BuildType::release;
#else
		static constexpr const char* buildTypeStr		= "Debug";
		static constexpr const BuildType buildType		= BuildType::debug;
#endif

		// Print all metadata to stdout.
		static void printInfo();
		// Print all metadata to the given stream.
		static void printInfo(std::ostream& stream);
		// Return all metadata as a multi-line string.
		static std::string getInfoStr();

		// Create a QWidget displaying the library metadata.
		// Requires QT_ENABLE=ON and "Widgets" in QT_MODULES; returns nullptr otherwise.
		// The caller takes ownership of the widget.
		static QWidget *createInfoWidget(QWidget* parent = nullptr, bool disableHyperlink = false);

/// USER_SECTION_START 5

/// USER_SECTION_END
	};

/// USER_SECTION_START 6

/// USER_SECTION_END
}

/// USER_SECTION_START 7

/// USER_SECTION_END