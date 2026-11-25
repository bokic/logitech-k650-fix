find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Pure C unit tests for the event remapping state machine (remap.c).
add_executable(remap_unit_tests
    test_remap.c
    ${CMAKE_SOURCE_DIR}/remap.c
)
set_target_properties(remap_unit_tests PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)
target_include_directories(remap_unit_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(remap_unit_tests PRIVATE -Wall -Wextra)

add_test(NAME remap_unit_tests COMMAND remap_unit_tests)

# LD_PRELOAD shim that fakes the evdev device and /dev/uinput (see shim.c).
add_library(k650_test_shim MODULE shim.c)
set_target_properties(k650_test_shim PROPERTIES
    PREFIX ""
    C_STANDARD 11
    C_EXTENSIONS ON
)
target_compile_options(k650_test_shim PRIVATE -Wall -Wextra)
target_link_libraries(k650_test_shim PRIVATE ${CMAKE_DL_LIBS})

add_test(NAME event_state_machine
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
        --binary $<TARGET_FILE:logitech-k650-fix>
        --shim $<TARGET_FILE:k650_test_shim>
        --group state -v
)

add_test(NAME process_behaviour
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.py
        --binary $<TARGET_FILE:logitech-k650-fix>
        --shim $<TARGET_FILE:k650_test_shim>
        --group process -v
)

set_tests_properties(remap_unit_tests event_state_machine process_behaviour PROPERTIES TIMEOUT 120)
