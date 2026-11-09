LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libaudiohal_pcm_dump
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := \
    src/logger.c \
    src/queue.c \
    src/session.c \
    src/file_mgr.c \
    src/dump_control.c \
    src/dump_writer.c \
    src/sidecar.c \
    src/dump_pcm_data.c

LOCAL_CFLAGS := \
    -std=c11 \
    -Wall \
    -Wextra \
    -Wno-unused-parameter \
    -DANDROID \
    -D_POSIX_C_SOURCE=200809L \
    -fvisibility=hidden

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH)/include \
    $(LOCAL_PATH)/src
LOCAL_EXPORT_C_INCLUDE_DIRS := $(LOCAL_PATH)/include

LOCAL_SHARED_LIBRARIES := liblog libcutils
LOCAL_VENDOR_MODULE := true

include $(BUILD_SHARED_LIBRARY)
