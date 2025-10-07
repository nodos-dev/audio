# Copyright MediaZ Teknoloji A.S. All Rights Reserved.
cmake_minimum_required(VERSION 3.24.2)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 20)

if (NOT WITH_NODOS_WORKSPACE)
    message(FATAL_ERROR "This repo currently does not support builds without Nodos workspace. "
    "Place this repo under nodos-workspace/Module folder and run cmake -S ./Toolchain/CMake -B Build from workspace root.")
endif()

add_compile_definitions(NOS_DISABLE_DEPRECATED)
# Nodos SDK
nos_find_plugin_sdk("40.0.0" NOS_PLUGIN_SDK_TARGET NOS_SDK_DIR)

# nos.sys.vulkan
nos_get_plugin("nos.sys.vulkan" "8.0" NOS_SYS_VULKAN_TARGET)
nos_find_plugin_path("nos.sys.vulkan" "8.0" NOS_SYS_VULKAN_PATH)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/AudioFile)

# Project generation
nos_generate_flatbuffers("${CMAKE_CURRENT_SOURCE_DIR}/Types" "${CMAKE_CURRENT_SOURCE_DIR}/Source" "cpp" 
    "${NOS_SDK_DIR}/Types;${NOS_SYS_VULKAN_PATH}" nosAudio_generated)
list(APPEND DEPENDENCIES AudioFile nosAudio_generated ${NOS_SYS_VULKAN_TARGET} ${NOS_PLUGIN_SDK_TARGET})
nos_add_plugin("nosAudio" "${DEPENDENCIES}" "${NOS_SYS_VULKAN_PATH}/Include/nosVulkanSubsystem")
nos_group_targets("nosAudio" "NOS Plugins")