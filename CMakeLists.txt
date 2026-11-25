cmake_minimum_required(VERSION 3.15)

project(logitech-k650-fix VERSION 0.9.4 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

add_executable(logitech-k650-fix
    main.c
    remap.c
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

pkg_get_variable(_default_systemd_unit_dir systemd systemdsystemunitdir)
if(NOT _default_systemd_unit_dir)
    set(_default_systemd_unit_dir "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")
endif()
set(SYSTEMD_UNIT_DIR "${_default_systemd_unit_dir}" CACHE PATH "Directory for systemd unit files")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/logitech-k650-fix.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/logitech-k650-fix.service"
    @ONLY
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/logitech-k650-fix.service"
    DESTINATION "${SYSTEMD_UNIT_DIR}"
)

option(BUILD_TESTING "Build the test suite" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
