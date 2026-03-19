cmake_minimum_required(VERSION 3.5)
project(VitaRecovery)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "PS Vita Recovery")
set(VITA_TITLEID  "RECM00001")
set(VITA_VERSION  "01.00")

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
)

add_custom_target(boot_plugins ALL
    DEPENDS "${CMAKE_BINARY_DIR}/boot_recovery.skprx"
            "${CMAKE_BINARY_DIR}/boot_trigger.suprx"
)
add_dependencies(${PROJECT_NAME} boot_plugins)

target_include_directories(${PROJECT_NAME} PRIVATE
    source
    $ENV{VITASDK}/share/gcc-arm-vita-eabi/samples/common
)

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
    m
    c
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

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
