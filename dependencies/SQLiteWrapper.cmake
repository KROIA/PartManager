## description: C++ SQLite3 wrapper library that provides a simple and easy to use interface for SQLite3 and some utilities for working in a multi user environment.

function(dep LIBRARY_MACRO_NAME SHARED_LIB STATIC_LIB STATIC_PROFILE_LIB INCLUDE_PATHS)
    # Define the git repository and tag to download from
    set(LIB_NAME SQLiteWrapper)
    set(LIB_MACRO_NAME SQLITEWRAPPER_LIBRARY_AVAILABLE)
    set(GIT_REPO https://github.com/KROIA/SQLiteWrapper.git)
    set(GIT_TAG main)
	set(NO_EXAMPLES True)	
	set(NO_UNITTESTS True)
	set(ADDITIONAL_INCLUDE_PATHS )

    set(SHARED_LIB_BAK "${${SHARED_LIB}}")
    downloadStandardLibrary()
    set(${SHARED_LIB} "${SHARED_LIB_BAK};${LIB_NAME}_static" PARENT_SCOPE)
endfunction()

dep(DEPENDENCY_NAME_MACRO 
    DEPENDENCIES_FOR_SHARED_LIB 
    DEPENDENCIES_FOR_STATIC_LIB 
    DEPENDENCIES_FOR_STATIC_PROFILE_LIB 
    DEPENDENCIES_INCLUDE_PATHS)
