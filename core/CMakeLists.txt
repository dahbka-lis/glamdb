add_library(glamdb_core STATIC api/canonical.cpp api/codec.cpp)
add_library(glamdb::core ALIAS glamdb_core)
target_link_libraries(glamdb_core PRIVATE glamdb_build_options)
target_include_directories(glamdb_core
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/public>"
           "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${PROJECT_SOURCE_DIR}")
