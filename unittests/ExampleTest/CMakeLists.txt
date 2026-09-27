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
## §14d: the part editor below carries the "Generate description" button, which names
## LlmController. Nothing here tests the controller -- it is compiled in because the dialog
## that is tested will not link without it. (Its own body is behind QTLLM_LIBRARY_AVAILABLE,
## so a build without QtLLM compiles this to an empty translation unit.)
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/controllers/PartManager_LlmController.cpp")
## ...which registers §14a's UI toolset beside the four core ones. It is an app source because a
## selection is a widget fact and core/ is widget-free, so it has to be named here too — the
## controller above will not link without it. It reaches the window through LlmUiBridge, which is
## a header of pure virtuals, so this adds no dialog and no Qt3D to the test binary.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/llm/PartManager_UiToolset.cpp")
## ...and §5c's downloaded-ECAD-archive tools, which are app sources because they drive
## PartEditorController and read the desktop's Downloads folder. Same reason as above: the
## controller names them, so the link fails without them.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/llm/PartManager_EcadDownloadToolset.cpp")

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
## ...whose category field opens the §2b type tree instead of a flat combo (TST_PartTypePickerDialogGui).
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartTypePickerDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_PartlistPanel.cpp")
## ...whose CSV import and Orders button are modeless now, so they go through the registry that
## keeps one window per screen and deletes them all before the database goes.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_ModelessDialogs.cpp")
## ...whose "+ Add Part" and Part-column repair both open the picker.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartPickerDialog.cpp")
## ...and a row's context menu opens the part editor.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartEditorDialog.cpp")
## The partlist panel's CSV import and its "Order Missing Parts" both open dialogs, so the
## whole §4/§5 tail comes along -- the panel alone would compile but not link.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartlistImportDialog.cpp")
## ...and the import wizard's "look this row up on Mouser" opens the search dialog.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_MouserSearchDialog.cpp")
## The pasted-list migration screen (TST_PartMigrationDialogGui), which drives both of the above.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_PartMigrationDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_OrderManagerDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_CartStagingDialog.cpp")
## ...and confirming an arrival opens the per-package dialog.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_ReceiveArrivalDialog.cpp")
## ...and New Part offers the §5c symbol/footprint download right after Create.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_EcadFetchDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_AttributeFormWidget.cpp")
## ...whose "Unrecognised values" footnote and the §2b move that drops such values are two halves
## of one rule (TST_AttributeFormGui), so the move dialog is compiled in beside it.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_MovePartDialog.cpp")
## ...and the §7a tick list of the search words the part inherits from its category.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_KeywordCheckList.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_DimensionLineEdit.cpp")
## The part editor and the main window preview both embed the KiCad symbol/footprint painter.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_KicadPreviewWidget.cpp")
## ...whose shape loop is shared with the footprint-variant overlay, so it lives beside it.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_KicadShapePainter.cpp")
## §5a's "use a footprint you already have" prompt, which New Part and the part editor both
## open after an ECAD download — and the overlay it confirms the suggestion with.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_FootprintSuggestionDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_KicadVariantView.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_TypeIconPainter.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/widgets/PartManager_AttachmentIconPainter.cpp")

## The §2b category transfer, all three screens (TST_CategoryTransferGui, TST_CategoryReconcileGui).
## The reconcile dialog drives the picker, the move dialog and the part editor, which are all
## already above it in this list -- it is the one that ties them together.
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_CategoryExportDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_CategoryImportDialog.cpp")
list(APPEND ADDITONAL_SOURCES "${APP_SRC_DIR}/ui/PartManager_CategoryReconcileDialog.cpp")

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