cmake_minimum_required(VERSION 3.16)
project(ammonia C CXX OBJC ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Ammonia.app stays arm64 (SwiftUI GUI). Core inject binaries use arm64e when BUILD_ARM64E=ON.
# Default CMAKE_OSX_ARCHITECTURES is arm64 so the app links; hook binaries override via ammonia_arm64e().
option(BUILD_ARM64E "Build infect/opener/ammonia as arm64e" ON)
if(NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64")
endif()
function(ammonia_arm64e target)
  if(BUILD_ARM64E)
    set_target_properties(${target} PROPERTIES OSX_ARCHITECTURES arm64e)
  endif()
endfunction()

set(CODESIGN_IDENTITY "-" CACHE STRING "Codesign identity (default: ad-hoc)")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/private/var/ammonia/core" CACHE PATH "Installation prefix" FORCE)
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
  # Ad-hoc + Master.entitlements is rejected by AMFI (Code=-424: "adhoc signed
  # but contains restricted entitlements"). SIP `--without debug` is the
  # privilege path for task_for_pid. Named identities can still embed the blob.
  if(CODESIGN_IDENTITY STREQUAL "-")
    add_custom_command(TARGET ${target} POST_BUILD
          COMMAND codesign -f -s "${CODESIGN_IDENTITY}"
                  $<TARGET_FILE:${target}>
          COMMENT "codesigning ${target} ad-hoc without restricted entitlements"
      )
  else()
    add_custom_command(TARGET ${target} POST_BUILD
          COMMAND codesign -f -s "${CODESIGN_IDENTITY}"
                  --entitlements "${CMAKE_SOURCE_DIR}/Master.entitlements"
                  $<TARGET_FILE:${target}>
          COMMENT "codesigning ${target} with entitlements (identity: ${CODESIGN_IDENTITY})"
      )
  endif()
endfunction()

function(syphon_codesign_dylib target)
  add_custom_command(TARGET ${target} POST_BUILD
        COMMAND codesign -f -s "${CODESIGN_IDENTITY}"
                $<TARGET_FILE:${target}>
        COMMENT "codesigning ${target} ad-hoc (identity: ${CODESIGN_IDENTITY})"
    )
endfunction()

# ── Shared syphon headers ─────────────────────────────────────────────────
set(SYPHON_INCLUDE_PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/syphon>
    $<INSTALL_INTERFACE:include/syphon>
)

# ── libinject: Ammonia posix_spawn hook (injected into launchd) ───────────
# OUTPUT_NAME inject + the usual lib prefix → libinject.dylib (not liblib…).
add_library(libinfect SHARED
    syphon/libinfect.m
    syphon/envbuf.c
    syphon/macho_sea.c
)
set_target_properties(libinfect PROPERTIES OUTPUT_NAME inject)
ammonia_arm64e(libinfect)
# Infect must use Ammonia's Gum ABI (replacement_data + original, not the
# 17.9.11 options-last signature). Prefer the sibling vendor copy that
# already survived Darwin 27 in PID 1; fall back to setup_frida.sh artifacts.
set(_infect_gum_inc "${CMAKE_SOURCE_DIR}/../legacy/ammonia/libinfect")
set(_infect_gum_a "${CMAKE_SOURCE_DIR}/../legacy/ammonia/libfrida-gum-arm64e-arm64.a")
if(NOT EXISTS "${_infect_gum_inc}/frida-gum.h")
    set(_infect_gum_inc "${CMAKE_SOURCE_DIR}/syphon")
endif()
if(NOT EXISTS "${_infect_gum_inc}/frida-gum.h")
    set(_infect_gum_inc "${CMAKE_SOURCE_DIR}/include")
endif()
if(NOT EXISTS "${_infect_gum_a}")
    set(_infect_gum_a "${CMAKE_SOURCE_DIR}/libfrida-gum-arm64e-arm64.a")
endif()
if(NOT EXISTS "${_infect_gum_inc}/frida-gum.h")
    message(FATAL_ERROR "frida-gum.h missing for libinfect; run sh ./setup_frida.sh")
endif()
if(NOT EXISTS "${_infect_gum_a}")
    message(FATAL_ERROR "libfrida-gum-arm64e-arm64.a missing; run sh ./setup_frida.sh")
endif()
target_include_directories(libinfect PRIVATE
    "${_infect_gum_inc}"
    ${SYPHON_INCLUDE_PRIVATE}
    "${CMAKE_SOURCE_DIR}/syphon"
)
target_link_libraries(libinfect PRIVATE
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-lresolv"
    "${_infect_gum_a}"
)
target_compile_options(libinfect PRIVATE -fobjc-arc)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(libinfect PRIVATE -fno-sanitize=address)
    target_link_options(libinfect PRIVATE -fno-sanitize=address)
endif()
# Linker-signed ad-hoc like stock Ammonia. Do not codesign -s - (that panicked
# playground copies of this hook on Darwin 27).

# ── libopener: per-process tweak loader (Playground opener, Ammonia path) ─
add_library(opener SHARED
    syphon/playground_opener.c
    syphon/envbuf.c
    syphon/tweak_utils.c
    syphon/options_loader.c
)
ammonia_arm64e(opener)
target_include_directories(opener PRIVATE ${SYPHON_INCLUDE_PRIVATE})
target_link_libraries(opener PRIVATE
    "-framework CoreFoundation"
)
syphon_codesign_dylib(opener)

# ── ammonia: inject libinject into launchd; --scan late-attaches opener ────
add_executable(ammonia
    grant/main.m
    grant/shellcode.S
    syphon/tweak_utils.c
    syphon/options_loader.c
)
target_include_directories(ammonia PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/grant"
    ${SYPHON_INCLUDE_PRIVATE}
)
target_link_libraries(ammonia PRIVATE
    "-framework Foundation"
    "-framework CoreFoundation"
)
target_compile_options(ammonia PRIVATE -fobjc-arc)
ammonia_arm64e(ammonia)
syphon_codesign(ammonia)

# ── Unit tests ─────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
  enable_testing()

  add_executable(test_envbuf test/test_envbuf.c syphon/envbuf.c)
  add_test(NAME envbuf COMMAND test_envbuf)

  add_executable(test_tweak_utils test/test_tweak_utils.c
      syphon/envbuf.c
      syphon/tweak_utils.c
      syphon/options_loader.c
  )
  target_include_directories(test_tweak_utils PRIVATE ${SYPHON_INCLUDE_PRIVATE})
  target_link_libraries(test_tweak_utils PRIVATE "-framework CoreFoundation")
  add_test(NAME tweak_utils COMMAND test_tweak_utils)

  add_executable(test_macho_sea test/test_macho_sea.c syphon/macho_sea.c)
  target_include_directories(test_macho_sea PRIVATE ${SYPHON_INCLUDE_PRIVATE})
  add_test(NAME macho_sea COMMAND test_macho_sea)
endif()

# ── Configurator ──────────────────────────────────────────────────────────

option(BUILD_CONFIGURATOR "Build the Configurator App" ON)

if(BUILD_CONFIGURATOR)
    set(GUI_PKG "${CMAKE_SOURCE_DIR}/gui")
    set(GUI_BUILD "${CMAKE_BINARY_DIR}/gui-spm")
    set(GUI_BIN "${GUI_BUILD}/release/Ammonia")
    set(GUI_APP "${CMAKE_BINARY_DIR}/configurator.app")

    file(GLOB_RECURSE GUI_SOURCES CONFIGURE_DEPENDS
        "${GUI_PKG}/Sources/*.swift"
        "${GUI_PKG}/Package.swift"
    )

    add_custom_command(
        OUTPUT "${GUI_BIN}"
        COMMAND ${CMAKE_COMMAND} -E env "SWIFT_MODULE_CACHE_PATH=${GUI_BUILD}/module-cache"
            swift build -c release --package-path "${GUI_PKG}" --build-path "${GUI_BUILD}"
        DEPENDS ${GUI_SOURCES}
        COMMENT "Building Ammonia SwiftUI app (release)"
        VERBATIM
    )

    add_custom_command(
        OUTPUT "${GUI_APP}/Contents/MacOS/Ammonia"
        COMMAND sh "${GUI_PKG}/bundle_app.sh" "${GUI_BUILD}" "${GUI_APP}" "${CMAKE_SOURCE_DIR}"
        DEPENDS "${GUI_BIN}" "${GUI_PKG}/bundle_app.sh" "${GUI_PKG}/Info.plist"
            "${GUI_PKG}/Resources/gearbox.icns" "${GUI_PKG}/Resources/drill.png"
        COMMENT "Bundling Ammonia.app"
        VERBATIM
    )

    add_custom_target(configurator ALL DEPENDS "${GUI_APP}/Contents/MacOS/Ammonia")

    install(DIRECTORY "${GUI_APP}"
        DESTINATION Applications
        USE_SOURCE_PERMISSIONS
    )
endif()

# ── Install ──────────────────────────────────────────────────────────────
install(TARGETS ammonia RUNTIME DESTINATION .)
install(TARGETS libinfect opener LIBRARY DESTINATION .)
install(FILES grant/com.ammonia.inject.plist DESTINATION share)
install(FILES grant/ammonia.blacklist DESTINATION share)
install(FILES grant/ammonia.blacklist DESTINATION .)
install(FILES include/playground_tweak.h DESTINATION include)
install(FILES fridagum.dylib DESTINATION . OPTIONAL)
