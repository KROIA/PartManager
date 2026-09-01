##
## Deliberately builds nothing. This folder is not a test target — it is a standalone, QtWidgets-only
## GUI driver that test targets compile into themselves (see ../ExampleTest/CMakeLists.txt), kept
## separate so it can move to the UnitTest library repository unchanged.
##
## unittests/CMakeLists.txt add_subdirectory()s every folder here, so this file has to exist.
##
