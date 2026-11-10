cmake_minimum_required(VERSION 3.20)
project(NeuraLibclang LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(neura_libclang_top_level ON)
else()
    set(neura_libclang_top_level OFF)
endif()
option(NEURA_LIBCLANG_TESTS "Build and register the libclang loader self-test"
    ${neura_libclang_top_level})

# The loader opens libclang at run time, so this library links only the
# platform's dynamic loader. Consumers get the pinned clang-c headers and
# libclang_loader.h through the include directories.
add_library(neura_libclang STATIC src/libclang_loader.c)
add_library(neura::libclang ALIAS neura_libclang)
target_include_directories(neura_libclang PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(neura_libclang PRIVATE c_std_99)
target_link_libraries(neura_libclang PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(neura_libclang PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(neura_libclang PRIVATE /W4)
else()
    target_compile_options(neura_libclang PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(NEURA_LIBCLANG_TESTS)
    if(neura_libclang_top_level)
        enable_testing()
    endif()
    add_executable(neura_libclang_self_test tests/self_test.c)
    target_link_libraries(neura_libclang_self_test PRIVATE neura_libclang)
    if(MSVC)
        target_compile_options(neura_libclang_self_test PRIVATE /W4)
    else()
        target_compile_options(neura_libclang_self_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # Exit code 77 means no libclang is installed; CTest reports a skip.
    add_test(NAME neura-libclang-load COMMAND neura_libclang_self_test)
    set_tests_properties(neura-libclang-load PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME neura-libclang-missing COMMAND neura_libclang_self_test missing)
    add_test(NAME neura-libclang-env COMMAND neura_libclang_self_test env)
    set_tests_properties(neura-libclang-env PROPERTIES
        ENVIRONMENT "NEURA_LIBCLANG=/nonexistent/neura/libclang.so")
endif()
