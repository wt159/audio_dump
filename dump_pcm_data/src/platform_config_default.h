#ifndef DUMP_PCM_PLATFORM_CONFIG_DEFAULT_H
#define DUMP_PCM_PLATFORM_CONFIG_DEFAULT_H

#define MAX_SESSIONS 16
#define MAX_OPEN_FILES 16
#define IDLE_TIMEOUT_SEC 5
#define MAX_QUEUE_BLOCKS 800
#define MAX_QUEUE_BYTES (512 * 1024 * 1024UL)

#define DUMP_ROOT_DIR "/data/vendor/audio/"
#define PROPERTY_NAME "vendor.audio.dump"
#define LOG_TAG "audio_hw_dump"

#endif
