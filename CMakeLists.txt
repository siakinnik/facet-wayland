cmake_minimum_required(VERSION 3.18)
project(facet-wayland VERSION 0.1.4 LANGUAGES C CXX)
set(WAYLAND_VERSION_SUFFIX "alpha")  # pre-release tag; empty for final releases; keep manifest.json in sync

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(WAYLAND_STATIC "Link fully static (portable release binaries)" OFF)

if(WAYLAND_VERSION_SUFFIX)
    set(WAYLAND_VERSION "${PROJECT_VERSION}-${WAYLAND_VERSION_SUFFIX}")
else()
    set(WAYLAND_VERSION "${PROJECT_VERSION}")
endif()

# The plugin SDK lives in facet-core: a local checkout next to this project
# (or FACET_CORE_DIR) is used when present, otherwise it is downloaded.
# Capability endpoints and lent surfaces need facet-core 0.6.
set(FACET_CORE_DIR "" CACHE PATH "facet-core checkout providing the SDK (default: ../facet-core, else download)")
set(FACET_CORE_REF "v0.7.0-alpha" CACHE STRING "facet-core git ref to download the SDK from")
if(NOT FACET_CORE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core/sdk/CMakeLists.txt")
    set(FACET_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core")
endif()
if(FACET_CORE_DIR)
    add_subdirectory(${FACET_CORE_DIR}/sdk ${CMAKE_BINARY_DIR}/facet_sdk)
else()
    include(FetchContent)
    FetchContent_Declare(facet_core
        GIT_REPOSITORY https://github.com/siakinnik/facet-core.git
        GIT_TAG ${FACET_CORE_REF}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk)
    FetchContent_MakeAvailable(facet_core)
endif()

# wlroots and its libraries belong to the plugin: the versions pinned in DEPS
# are built once into third_party/deps and linked statically, so nothing has
# to be installed on the device.
set(WAYLAND_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deps" CACHE PATH "Install prefix of the bundled libraries")
execute_process(COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-deps.sh" "${WAYLAND_DEPS_DIR}"
                RESULT_VARIABLE deps_result)
if(NOT deps_result EQUAL 0)
    message(FATAL_ERROR "Building the bundled libraries failed (scripts/build-deps.sh)")
endif()
set(ENV{PKG_CONFIG_PATH} "${WAYLAND_DEPS_DIR}/lib/pkgconfig:${WAYLAND_DEPS_DIR}/share/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(WLR REQUIRED wlroots-0.18 wayland-server xkbcommon pixman-1)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
find_program(WAYLAND_SCANNER wayland-scanner PATHS "${WAYLAND_DEPS_DIR}/bin" NO_DEFAULT_PATH REQUIRED)

set(GEN ${CMAKE_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${GEN})
add_custom_command(OUTPUT ${GEN}/xdg-shell-protocol.h
    COMMAND ${WAYLAND_SCANNER} server-header ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml
            ${GEN}/xdg-shell-protocol.h
    DEPENDS ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)

add_library(wayland_policy STATIC src/scopes.c src/keymap.c)
target_include_directories(wayland_policy PUBLIC src)
target_compile_options(wayland_policy PRIVATE -Wall -Wextra)

add_library(wayland_compositor STATIC src/compositor.c ${GEN}/xdg-shell-protocol.h)
target_include_directories(wayland_compositor PUBLIC src PRIVATE ${GEN} ${WLR_STATIC_INCLUDE_DIRS})
target_compile_definitions(wayland_compositor PRIVATE WLR_USE_UNSTABLE)
target_compile_options(wayland_compositor PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(wayland_compositor PUBLIC wayland_policy ${WLR_STATIC_LDFLAGS})

add_executable(wayland src/main.cpp src/i18n/ru.cpp)
target_include_directories(wayland PRIVATE src)
target_link_libraries(wayland PRIVATE wayland_compositor facet_sdk)
target_compile_options(wayland PRIVATE -Wall -Wextra)
target_compile_definitions(wayland PRIVATE WAYLAND_VERSION="${WAYLAND_VERSION}")
if(WAYLAND_STATIC)
    target_link_options(wayland PRIVATE -static -s)
endif()

# The build dir doubles as a plugin dir: FACET_PLUGIN_PATH=<build>
set(PLUGIN_OUT ${CMAKE_BINARY_DIR}/wayland.plugin)
set_target_properties(wayland PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PLUGIN_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${PLUGIN_OUT}/manifest.json)

# Example client module (a template for app modules): runs the command in
# examples/desktop-app/command inside its container and shows its windows.
option(WAYLAND_EXAMPLES "Build the example client module" ON)
if(WAYLAND_EXAMPLES)
    set(EXAMPLE_OUT ${CMAKE_BINARY_DIR}/examples/desktop-app.plugin)
    add_executable(desktop-app examples/desktop-app/main.cpp examples/desktop-app/i18n/ru.cpp)
    target_include_directories(desktop-app PRIVATE examples/desktop-app)
    target_link_libraries(desktop-app PRIVATE facet_sdk)
    target_compile_options(desktop-app PRIVATE -Wall -Wextra)
    set_target_properties(desktop-app PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${EXAMPLE_OUT})
    facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/examples/desktop-app/manifest.json ${EXAMPLE_OUT}/manifest.json)
    configure_file(examples/desktop-app/command ${EXAMPLE_OUT}/command COPYONLY)
endif()

enable_testing()
add_executable(policy_test tests/policy_test.cpp)
target_link_libraries(policy_test PRIVATE wayland_policy ${WLR_STATIC_LDFLAGS})
target_include_directories(policy_test PRIVATE ${WLR_STATIC_INCLUDE_DIRS})
add_test(NAME policy COMMAND policy_test)

install(TARGETS wayland RUNTIME DESTINATION lib/facet/plugins/wayland)
install(FILES ${PLUGIN_OUT}/manifest.json DESTINATION lib/facet/plugins/wayland)
