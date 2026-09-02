## 
## This file creates a new target exe with the given parameters
## Override any settings if needed.
## If any setting is not overriden, the default value from the library will be used.
##

## USER_SECTION_START 1

## The app's sources live in src/ with the §12b module subfolders (ui/, controllers/, widgets/),
## not in the template's default inc/ — put src/ on the include path so includes like
## "ui/PartManager_MainWindow.h" resolve from anywhere in the tree.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

## §13's 3D viewer. Qt3D only, and only in the app -- core/ stays widget-free (§12a), so the
## library never links these. Qt3D's own geometry loaders cover OBJ/PLY/STL/glTF; STEP is not a
## mesh format and needs a converter, which Model3DFormat reports rather than pretending to open.
set(QT_MODULES
    Core
    Gui
    Network
    Widgets
    3DCore
    3DRender
    3DInput
    3DExtras
)

## USER_SECTION_END

## Override the QT_MODULES if you want to use other modules. 
#[[
set(QT_MODULES
    Core
    Widgets
    Gui
)
]]#


## USER_SECTION_START 2

## USER_SECTION_END

## Enable/disable QT
#set(QT_ENABLE ON)  

## Enable/disable QT deployment. If enabled, windeployqt will be called on the target
#set(QT_DEPLOY ON)    

## Set the target icon resource file
set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/AppIcon.rc")  # Set the icon for the application
list(APPEND ADDITONAL_SOURCES ${APP_ICON})               

## USER_SECTION_START 3

## USER_SECTION_END

list(APPEND ADDITIONAL_LIBRARIES ) 

## USER_SECTION_START 4

## USER_SECTION_END

## Do not change the first 2 parameters             
##             Do not change      Do not change      
##                 V                  V
exampleMaster(${LIBRARY_NAME} ${LIB_PROFILE_DEFINE} ${QT_ENABLE} ${QT_DEPLOY} "${QT_MODULES}" "${ADDITONAL_SOURCES}" "${ADDITIONAL_LIBRARIES}" "${INSTALL_BIN_PATH}")

## USER_SECTION_START 5

## §8 German translation. The .ts is source and is committed; the .qm is build output and is not.
## applyLanguage() looks beside the executable and in translations/ next to it, so the .qm is
## compiled straight into the install tree. Missing Qt tools are not an error: the app then falls
## back to English, which is exactly what it does for the 30% of strings still untranslated.
find_program(PM_LRELEASE NAMES lrelease lrelease.exe
    HINTS "${Qt5_DIR}/../../../bin" "$ENV{QTDIR}/bin")
if(PM_LRELEASE)
    set(PM_TS "${CMAKE_CURRENT_SOURCE_DIR}/translations/PartManager_de.ts")
    set(PM_QM "${INSTALL_BIN_PATH}/translations/PartManager_de.qm")
    ## OUTPUT/DEPENDS, not TARGET ... POST_BUILD. A POST_BUILD command only runs when the target
    ## relinks, and editing a .ts changes no source the app compiles -- so a translation-only edit
    ## silently shipped the previous .qm. Caught 2026-09-02 by comparing file sizes after a build
    ## that reported success.
    add_custom_command(OUTPUT "${PM_QM}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${INSTALL_BIN_PATH}/translations"
        COMMAND "${PM_LRELEASE}" "${PM_TS}" -qm "${PM_QM}"
        DEPENDS "${PM_TS}"
        COMMENT "Compiling German translation (§8)"
        VERBATIM)
    add_custom_target(PartManagerTranslations ALL DEPENDS "${PM_QM}")
    add_dependencies(PartManagerApp PartManagerTranslations)
else()
    message(STATUS "lrelease not found - German translation will not be built, app stays English")
endif()

## USER_SECTION_END