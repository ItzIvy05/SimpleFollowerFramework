cmake_minimum_required(VERSION 3.21)
project(SimpleFollowerFramework VERSION 1.0.0 LANGUAGES CXX)

# Output folder selection (same as your current file)
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()

if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

# ---- Force CommonLibSSE-NG from your environment variable ----
# You MUST set COMMONLIB_SSE_FOLDER to the CommonLibSSE-NG *install prefix*
# (the folder that contains lib/cmake/CommonLibSSE/CommonLibSSEConfig.cmake)
if(NOT DEFINED ENV{COMMONLIB_SSE_FOLDER})
    message(FATAL_ERROR "COMMONLIB_SSE_FOLDER env var not set. Set it to your CommonLibSSE-NG install prefix.")
endif()

set(COMMONLIB_PREFIX "$ENV{COMMONLIB_SSE_FOLDER}")

# Prefer config packages from this prefix first
list(PREPEND CMAKE_PREFIX_PATH "${COMMONLIB_PREFIX}")
find_package(CommonLibSSE CONFIG REQUIRED)

# Print what got found (this is the key sanity check)
message(STATUS "Using CommonLibSSE from: ${CommonLibSSE_DIR}")

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES plugin.cpp)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# Deploy the DLL into mod folder if OUTPUT_FOLDER set
if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")

    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()