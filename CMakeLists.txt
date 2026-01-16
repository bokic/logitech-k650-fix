cmake_minimum_required(VERSION 3.15)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UDEV REQUIRED libudev)

project(logitech-k650-fix VERSION 0.9.1 LANGUAGES C)

add_executable(logitech-k650-fix
    main.c
)

target_include_directories(logitech-k650-fix PRIVATE ${UDEV_INCLUDE_DIRS})
target_link_libraries(logitech-k650-fix PRIVATE ${UDEV_LIBRARIES})

install(TARGETS
    logitech-k650-fix
)
