cmake_minimum_required(VERSION 3.10)
project(TardsPipeline)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(pipeline/include)
include_directories(.)

# Pipeline library sources
set(PIPELINE_SOURCES
    pipeline/src/pipeline.cpp
    pipeline/src/pipeline_node.cpp
    pipeline/src/pipeline_nodes.cpp
    pipeline/src/pipeline_pad.cpp
    pipeline/src/pipeline_pads.cpp
)

# Pipeline library headers
set(PIPELINE_HEADERS
    pipeline/include/pipeline/pipeline.h
    pipeline/include/pipeline/pipeline_node.h
    pipeline/include/pipeline/pipeline_nodes.h
    pipeline/include/pipeline/pipeline_packet.h
    pipeline/include/pipeline/pipeline_pad.h
    pipeline/include/pipeline/pipeline_pads.h
)

# Tardsplaya Pipeline integration sources
set(TARDSPLAYA_PIPELINE_SOURCES
    pipeline_manager.cpp
)

# Tardsplaya Pipeline integration headers
set(TARDSPLAYA_PIPELINE_HEADERS
    pipeline_manager.h
    pipeline_stream_nodes.h
    pipeline_stream_packets.h
)

# Create Pipeline library
add_library(pipeline STATIC ${PIPELINE_SOURCES})
target_include_directories(pipeline PUBLIC pipeline/include)

# Find required system libraries
find_package(Threads REQUIRED)

# Create Pipeline example executable
add_executable(pipeline_example 
    pipeline_example.cpp
    ${TARDSPLAYA_PIPELINE_SOURCES}
)

target_link_libraries(pipeline_example 
    pipeline 
    Threads::Threads
)

# Compiler-specific options
if(MSVC)
    target_compile_options(pipeline PRIVATE /W4)
    target_compile_options(pipeline_example PRIVATE /W4)
else()
    target_compile_options(pipeline PRIVATE -Wall -Wextra -pedantic)
    target_compile_options(pipeline_example PRIVATE -Wall -Wextra -pedantic)
endif()

# Set output directories
set_target_properties(pipeline_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

set_target_properties(pipeline PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

# Create a simple test
enable_testing()
add_test(NAME pipeline_test COMMAND pipeline_example)

# Install targets
install(TARGETS pipeline pipeline_example
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${PIPELINE_HEADERS} DESTINATION include/pipeline)
install(FILES ${TARDSPLAYA_PIPELINE_HEADERS} DESTINATION include/tardsplaya)

# Print build configuration
message(STATUS "Pipeline Library Build Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Pipeline Sources: ${PIPELINE_SOURCES}")
message(STATUS "  Integration Sources: ${TARDSPLAYA_PIPELINE_SOURCES}")