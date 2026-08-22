cmake_minimum_required(VERSION 3.20)
project(cad_converter2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CAD_CONVERTER_VERSION "V0.34D" CACHE STRING "Application version label")
set(CAD_CONVERTER_OUTPUT_SUFFIX "-D" CACHE STRING "Suffix inserted before converted output extension")
configure_file(src/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.hpp" @ONLY)

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
set(CAD_CONVERTER_HAS_DRACO 0)
if(EXISTS "${DRACO_RUNTIME_DLL}")
  set(CAD_CONVERTER_HAS_DRACO 1)
endif()

add_executable(cad-converter2 WIN32
  src/main.cpp
  src/gltf_validation.cpp
  src/xcaf_structure.cpp
)
add_dependencies(cad-converter2 gltfpack)
target_include_directories(cad-converter2 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${OpenCASCADE_INCLUDE_DIR}
)
target_link_directories(cad-converter2 PRIVATE ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(cad-converter2 PRIVATE
  ${OpenCASCADE_LIBRARIES}
  ole32
  opengl32
  comctl32
  shell32
  uuid
)
target_compile_definitions(cad-converter2 PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_definitions(cad-converter2 PRIVATE CAD_CONVERTER_HAS_DRACO=${CAD_CONVERTER_HAS_DRACO})
target_compile_options(cad-converter2 PRIVATE /EHsc)

add_executable(stl-benchmark bench/stl_benchmark.cpp)
target_include_directories(stl-benchmark PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_directories(stl-benchmark PRIVATE ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(stl-benchmark PRIVATE ${OpenCASCADE_LIBRARIES})
target_compile_definitions(stl-benchmark PRIVATE NOMINMAX)
target_compile_options(stl-benchmark PRIVATE /EHsc)

add_executable(iges-structure-test EXCLUDE_FROM_ALL
  bench/iges_structure_test.cpp
  src/xcaf_structure.cpp
)
target_include_directories(iges-structure-test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${OpenCASCADE_INCLUDE_DIR}
)
target_link_directories(iges-structure-test PRIVATE ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(iges-structure-test PRIVATE ${OpenCASCADE_LIBRARIES})
target_compile_definitions(iges-structure-test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(iges-structure-test PRIVATE /EHsc)

add_executable(gltf-validation-test EXCLUDE_FROM_ALL
  bench/gltf_validation_test.cpp
  src/gltf_validation.cpp
)
target_include_directories(gltf-validation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(gltf-validation-test PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_options(gltf-validation-test PRIVATE /EHsc)

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
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/docs"
    "$<TARGET_FILE_DIR:cad-converter2>/docs"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
    "$<TARGET_FILE_DIR:cad-converter2>/CHANGELOG.md"
)

if(EXISTS "${DRACO_RUNTIME_DLL}")
  add_custom_command(TARGET cad-converter2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${DRACO_RUNTIME_DLL}"
      "$<TARGET_FILE_DIR:cad-converter2>/draco.dll"
  )
endif()
