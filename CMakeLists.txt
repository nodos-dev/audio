# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/AudioFile)
target_link_libraries(${NOS_PLUGIN_TARGET} PRIVATE AudioFile)

target_compile_definitions(${NOS_PLUGIN_TARGET} PRIVATE NOS_DISABLE_DEPRECATED)