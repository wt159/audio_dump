cmake_minimum_required(VERSION 3.15)
project(audio_dump C)

# Desktop builds may opt in to the status query APIs. The flag is PUBLIC so
# consumers see the declarations and the symbols get default visibility.
option(DUMP_PCM_DESKTOP_OBSERVE "Expose and export the status query APIs" OFF)

set(AUDIO_DUMP_SOURCES
    src/logger.c
    src/queue.c
    src/session.c
    src/file_mgr.c
    src/dump_control.c
    src/dump_writer.c
    src/dump_pcm_data.c)

add_library(audio_dump STATIC ${AUDIO_DUMP_SOURCES})

target_include_directories(audio_dump
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(audio_dump PUBLIC c_std_11)
target_compile_definitions(audio_dump PRIVATE DUMP_PCM_PC _POSIX_C_SOURCE=200809L)

if(DUMP_PCM_DESKTOP_OBSERVE)
    target_compile_definitions(audio_dump PUBLIC DUMP_PCM_DESKTOP_OBSERVE)
endif()

if(WIN32)
    target_link_libraries(audio_dump PUBLIC pthread)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(audio_dump PUBLIC Threads::Threads)
endif()
