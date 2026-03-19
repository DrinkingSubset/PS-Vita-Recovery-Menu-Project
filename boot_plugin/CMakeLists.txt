cmake_minimum_required(VERSION 3.5)
project(BootRecovery)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -nostdlib -fno-builtin")

add_executable(boot_recovery
    boot_recovery.c
)

target_include_directories(boot_recovery PRIVATE
    $ENV{VITASDK}/arm-vita-eabi/include
    $ENV{VITASDK}/include
)

target_link_libraries(boot_recovery
    taihenForKernel_stub
    SceCtrlForDriver_stub
    SceIofilemgrForDriver_stub
    SceThreadmgrForDriver_stub
    SceSysmemForDriver_stub
    SceAppMgrForDriver_stub
)

vita_create_self(boot_recovery.skprx boot_recovery
    UNSAFE
)
