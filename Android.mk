LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := su
LOCAL_SRC_FILES := su.c db.c activity.cpp

SU_SHARED_LIBRARIES := liblog libsqlite
ifeq (0,$(shell [ -n $(PLATFORM_SDK_VERSION) -a $(PLATFORM_SDK_VERSION) -lt 4 ]))
LOCAL_CFLAGS += -DSU_LEGACY_BUILD
SU_SHARED_LIBRARIES += libandroid_runtime
else
SU_SHARED_LIBRARIES += libcutils libbinder libutils
LOCAL_MODULE_TAGS := debug,eng
endif

LOCAL_C_INCLUDES += external/sqlite/dist

LOCAL_SHARED_LIBRARIES := $(SU_SHARED_LIBRARIES)

LOCAL_MODULE_PATH := $(TARGET_OUT_OPTIONAL_EXECUTABLES)

include $(BUILD_EXECUTABLE)
