#ifndef QUEST_NATIVE_APP_H
#define QUEST_NATIVE_APP_H
#include <stdint.h>
#include <android/native_activity.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QUEST_NATIVE_ABI_VERSION 1u
/* Static storage; do not allocate or initialize XR from this query. */
typedef struct QuestNativeAppV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *name;
} QuestNativeAppV1;
__attribute__((visibility("default"))) const QuestNativeAppV1 *quest_native_app_v1(void);
/* Also export the standard ANativeActivity_onCreate entry. Its activity,
 * callbacks, JVM and saved-state arguments have the normal NDK meanings.
 * No C++ objects cross this boundary. Runtime upgrades must retain ABI v1. */
#ifdef __cplusplus
}
#endif
#endif
