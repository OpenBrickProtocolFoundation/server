add_executable(main
        main.cpp
        tetrion.cpp
        tetrion.hpp
)

target_link_libraries(main PRIVATE project_options)
target_link_system_libraries(main PRIVATE spdlog::spdlog c2k_sockets simulator_static)
