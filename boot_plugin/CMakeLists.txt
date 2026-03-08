cmake_minimum_required(VERSION 3.5)
project(BootRecovery)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -nostdlib")

add_executable(boot_recovery
    boot_recovery.c
)

# taihen kernel headers — also check $VITASDK/include
# If taihen.h is still not found, run: $VITASDK/bin/vdpm taihen
target_include_directories(boot_recovery PRIVATE
    $ENV{VITASDK}/arm-vita-eabi/include
    $ENV{VITASDK}/include
)

target_link_libraries(boot_recovery
    taihenForKernel_stub        # kernel-mode: taiHookFunctionExportForKernel, taiHookRelease, TAI_NEXT
    SceCtrlForDriver_stub       # ksceCtrlPeekBufferPositive
    SceIofilemgrForDriver_stub  # ksceIoOpen, ksceIoClose
    SceThreadmgrForDriver_stub  # ksceKernelDelayThread
    SceSysmemForDriver_stub     # basic kernel runtime
)

vita_create_self(boot_recovery.skprx boot_recovery
    UNSAFE
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/exports.yml"
)
