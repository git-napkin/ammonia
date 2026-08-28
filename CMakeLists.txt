cmake_minimum_required(VERSION 3.16)
project(pluginplayground C CXX OBJC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_ARM64E "Build for arm64e architecture" ON)
if(BUILD_ARM64E)
  set(CMAKE_OSX_ARCHITECTURES "arm64e")
else()
  set(CMAKE_OSX_ARCHITECTURES "arm64")
endif()

set(CODESIGN_IDENTITY "-" CACHE STRING "Codesign identity (default: ad-hoc)")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/opt/pluginplayground" CACHE PATH "Installation prefix" FORCE)
endif()

# ── Sanitisers (debug builds) ──────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(CMAKE_C_COMPILER_ID MATCHES "AppleClang|Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ── clang-tidy (if available) ──────────────────────────────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  find_program(CLANG_TIDY clang-tidy)
  if(CLANG_TIDY)
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY}")
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY}")
    message(STATUS "clang-tidy enabled: ${CLANG_TIDY}")
  endif()
endif()

function(syphon_codesign target)
  add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign -f -s "${CODESIGN_IDENTITY}"
                --entitlements "${CMAKE_SOURCE_DIR}/Master.entitlements"
                $<TARGET_FILE:${target}>
        COMMENT "codesigning ${target} (identity: ${CODESIGN_IDENTITY})"
    )
endfunction()

# ── Shared syphon object library (compiled once for fangs_hook targets) ──
add_library(syphon_obj OBJECT
    syphon/envbuf.c
    syphon/tweak_utils.c
    syphon/options_loader.c
)
set_target_properties(syphon_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(SYPHON_INCLUDE_PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/syphon>
    $<INSTALL_INTERFACE:include/syphon>
)

target_include_directories(syphon_obj PRIVATE ${SYPHON_INCLUDE_PRIVATE})

# ── libfangs_hook: Frida-Gum posix_spawn hook (injected into launchd) ────
add_library(fangs_hook SHARED
    syphon/fangs_hook.c
    syphon/exe.c
    syphon/pac_utils.c
    syphon/bundle_copy.c
    $<TARGET_OBJECTS:syphon_obj>
)
target_include_directories(fangs_hook PRIVATE ${SYPHON_INCLUDE_PRIVATE})
target_link_libraries(fangs_hook PRIVATE
    "-framework CoreFoundation"
    "-framework Security"
)

# ── libplayground_opener: per-process tweak loader ────────────────────────
add_library(playground_opener SHARED
    syphon/playground_opener.c
    $<TARGET_OBJECTS:syphon_obj>
)
target_include_directories(playground_opener PRIVATE ${SYPHON_INCLUDE_PRIVATE})
target_link_libraries(playground_opener PRIVATE
    "-framework CoreFoundation"
)

# ── Privilege grant tool (launchd shellcode injector) ─────────────────────
add_executable(grant grant/main.m)
target_include_directories(grant PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/grant")
target_link_libraries(grant PRIVATE
    "-framework Foundation"
)
target_compile_options(grant PRIVATE -fobjc-arc)
syphon_codesign(grant)

# ── Unit tests ─────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  enable_testing()

  add_executable(test_envbuf test/test_envbuf.c syphon/envbuf.c)
  add_test(NAME envbuf COMMAND test_envbuf)

  add_executable(test_tweak_utils test/test_tweak_utils.c
      $<TARGET_OBJECTS:syphon_obj>
  )
  target_include_directories(test_tweak_utils PRIVATE ${SYPHON_INCLUDE_PRIVATE})
  target_link_libraries(test_tweak_utils PRIVATE "-framework CoreFoundation")
  add_test(NAME tweak_utils COMMAND test_tweak_utils)
endif()

# ── Configurator ──────────────────────────────────────────────────────────

option(BUILD_CONFIGURATOR "Build the Configurator App" ON)

if(BUILD_CONFIGURATOR)
    include(FetchContent)
    FetchContent_Declare(
        Slint
        GIT_REPOSITORY https://github.com/slint-ui/slint.git
        GIT_TAG        v1.16.1
    )
    set(SLINT_STYLE "native" CACHE STRING "Slint widget style")
    set(SLINT_FEATURE_STYLE_NATIVE ON CACHE BOOL "Enable native widget style")
    set(SLINT_FETCH_CONTENT ON CACHE BOOL "" FORCE)

    file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/.build-cargo")
    if(EXISTS "${CMAKE_BINARY_DIR}/cargo")
        if(NOT IS_SYMLINK "${CMAKE_BINARY_DIR}/cargo")
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/cargo")
            file(CREATE_LINK "${CMAKE_SOURCE_DIR}/.build-cargo" "${CMAKE_BINARY_DIR}/cargo" SYMBOLIC)
        endif()
    else()
        file(CREATE_LINK "${CMAKE_SOURCE_DIR}/.build-cargo" "${CMAKE_BINARY_DIR}/cargo" SYMBOLIC)
    endif()
    FetchContent_MakeAvailable(Slint)

    add_executable(configurator
        configurator/main.cpp
        configurator/controller.cpp
        configurator/dmanager.cpp
        configurator/options.cpp
        configurator/tweaks.cpp
        configurator/configurator.slint
    )
    slint_target_sources(configurator configurator/configurator.slint)
    set_property(TARGET configurator PROPERTY SLINT_EMBED_RESOURCES embed-files)
    set_target_properties(configurator PROPERTIES
        CXX_STANDARD 17
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pluginplayground.configurator"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_BUNDLE_NAME "Plugin Playground Configurator"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/configurator/Info.plist.in"
        BUILD_RPATH "@executable_path/."
        INSTALL_RPATH "@executable_path/."
    )
    target_link_libraries(configurator PRIVATE Slint::Slint "-framework CoreFoundation")

    add_custom_command(TARGET configurator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/_deps/slint-build/api/cpp/libslint_cpp.dylib"
            "$<TARGET_FILE_DIR:configurator>"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_SOURCE_DIR}/configurator/gearbox.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:configurator>/Resources/gearbox.icns"
        COMMENT "Copying libslint_cpp.dylib + gearbox.icns into configurator.app bundle"
    )

    install(TARGETS configurator
        RUNTIME DESTINATION bin
        BUNDLE DESTINATION Applications
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────
install(TARGETS grant RUNTIME DESTINATION bin)
install(TARGETS fangs_hook playground_opener LIBRARY DESTINATION lib)
