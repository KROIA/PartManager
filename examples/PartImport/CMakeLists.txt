##
## Console importer: reads a `;`-separated Stockcount;MouserNR;Link CSV, looks each part up on
## the live Mouser Search API and writes it into a .pmdb database. Console on purpose (no
## WIN32_EXECUTABLE) so the run is scriptable and its output readable.
##

## USER_SECTION_START 1

## USER_SECTION_END

## USER_SECTION_START 2

## USER_SECTION_END

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
