add_executable(glamdb_cli main.cpp)
set_target_properties(glamdb_cli PROPERTIES OUTPUT_NAME glamdb-cli)
target_link_libraries(glamdb_cli PRIVATE glamdb_build_options glamdb_transport CLI11::CLI11)
install(TARGETS glamdb_cli RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
