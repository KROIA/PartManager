// @file PartManager_global.h
// @brief DLL export/import macros, compiler portability helpers, and warning configuration.
//
// This header defines PART_MANAGER_API which resolves to __declspec(dllexport)
// when building the library, __declspec(dllimport) when consuming the shared
// library, or nothing for static builds. It also promotes selected MSVC
// warnings to errors to catch common bugs at compile time.
#pragma once

/// USER_SECTION_START 1

/// USER_SECTION_END

#include <chrono>

// DLL export/import switch.
// BUILD_STATIC is defined by CMake for the static library targets.
// LIBRARY_NAME_LIB is defined only when compiling the library itself.
#ifndef BUILD_STATIC
	#if defined(PARTMANAGER_LIB)
		#define PART_MANAGER_API __declspec(dllexport)
	#else
		#define PART_MANAGER_API __declspec(dllimport)
	#endif
#else
	#define PART_MANAGER_API
#endif

/// USER_SECTION_START 2

/// USER_SECTION_END

#ifdef QT_ENABLED
	#ifdef QT_WIDGETS_ENABLED
	#endif
#endif

/// USER_SECTION_START 3

/// USER_SECTION_END

// Portable __PRETTY_FUNCTION__ and high-resolution time point.
#ifdef _MSC_VER
	#define __PRETTY_FUNCTION__ __FUNCSIG__
	typedef std::chrono::steady_clock::time_point TimePoint;
#else
	typedef std::chrono::system_clock::time_point TimePoint;
#endif

// Suppress "unused parameter" warnings for intentionally unused variables.
#define PM_UNUSED(x) (void)x;

/// USER_SECTION_START 4

/// USER_SECTION_END

// Promote selected MSVC warnings to errors while compiling the library.
// Only active when LIBRARY_NAME_LIB is defined (i.e. building the library,
// not when a consumer includes the headers). Comment out via USER_SECTION 4/5
// if you want to disable this behaviour (see ModifyingTheProject.md).
#if defined(PARTMANAGER_LIB)
    #pragma warning (error : 4715) // not all control paths return a value
    #pragma warning (error : 4700) // uninitialized local variable used
	#pragma warning (error : 4244) // implicit type conversion (possible loss of data)
	#pragma warning (error : 4100) // unreferenced formal parameter
	#pragma warning (error : 4018) // signed/unsigned mismatch
	#pragma warning (error : 4996) // deprecated / unsafe function call
	#pragma warning (error : 4456) // declaration hides previous local declaration
	#pragma warning (error : 4065) // switch contains 'default' but no 'case' labels
	#pragma warning (error : 4189) // local variable initialized but not referenced
	#pragma warning (error : 4172) // returning address of local variable or temporary
#endif

/// USER_SECTION_START 5

/// USER_SECTION_END