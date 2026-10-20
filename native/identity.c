#include "quest_native_app.h"
#ifndef QUEST_NATIVE_APP_NAME
#define QUEST_NATIVE_APP_NAME "Unnamed native app"
#endif
const QuestNativeAppV1 *quest_native_app_v1(void) {
    static const QuestNativeAppV1 app = {
        QUEST_NATIVE_ABI_VERSION, sizeof(QuestNativeAppV1), QUEST_NATIVE_APP_NAME
    };
    return &app;
}
