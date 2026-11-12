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
    src/sidecar.c
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

# The PC suite is the same source on every platform: it reaches Windows through
# test/proc_iso.c (process isolation) and test/test_plat.h (POSIX spellings),
# not through a separate harness. Off by default so consumers of the static
# library never pay for it.
option(DUMP_PCM_BUILD_TESTS "Build the PC test harness and register it with CTest" OFF)

if(DUMP_PCM_BUILD_TESTS)
    enable_testing()

    # Only the test sources: the module objects come from the library, so the
    # harness can never end up linked against a second copy built with
    # different definitions.
    add_executable(dump_pcm_test
        test/main.c
        test/whitebox.c
        test/proc_iso.c)

    target_include_directories(dump_pcm_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/test)

    target_compile_features(dump_pcm_test PRIVATE c_std_11)
    target_compile_definitions(dump_pcm_test PRIVATE DUMP_PCM_PC _POSIX_C_SOURCE=200809L)

    # The allocator fault-injection cases intercept malloc/calloc/realloc with
    # --wrap, which only redirects real symbol references. Left to its own
    # devices at -O2 gcc treats them as builtins and reorders or elides the
    # calls, so an injection meant for the second allocation lands elsewhere.
    if(NOT MSVC)
        target_compile_options(dump_pcm_test PRIVATE
            -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc)
    endif()

    target_link_libraries(dump_pcm_test PRIVATE audio_dump)

    # Link-time fault injection. Every symbol below has to stay interceptable,
    # so nothing here may define DLL_EXPORT: that would turn the winpthread
    # declarations into dllimport and --wrap would stop working silently.
    target_link_options(dump_pcm_test PRIVATE
        -Wl,--wrap=malloc
        -Wl,--wrap=calloc
        -Wl,--wrap=realloc
        -Wl,--wrap=queue_pop
        -Wl,--wrap=pthread_mutex_init
        -Wl,--wrap=pthread_condattr_init
        -Wl,--wrap=pthread_condattr_setclock
        -Wl,--wrap=pthread_cond_init
        -Wl,--wrap=pthread_cond_timedwait
        -Wl,--wrap=opendir)

    add_test(NAME pc-suite COMMAND dump_pcm_test)
    set_tests_properties(pc-suite PROPERTIES
        ENVIRONMENT "AUDIO_DUMP_ENABLED=1;AUDIO_DUMP_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-root"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
