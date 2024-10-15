cmake_minimum_required(VERSION 3.15)

project(logitech-k650-fix VERSION 0.9.1 LANGUAGES C)

add_executable(logitech-k650-fix
    main.c
)

install(TARGETS
    logitech-k650-fix
)
