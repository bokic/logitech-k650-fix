cmake_minimum_required(VERSION 3.15)

project(logitech-k650-fix VERSION 0.9.3 LANGUAGES C)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

add_executable(logitech-k650-fix
    main.c
)

set_target_properties(logitech-k650-fix PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

target_compile_options(logitech-k650-fix PRIVATE -Wall -Wextra)
target_link_libraries(logitech-k650-fix PRIVATE PkgConfig::UDEV)

install(TARGETS logitech-k650-fix
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/logitech-k650-fix.service" _service_content)
string(REPLACE "/usr/bin/logitech-k650-fix" "${CMAKE_INSTALL_FULL_BINDIR}/logitech-k650-fix" _service_content "${_service_content}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/logitech-k650-fix.service" "${_service_content}")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/logitech-k650-fix.service"
    DESTINATION lib/systemd/system
)
