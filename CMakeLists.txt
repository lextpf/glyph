cmake_minimum_required(VERSION 3.19)

project(glyph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# /EHa supports SEH guards; /MT keeps the plugin runtime self-contained.
if(MSVC)
    add_compile_options(/MT /EHa /std:c++latest)
else()
    add_compile_options(/MT /EHa)
endif()
add_link_options(/NODEFAULTLIB:MSVCRT)

# clang-tidy uses the Ninja sidecar in build.bat; do not wrap the MSVC build.

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Xbyak is required by hook_function_prologue.
set(SKSE_SUPPORT_XBYAK ON CACHE BOOL "" FORCE)

# one Address Library-backed DLL supports SE and AE, including GOG; VR is unsupported.
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Enable Skyrim SE runtime support" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Enable Skyrim AE/GOG runtime support" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Enable Skyrim VR runtime support" FORCE)

include(FetchContent)
FetchContent_Declare(
    CommonLibSSE-NG
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES CommonLibSSE
)
FetchContent_MakeAvailable(CommonLibSSE-NG)

# CommonLibSSE-NG templates exhaust RAM with parallel cl.exe workers.
if(TARGET CommonLibSSE)
    target_compile_options(CommonLibSSE PRIVATE /MP1)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(xbyak CONFIG REQUIRED)
find_package(NanoSVG CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_path(RAPIDCSV_INCLUDE_DIRS "rapidcsv.h") # Added for CommonLibSSE-NG dependency

find_path(BOOST_UNORDERED_INCLUDE_DIRS "boost_unordered.hpp")
if(NOT BOOST_UNORDERED_INCLUDE_DIRS)
    find_path(BOOST_UNORDERED_INCLUDE_DIRS "boost_unordered.hpp" 
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
    )
endif()
if(BOOST_UNORDERED_INCLUDE_DIRS)
    message(STATUS "Found boost-unordered at: ${BOOST_UNORDERED_INCLUDE_DIRS}")
else()
    message(WARNING "boost-unordered not found. Make sure it's installed via vcpkg.")
endif()

find_path(SRELL_INCLUDE_DIRS "srell.hpp")
if(NOT SRELL_INCLUDE_DIRS)
    find_path(SRELL_INCLUDE_DIRS "srell.hpp" 
        PATHS
            "${CMAKE_CURRENT_SOURCE_DIR}/build/vcpkg_installed/x64-windows-static/include"
            "$ENV{VCPKG_ROOT}/installed/x64-windows-static/include"
    )
endif()
if(SRELL_INCLUDE_DIRS)
    message(STATUS "Found srell at: ${SRELL_INCLUDE_DIRS}")
else()
    message(WARNING "srell not found. Make sure it's installed via vcpkg.")
endif()

# ClibUtil ships with powerof3's CommonLibSSE, separately from CommonLibSSE-NG.
find_path(CLIBUTIL_INCLUDE_DIRS 
    "ClibUtil/simpleINI.hpp" 
    PATHS 
        "${CMAKE_CURRENT_SOURCE_DIR}/extern/commonlibsse/include"
        "$ENV{CommonLibSSEPath}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/commonlibsse-ng-src/include"
    NO_DEFAULT_PATH
)
if(NOT CLIBUTIL_INCLUDE_DIRS)
    find_path(CLIBUTIL_INCLUDE_DIRS "ClibUtil/simpleINI.hpp")
endif()
if(CLIBUTIL_INCLUDE_DIRS)
    message(STATUS "Found ClibUtil at: ${CLIBUTIL_INCLUDE_DIRS}")
else()
    message(WARNING "ClibUtil not found. You may need to set CommonLibSSEPath environment variable or add extern/commonlibsse submodule.")
endif()

add_library(glyph SHARED
    src/main.cpp
    src/PCH.hpp
    src/ConsoleCommands.hpp
    src/ConsoleCommands.cpp
    src/ConsoleParse.hpp
    src/ActorOverrides.hpp
    src/ActorOverrides.cpp
    src/Settings.cpp
    src/Settings.hpp
    src/SettingsBinding.hpp
    src/Renderer.cpp
    src/Renderer.hpp
    src/RendererInternal.hpp
    src/RendererSnapshot.cpp
    src/RendererLayout.cpp
    src/RendererEffects.cpp
    src/GameState.hpp
    src/GameState.cpp
    src/Hooks.cpp
    src/Hooks.hpp
    src/TextEffects.hpp
    src/TextEffectsInternal.hpp
    src/TextEffectsCore.cpp
    src/TextEffectsGradient.cpp
    src/TextEffectsAnimated.cpp
    src/TextEffectsComplex.cpp
    src/TextEffectsParticle.cpp
    src/Occlusion.hpp
    src/Occlusion.cpp
    src/DebugOverlay.hpp
    src/DebugOverlay.cpp
    src/RenderConstants.hpp
    src/RasterQuality.hpp
    src/RenderSampling.hpp
    src/RenderSampling.cpp
    src/NameFit.hpp
    src/TierEmblem.hpp
    src/ParticleTextures.hpp
    src/ParticleTextures.cpp
    src/BadgeTextures.hpp
    src/BadgeTextures.cpp
    src/DeckUtils.hpp
    src/DeckUtils.cpp
    src/DeckPng.hpp
    src/DeckPng.cpp
    src/Deck.hpp
    src/Deck.cpp
    src/ProjectManifest.hpp
    src/ProjectManifest.cpp
    src/TextPostProcess.hpp
    src/TextPostProcess.cpp
    src/SceneMeter.hpp
    src/SceneMeter.cpp
    src/DepthClip.hpp
    src/DepthClip.cpp
    src/GraffitoMath.hpp
    src/GraffitoShaderContract.hpp
    src/Graffito.hpp
    src/Graffito.cpp
    src/TrueHUDAPI.h
    src/HudCompat.hpp
    src/HudCompat.cpp
    src/Utf8Utils.hpp
    src/Version.hpp
)

target_include_directories(glyph PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${RAPIDCSV_INCLUDE_DIRS}
)

if(CLIBUTIL_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${CLIBUTIL_INCLUDE_DIRS})
endif()

if(BOOST_UNORDERED_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${BOOST_UNORDERED_INCLUDE_DIRS})
endif()

if(SRELL_INCLUDE_DIRS)
    target_include_directories(glyph PRIVATE ${SRELL_INCLUDE_DIRS})
endif()

target_link_libraries(glyph PRIVATE
    CommonLibSSE
    imgui::imgui
    spdlog::spdlog
    xbyak::xbyak
    NanoSVG::nanosvg
    NanoSVG::nanosvgrast
    nlohmann_json::nlohmann_json
    ole32
    windowscodecs
)

option(BUILD_GLYPH_TESTS "Build glyph unit tests" ON)

if(BUILD_GLYPH_TESTS)
    find_package(GTest CONFIG REQUIRED)

    add_executable(glyph_test_utils tests/test_utils.cpp)
    target_include_directories(glyph_test_utils PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_utils PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_utils PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_utils PRIVATE /W4)
    endif()

    add_executable(glyph_test_settings tests/test_settings.cpp)
    target_compile_features(glyph_test_settings PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_settings PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_settings PRIVATE /W4)
    endif()

    add_executable(glyph_test_label_format tests/test_label_format.cpp)
    target_compile_features(glyph_test_label_format PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_label_format PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_label_format PRIVATE /W4)
    endif()

    add_executable(glyph_test_graffito tests/test_graffito.cpp)
    target_include_directories(glyph_test_graffito PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_graffito PRIVATE cxx_std_17)
    target_link_libraries(glyph_test_graffito PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_graffito PRIVATE /W4)
    endif()

    add_executable(glyph_test_deck tests/test_deck.cpp src/DeckUtils.cpp src/DeckPng.cpp)
    target_include_directories(glyph_test_deck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_deck PRIVATE cxx_std_23)
    target_link_libraries(glyph_test_deck PRIVATE
        GTest::gtest
        GTest::gtest_main
        ole32
        windowscodecs
    )
    if(MSVC)
        target_compile_options(glyph_test_deck PRIVATE /W4)
    endif()

    add_executable(glyph_test_console tests/test_console.cpp src/ActorOverrides.cpp)
    target_include_directories(glyph_test_console PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(glyph_test_console PRIVATE cxx_std_23)
    target_link_libraries(glyph_test_console PRIVATE GTest::gtest GTest::gtest_main)
    if(MSVC)
        target_compile_options(glyph_test_console PRIVATE /W4)
    endif()

    include(GoogleTest)
    enable_testing()
    gtest_discover_tests(glyph_test_utils)
    gtest_discover_tests(glyph_test_settings)
    gtest_discover_tests(glyph_test_label_format)
    gtest_discover_tests(glyph_test_graffito)
    gtest_discover_tests(glyph_test_deck)
    gtest_discover_tests(glyph_test_console)
endif()
