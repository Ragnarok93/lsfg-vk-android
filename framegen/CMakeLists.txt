cmake_minimum_required(VERSION 3.10)

set(CMAKE_C_VISIBILITY_PRESET "hidden")
set(CMAKE_CXX_VISIBILITY_PRESET "hidden")

project(lsfg-vk-framegen
    DESCRIPTION "Lossless Scaling Frame Generation Backend"
    LANGUAGES CXX)

file(GLOB SOURCES
    "src/common/*.cpp"
    "src/config/*.cpp"
    "src/core/*.cpp"
    "src/pool/*.cpp"
    "src/*.cpp"
    "v3.1_src/core/*.cpp"
    "v3.1_src/pool/*.cpp"
    "v3.1_src/shaders/*.cpp"
    "v3.1_src/utils/*.cpp"
    "v3.1_src/*.cpp"
    "v3.1p_src/core/*.cpp"
    "v3.1p_src/pool/*.cpp"
    "v3.1p_src/shaders/*.cpp"
    "v3.1p_src/utils/*.cpp"
    "v3.1p_src/*.cpp"
)

add_library(lsfg-vk-framegen STATIC ${SOURCES})

set_target_properties(lsfg-vk-framegen PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
target_include_directories(lsfg-vk-framegen
    PUBLIC include
    PUBLIC public
    PRIVATE v3.1_include
    PRIVATE v3.1p_include)
target_link_libraries(lsfg-vk-framegen
    PUBLIC volk)

if(ANDROID)
    target_link_libraries(lsfg-vk-framegen PUBLIC log)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(lsfg-capabilities-test
        tests/capabilities_test.cpp
        src/core/capabilities.cpp)
    set_target_properties(lsfg-capabilities-test PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON)
    target_include_directories(lsfg-capabilities-test PRIVATE include)
    target_link_libraries(lsfg-capabilities-test PRIVATE volk)
    add_test(NAME lsfg-capabilities COMMAND lsfg-capabilities-test)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_target_properties(lsfg-vk-framegen PROPERTIES
        EXPORT_COMPILE_COMMANDS ON)
endif()

if(LSFGVK_EXCESS_DEBUG)
    target_compile_options(lsfg-vk-framegen PRIVATE
        -Weverything
        -Wno-pre-c++20-compat-pedantic
        -Wno-pre-c++17-compat
        -Wno-c++98-compat-pedantic
        -Wno-c++98-compat
        -Wno-missing-designated-field-initializers
        -Wno-shadow
        -Wno-switch-enum
        -Wno-switch-default
        -Wno-padded
        -Wno-exit-time-destructors
        -Wno-global-constructors
        -Wno-cast-function-type-strict)

    set_target_properties(lsfg-vk-framegen PROPERTIES
        CXX_CLANG_TIDY clang-tidy)
endif()
