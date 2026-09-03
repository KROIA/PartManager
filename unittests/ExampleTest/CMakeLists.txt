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
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_OrderController.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_KicadController.cpp")

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
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_PartlistPanel.cpp")
## ...whose "+ Add Part" and Part-column repair both open the picker.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartPickerDialog.cpp")
## ...and a row's context menu opens the part editor.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartEditorDialog.cpp")
## The partlist panel's CSV import and its "Order Missing Parts" both open dialogs, so the
## whole §4/§5 tail comes along -- the panel alone would compile but not link.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartlistImportDialog.cpp")
## ...and the import wizard's "look this row up on Mouser" opens the search dialog.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_MouserSearchDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_OrderManagerDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_CartStagingDialog.cpp")
## ...and confirming an arrival opens the per-package dialog.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_ReceiveArrivalDialog.cpp")
## ...and New Part offers the §5c symbol/footprint download right after Create.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_EcadFetchDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_AttributeFormWidget.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_DimensionLineEdit.cpp")
## The part editor and the main window preview both embed the KiCad symbol/footprint painter.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_KicadPreviewWidget.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_TypeIconPainter.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_AttachmentIconPainter.cpp")

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

## Every suite is a header full of templates included into one translation unit, so main.cpp's
## object file grows a COMDAT section per test function. It crossed MSVC's 65279-section limit at
## 29 suites (C1128). /bigobj raises it; splitting tests.h across several .cpp files would too,
## but this is one line instead of a per-suite registration dance.
if(MSVC)
    target_compile_options(ExampleTest PRIVATE /bigobj)
endif()

## USER_SECTION_START 5

## TST_EcadArchive builds its fixture archive with QZipWriter, the write half of the private
## QtGui zip API core reads with. Same include path, same reasoning (PartManager_EcadArchive.h).
## The imported target, not Qt5Gui_PRIVATE_INCLUDE_DIRS: that variable is only set the first time
## find_package(Qt5Gui) runs, and by the time this file gets to say anything the package is
## already found, so reading the variable here silently yields nothing. The target persists.
## Plain signature, no PRIVATE keyword: exampleMaster() already linked this target the plain way,
## and CMake refuses to mix the two forms on one target.
## CorePrivate too: qtguiglobal_p.h includes QtCore/private/qglobal_p.h.
if(TARGET Qt5::GuiPrivate)
    target_link_libraries(ExampleTest Qt5::CorePrivate Qt5::GuiPrivate)
endif()

## USER_SECTION_END