cmake_minimum_required(VERSION 3.16)
project(dmg07_libretro C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DMG07_TESTS "Build the Godot-free protocol test" ON)

set(DMG07_SOURCES src/libretro.c src/dmg07.c)

add_library(dmg07_libretro SHARED ${DMG07_SOURCES})
target_include_directories(dmg07_libretro PRIVATE include src)

# The file name is the contract with every frontend: no "lib" prefix anywhere,
# and Android's suffix added by the release workflow when it is packaged.
set_target_properties(dmg07_libretro PROPERTIES PREFIX "")

if(NOT MSVC)
  target_link_libraries(dmg07_libretro PRIVATE m)
endif()

if(MSVC)
  target_compile_options(dmg07_libretro PRIVATE /W4 /wd4100)
  # Static CRT: a core is copied into a temp dir and loaded from there, and must
  # not depend on a runtime the player may not have.
  set_property(TARGET dmg07_libretro PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  target_compile_options(dmg07_libretro PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
  if(APPLE)
    target_link_options(dmg07_libretro PRIVATE -Wl,-exported_symbol,_retro_*)
  elseif(WIN32)
    # mingw: export only retro_*, and carry libgcc in the DLL.
    target_link_options(dmg07_libretro PRIVATE -static-libgcc
      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T)
  else()
    target_link_options(dmg07_libretro PRIVATE
      -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T -Wl,--no-undefined)
  endif()
endif()

if(DMG07_TESTS AND NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  # The test #includes the protocol, so it links nothing of the library's.
  add_executable(dmg07_test tests/dmg07_test.c)
  target_include_directories(dmg07_test PRIVATE include src)
  if(MSVC)
    target_compile_definitions(dmg07_test PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  add_test(NAME dmg07_test COMMAND dmg07_test)
endif()
