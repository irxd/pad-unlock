cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(padunlock C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99 -nostdlib")

add_executable(padunlock
  src/main.c
)

target_link_libraries(padunlock
  gcc
  taihenForKernel_stub
  SceThreadmgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysrootForDriver_stub
  SceCtrlForDriver_stub
  ScePowerForDriver_stub
  SceRtcForDriver_stub
  SceIofilemgrForDriver_stub
)

set_target_properties(padunlock
  PROPERTIES LINK_FLAGS "-nostdlib"
  COMPILE_FLAGS "-D__VITA_KERNEL__"
)

vita_create_self(padunlock.skprx padunlock CONFIG exports.yml UNSAFE)
