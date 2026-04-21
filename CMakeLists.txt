# Copyright MediaZ Teknoloji A.S. All Rights Reserved.

target_compile_definitions(${NOS_PLUGIN_TARGET} PRIVATE NOS_DISABLE_DEPRECATED)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/External/AudioFile)
target_link_libraries(${NOS_PLUGIN_TARGET} PRIVATE AudioFile)

# SystemAudioCapture has one backend per OS. The Source/ glob picks up every
# .cpp unconditionally, so exclude the platform files that don't match the
# current target to keep the build clean, then add the .mm file back on
# macOS (the glob doesn't match .mm extensions).
if (NOT WIN32)
	set_source_files_properties(
		${CMAKE_CURRENT_SOURCE_DIR}/Source/SystemAudioCaptureWindows.cpp
		PROPERTIES HEADER_FILE_ONLY ON)
endif()

if (APPLE)
	target_sources(${NOS_PLUGIN_TARGET} PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/Source/SystemAudioCaptureMac.mm)
	# ARC keeps retain/release of SCStream/SCContentFilter/etc. out of our
	# hair; the other .mm files in the engine deliberately avoid ARC, but
	# here we hold Obj-C objects across method boundaries so it's worth it.
	set_source_files_properties(
		${CMAKE_CURRENT_SOURCE_DIR}/Source/SystemAudioCaptureMac.mm
		PROPERTIES COMPILE_FLAGS "-fobjc-arc")
	target_link_libraries(${NOS_PLUGIN_TARGET} PRIVATE
		"-framework Foundation"
		"-framework CoreMedia"
		"-framework CoreAudio"
		"-framework AudioToolbox"
		"-framework ScreenCaptureKit")
endif()
