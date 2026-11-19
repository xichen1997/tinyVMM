cmake_minimum_required(VERSION 3.16)
project(tinykvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
find_program(LD NAMES ld REQUIRED)

set(GUEST_BIN ${CMAKE_CURRENT_BINARY_DIR}/guest.bin)
set(GUEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/guest)

# project headers are included as "<module>/<file>.hpp", rooted at src/
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

# --- guest: freestanding C → ELF → flat binary (linked at 0x10000) ---
add_custom_command(
    OUTPUT ${GUEST_BIN}
    COMMAND ${CMAKE_C_COMPILER}
        -m64 -O2 -ffreestanding -nostdlib -mno-red-zone -fno-pic
        -fno-stack-protector
        -c ${GUEST_DIR}/guest.c
        -o ${CMAKE_CURRENT_BINARY_DIR}/guest.o
    COMMAND ${LD}
        -T ${GUEST_DIR}/guest.ld
        -o ${CMAKE_CURRENT_BINARY_DIR}/guest.elf
        ${CMAKE_CURRENT_BINARY_DIR}/guest.o
    COMMAND ${OBJCOPY}
        -O binary
        ${CMAKE_CURRENT_BINARY_DIR}/guest.elf
        ${GUEST_BIN}
    DEPENDS
        ${GUEST_DIR}/guest.c
        ${GUEST_DIR}/guest.ld
    COMMENT "Building guest.bin"
    VERBATIM
)

add_custom_target(guest ALL DEPENDS ${GUEST_BIN})

# --- host VMM (loads ./guest.bin relative to cwd) ---
add_executable(raii src/vmm/raii.cpp)
target_include_directories(raii PRIVATE ${SRC_DIR})
target_compile_options(raii PRIVATE -Wall -Wextra -g)
add_dependencies(raii guest)

find_package(Threads REQUIRED)
target_link_libraries(raii PRIVATE Threads::Threads)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/gdt_test.cpp)
    add_executable(gdt_test tests/gdt_test.cpp)
    target_include_directories(gdt_test PRIVATE ${SRC_DIR})
    target_compile_options(gdt_test PRIVATE -Wall -Wextra -g)
endif()

# guest hc_exit(42) makes the host return 42; treat that as success for convenience targets
add_custom_target(run
    COMMAND $<TARGET_FILE:raii> || test $$? -eq 42
    DEPENDS raii guest
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)

add_custom_target(run-trace
    COMMAND $<TARGET_FILE:raii> --trace-exits || test $$? -eq 42
    DEPENDS raii guest
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)

# tests/test.cpp
add_executable(test_cpu tests/test.cpp)
target_compile_options(test_cpu PRIVATE -Wall -Wextra -g)

add_custom_target(run-test-cpu
    COMMAND $<TARGET_FILE:test_cpu>
    DEPENDS test_cpu
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)


# tests/bzimage_test.cpp

add_executable(bzimage_test tests/bzimage_test.cpp)
target_include_directories(bzimage_test PRIVATE ${SRC_DIR})
target_compile_options(bzimage_test PRIVATE -Wall -Wextra -g)

add_custom_target(bzimage_test_run
    COMMAND $<TARGET_FILE:bzimage_test>
    DEPENDS bzimage_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)


# tests/boot_params.cpp
add_executable(boot_params tests/boot_params.cpp)
target_include_directories(boot_params PRIVATE ${SRC_DIR})
target_compile_options(boot_params PRIVATE -Wall -Wextra -g)

add_custom_target(boot_params_test
    COMMAND $<TARGET_FILE:boot_params>
    DEPENDS boot_params
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)