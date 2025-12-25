# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

add_compile_definitions(NOS_DISABLE_DEPRECATED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/AudioFile)
target_link_libraries(${NOS_PLUGIN_TARGET} PRIVATE AudioFile)