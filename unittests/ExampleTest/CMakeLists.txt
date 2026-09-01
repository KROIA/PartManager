## 
## This file creates a new target exe with the given parameters
## Override any settings if needed.
## If any setting is not overriden, the default value from the library will be used.
##

## USER_SECTION_START 1

## The app's controllers are plain classes with no widget dependency (§12b), so the
## Home-tab logic they hold is testable here — pull in the app's src/ include root and
## compile the one controller under test straight into this target.
set(APP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../examples/PartManagerApp/src")
include_directories(${APP_SRC_DIR})
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_MainWindowController.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_PartEditorController.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_StockController.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_PartlistController.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_DatabaseSelectorController.cpp")

## The app's display setup, so the GUI tests render at the same scale and font size the user's
## app does. A suite that runs unscaled cannot see a layout that only breaks on the real screen.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/PartManager_AppStartup.cpp")

## The GUI driver ships with the UnitTest library, but stays out of its build behind
## UNITTEST_GUI_ENABLED so the library itself needs no Qt. A target that wants it defines the macro
## and compiles the one .cpp in, which is what happens here.
FetchContent_GetProperties(UnitTest)    # gives us unittest_SOURCE_DIR in this scope
include_directories("${unittest_SOURCE_DIR}/core/inc/gui")
list(APPEND ADDITONAL_SOURCES "${unittest_SOURCE_DIR}/core/src/gui/UnitTest_Gui.cpp")
add_definitions(-DUNITTEST_GUI_ENABLED)

## One real app dialog is compiled in as well, so the GUI driver is proven against a screen the
## user actually sees and not only against synthetic widgets. AUTOUIC turns its .ui into the
## ui_*.h the dialog includes.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_StockDialog.cpp")

## The §6 Mouser prefill lands on New Part's runtime-generated attribute rows, so that whole
## widget stack has to come along -- the dialog alone would link but render an empty form.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_NewPartDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartlistEditorDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_AttributeFormWidget.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_DimensionLineEdit.cpp")

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

## A unittest target will use an additional library
list(APPEND ADDITIONAL_LIBRARIES UnitTest_static) 

## USER_SECTION_START 4

## USER_SECTION_END

## Do not change the first 2 parameters             
##             Do not change      Do not change      
##                 V                  V
exampleMaster(${LIBRARY_NAME} ${LIB_PROFILE_DEFINE} ${QT_ENABLE} ${QT_DEPLOY} "${QT_MODULES}" "${ADDITONAL_SOURCES}" "${ADDITIONAL_LIBRARIES}" "${INSTALL_BIN_PATH}")

## The app's .ui files live in the app tree, not here, so AUTOUIC has to be told where to look.
set_target_properties(ExampleTest PROPERTIES AUTOUIC ON AUTOUIC_SEARCH_PATHS "${APP_SRC_DIR}/ui")

## USER_SECTION_START 5

## USER_SECTION_END