cmake_minimum_required(VERSION 3.16)
project(tinykvm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
find_program(LD NAMES ld REQUIRED)

set(GUEST_BIN ${CMAKE_CURRENT_BINARY_DIR}/guest.bin)

# --- guest: freestanding C → ELF → flat binary (linked at 0x10000) ---
add_custom_command(
    OUTPUT ${GUEST_BIN}
    COMMAND ${CMAKE_C_COMPILER}
        -m64 -O2 -ffreestanding -nostdlib -mno-red-zone -fno-pic
        -fno-stack-protector
        -c ${CMAKE_CURRENT_SOURCE_DIR}/guest.c
        -o ${CMAKE_CURRENT_BINARY_DIR}/guest.o
    COMMAND ${LD}
        -T ${CMAKE_CURRENT_SOURCE_DIR}/guest.ld
        -o ${CMAKE_CURRENT_BINARY_DIR}/guest.elf
        ${CMAKE_CURRENT_BINARY_DIR}/guest.o
    COMMAND ${OBJCOPY}
        -O binary
        ${CMAKE_CURRENT_BINARY_DIR}/guest.elf
        ${GUEST_BIN}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/guest.c
        ${CMAKE_CURRENT_SOURCE_DIR}/guest.ld
    COMMENT "Building guest.bin"
    VERBATIM
)

add_custom_target(guest ALL DEPENDS ${GUEST_BIN})

# --- host VMM (loads ./guest.bin relative to cwd) ---
add_executable(raii raii.cpp)
target_compile_options(raii PRIVATE -Wall -Wextra -g)
add_dependencies(raii guest)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gdt_test.cpp)
    add_executable(gdt_test gdt_test.cpp)
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

# test.cpp
add_executable(test_cpu test.cpp)
target_compile_options(test_cpu PRIVATE -Wall -Wextra -g)

add_custom_target(run-test-cpu
    COMMAND $<TARGET_FILE:test_cpu>
    DEPENDS test_cpu
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    USES_TERMINAL
)
