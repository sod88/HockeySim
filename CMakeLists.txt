cmake_minimum_required(VERSION 3.20)
project(nhl_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NHL_SIM_VIEWER "Build the raylib 2D viewer (downloads raylib 5.5 if not installed)" ON)

# ---- simulation core: no graphics dependencies ----
add_library(hkcore STATIC src/core/League.cpp src/core/Game.cpp)
target_include_directories(hkcore PUBLIC src)
if(MSVC)
  target_compile_options(hkcore PUBLIC /utf-8 /W3)
else()
  target_compile_options(hkcore PRIVATE -Wall -Wextra)
endif()

# ---- headless batch runner / calibration ----
add_executable(headless_sim tools/headless_sim.cpp)
target_link_libraries(headless_sim PRIVATE hkcore)

# ---- 2D viewer ----
if(NHL_SIM_VIEWER)
  find_package(raylib 5.5 QUIET)
  if(NOT raylib_FOUND)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(raylib
      GIT_REPOSITORY https://github.com/raysan5/raylib.git
      GIT_TAG 5.5
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(raylib)
  endif()

  add_executable(nhl_sim src/viewer/main.cpp)
  target_link_libraries(nhl_sim PRIVATE hkcore raylib)
  if(APPLE)
    target_link_libraries(nhl_sim PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
  endif()
  if(WIN32)
    set_target_properties(nhl_sim PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  endif()
  # Ship the stats next to the executable so it runs from anywhere.
  add_custom_command(TARGET nhl_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:nhl_sim>/data)
endif()
