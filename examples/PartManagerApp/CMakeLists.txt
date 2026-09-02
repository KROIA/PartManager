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

## USER_SECTION_END