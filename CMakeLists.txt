cmake_minimum_required(VERSION 3.5)
project(VitaRecovery)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "PS Vita Recovery")
set(VITA_TITLEID  "RECM00001")

# ── Version: single source of truth is the VERSION file (e.g. 1.0.8) ──────
# Run ./bump_version.sh before each new build to increment it.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" APP_VERSION)
string(STRIP "${APP_VERSION}" APP_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
if(NOT APP_VERSION MATCHES "^([0-9]+)\\.([0-9])\\.([0-9])$")
    message(FATAL_ERROR "VERSION file must look like 1.0.8 (got '${APP_VERSION}')")
endif()
# param.sfo only holds "MM.mm", so pack minor+patch into the second field:
#   1.0.8 -> 01.08   1.1.0 -> 01.10   1.9.9 -> 01.99   2.0.0 -> 02.00
math(EXPR _sfo_minor "${CMAKE_MATCH_2} * 10 + ${CMAKE_MATCH_3}")
set(_sfo_major "${CMAKE_MATCH_1}")
if(_sfo_major LESS 10)
    set(_sfo_major "0${_sfo_major}")
endif()
if(_sfo_minor LESS 10)
    set(_sfo_minor "0${_sfo_minor}")
endif()
set(VITA_VERSION "${_sfo_major}.${_sfo_minor}")
message(STATUS "Building PS Vita Recovery Menu v${APP_VERSION} (SFO version ${VITA_VERSION})")

# ── Step 1: Build boot_recovery.skprx (kernel plugin) ─────────────────────
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/boot_recovery.skprx"
    COMMAND ${CMAKE_COMMAND}
            -S "${CMAKE_SOURCE_DIR}/boot_plugin"
            -B "${CMAKE_BINARY_DIR}/boot_plugin_build"
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}/boot_plugin_build"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/boot_plugin_build/boot_recovery.skprx"
            "${CMAKE_BINARY_DIR}/boot_recovery.skprx"
    DEPENDS "${CMAKE_SOURCE_DIR}/boot_plugin/boot_recovery.c"
            "${CMAKE_SOURCE_DIR}/boot_plugin/CMakeLists.txt"
    COMMENT "Building boot_recovery.skprx"
)

# ── Step 2: Build boot_trigger.suprx (user plugin) ────────────────────────
add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/boot_trigger.suprx"
    COMMAND ${CMAKE_COMMAND}
            -S "${CMAKE_SOURCE_DIR}/boot_trigger_user"
            -B "${CMAKE_BINARY_DIR}/boot_trigger_build"
            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}/boot_trigger_build"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_BINARY_DIR}/boot_trigger_build/boot_trigger.suprx"
            "${CMAKE_BINARY_DIR}/boot_trigger.suprx"
    DEPENDS "${CMAKE_SOURCE_DIR}/boot_trigger_user/boot_trigger_user.c"
            "${CMAKE_SOURCE_DIR}/boot_trigger_user/CMakeLists.txt"
    COMMENT "Building boot_trigger.suprx"
)

# ── Step 3: Build the main Recovery Menu app ───────────────────────────────
add_executable(${PROJECT_NAME}
    source/main.c
    source/menu.c
    source/draw.c
    source/compat.c
    source/cpu.c
    source/plugins.c
    source/registry.c
    source/restore.c
    source/restore_screen.c
    source/official_recovery.c
    source/sd2vita.c
    source/sysinfo.c
    source/cheat_manager.c
    source/settings.c
    source/display.c
    source/debugScreen.c
    source/filemanager.c
    source/plugin_fix.c
    source/diag.c
    source/recovery_installer.c
    source/ftp_server.c
    source/update.c
    source/https.c
    source/fs_policy.c
)

add_custom_target(boot_plugins ALL
    DEPENDS "${CMAKE_BINARY_DIR}/boot_recovery.skprx"
            "${CMAKE_BINARY_DIR}/boot_trigger.suprx"
)
add_dependencies(${PROJECT_NAME} boot_plugins)

target_compile_definitions(${PROJECT_NAME} PRIVATE APP_VERSION="${APP_VERSION}")

target_include_directories(${PROJECT_NAME} PRIVATE
    source
    $ENV{VITASDK}/share/gcc-arm-vita-eabi/samples/common
)

# The SDK's mbedTLS was built with pthread mutexes. Link libpthread if the SDK
# has it; otherwise https.c supplies tiny single-thread replacements.
find_library(VITA_PTHREAD pthread HINTS "$ENV{VITASDK}/arm-vita-eabi/lib")
if(VITA_PTHREAD)
    message(STATUS "pthread library: ${VITA_PTHREAD}")
    set(PTHREAD_LINK pthread)
else()
    message(STATUS "pthread library not found: using built-in mutex stubs")
    target_compile_definitions(${PROJECT_NAME} PRIVATE VITA_PTHREAD_STUBS=1)
    set(PTHREAD_LINK "")
endif()

# mbedTLS client (source/https.c) needs a random-number source from the SDK.
# Find which stub library exports sceKernelGetRandomNumber and link it.
set(RNG_STUB "")
file(GLOB _stubs "$ENV{VITASDK}/arm-vita-eabi/lib/lib*_stub.a")
foreach(_s ${_stubs})
    execute_process(COMMAND "$ENV{VITASDK}/bin/arm-vita-eabi-nm" "${_s}"
                    OUTPUT_VARIABLE _nm ERROR_QUIET)
    if(_nm MATCHES "sceKernelGetRandomNumber")
        get_filename_component(_n "${_s}" NAME_WE)
        string(REGEX REPLACE "^lib" "" _n "${_n}")
        set(RNG_STUB "${_n}")
        break()
    endif()
endforeach()
if(RNG_STUB)
    message(STATUS "HTTPS random source: sceKernelGetRandomNumber (${RNG_STUB})")
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_RNG=1)
    target_link_libraries(${PROJECT_NAME} ${RNG_STUB})
else()
    message(WARNING "sceKernelGetRandomNumber not found in the SDK: HTTPS will use a WEAK fallback random source")
endif()

target_link_libraries(${PROJECT_NAME}
    SceCtrl_stub
    SceLibKernel_stub
    SceProcessmgr_stub
    ScePower_stub
    SceIofilemgr_stub
    SceSysmem_stub
    SceAppMgr_stub
    SceRegistryMgr_stub
    SceSysmodule_stub
    ScePromoterUtil_stub
    SceDisplay_stub
    SceVshBridge_stub
    SceNet_stub
    SceNetCtl_stub
    SceHttp_stub
    mbedtls
    mbedx509
    mbedcrypto
    ${PTHREAD_LINK}
    m
    c
)

# v1.1.9: built as UNSAFE homebrew (needs HENkaku "Enable Unsafe Homebrew").
# This lets the app read system drives (real firmware version, hardware info,
# ur0:shell). Writes stay limited by source/fs_policy.c.
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)

# ── Step 4: Package everything into the VPK ───────────────────────────────
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
    FILE sce_sys/icon0.png                             sce_sys/icon0.png
    FILE sce_sys/pic0.png                              sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg0.png             sce_sys/livearea/contents/bg0.png
    FILE sce_sys/livearea/contents/startup.png         sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml        sce_sys/livearea/contents/template.xml
    FILE "${CMAKE_BINARY_DIR}/boot_recovery.skprx"     boot_recovery.skprx
    FILE "${CMAKE_BINARY_DIR}/boot_trigger.suprx"      boot_trigger.suprx
)
