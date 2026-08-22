cmake_minimum_required(VERSION 3.20)
project(cad_converter2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCASCADE 8.0.1 REQUIRED
  COMPONENTS FoundationClasses ModelingData ModelingAlgorithms Visualization
             ApplicationFramework DataExchange)

set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK ON CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/meshoptimizer)

set(DRACO_RUNTIME_DLL "" CACHE FILEPATH
  "Optional Draco runtime DLL used by a Draco-enabled OCCT build")

add_executable(cad-converter2 WIN32 src/main.cpp)
add_dependencies(cad-converter2 gltfpack)
target_include_directories(cad-converter2 PRIVATE
  ${OpenCASCADE_INCLUDE_DIR}
)
target_link_directories(cad-converter2 PRIVATE ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(cad-converter2 PRIVATE
  ${OpenCASCADE_LIBRARIES}
  ole32
  opengl32
  shell32
  uuid
)
target_compile_definitions(cad-converter2 PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(cad-converter2 PRIVATE /EHsc)

add_executable(stl-benchmark bench/stl_benchmark.cpp)
target_include_directories(stl-benchmark PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_directories(stl-benchmark PRIVATE ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(stl-benchmark PRIVATE ${OpenCASCADE_LIBRARIES})
target_compile_definitions(stl-benchmark PRIVATE NOMINMAX)
target_compile_options(stl-benchmark PRIVATE /EHsc)

add_custom_command(TARGET cad-converter2 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${OpenCASCADE_BINARY_DIR}"
    "$<TARGET_FILE_DIR:cad-converter2>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/run.bat"
    "$<TARGET_FILE_DIR:cad-converter2>/run.bat"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "$<TARGET_FILE:gltfpack>"
    "$<TARGET_FILE_DIR:cad-converter2>/gltfpack.exe"
)

if(EXISTS "${DRACO_RUNTIME_DLL}")
  add_custom_command(TARGET cad-converter2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${DRACO_RUNTIME_DLL}"
      "$<TARGET_FILE_DIR:cad-converter2>/draco.dll"
  )
endif()
