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

if(NOT LSFGVK_ADRENO_B10_MIPMAPS)
    list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/adreno_b10_mipmaps.cpp")
endif()

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
if(LSFGVK_ADRENO_B10_MIPMAPS)
    target_link_libraries(lsfg-vk-framegen PRIVATE dxbc)
endif()

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

    if(LSFGVK_ADRENO_B10_MIPMAPS)
        add_executable(lsfg-adreno-b10-mipmaps-test
            tests/adreno_b10_mipmaps_test.cpp
            src/adreno_b10_mipmaps.cpp)
        set_target_properties(lsfg-adreno-b10-mipmaps-test PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON)
        target_compile_definitions(lsfg-adreno-b10-mipmaps-test PRIVATE
            LSFGVK_B10_SELF_TEST=1)
        target_include_directories(lsfg-adreno-b10-mipmaps-test PRIVATE include)
        target_link_libraries(lsfg-adreno-b10-mipmaps-test PRIVATE dxbc)
        add_test(NAME lsfg-adreno-b10-mipmaps COMMAND lsfg-adreno-b10-mipmaps-test)
    endif()
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
