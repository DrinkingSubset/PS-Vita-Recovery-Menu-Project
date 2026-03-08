cmake_minimum_required(VERSION 3.5)
project(VitaRecovery)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "PS Vita Recovery")
set(VITA_TITLEID  "RECM00001")
set(VITA_VERSION  "01.00")

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

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME "${VITA_APP_NAME}"
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE boot_recovery.skprx boot_recovery.skprx
)
