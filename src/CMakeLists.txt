add_executable(main main.cpp)

target_link_libraries(main PRIVATE project_options)
target_link_system_libraries(main PRIVATE c2k_sockets)
