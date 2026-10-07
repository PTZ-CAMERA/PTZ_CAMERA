#pragma once

#include "performance_log.h"
#include <gst/gst.h>

namespace ptz::perf {
// 1초 단위 관측값이다. 관측 사이의 큐 최대 깊이를 보장하지 않는다.
inline void recordQueueLevels(GstElement* pipeline) {
    if (!enabled()) return;
    for (const char* name : {"stream-queue", "analysis-queue"}) {
        auto* queue = gst_bin_get_by_name(GST_BIN(pipeline), name);
        if (!queue) continue;
        guint buffers = 0;
        guint64 queuedTime = 0;
        g_object_get(queue, "current-level-buffers", &buffers,
                     "current-level-time", &queuedTime, nullptr);
        instant("queue_state", buffers, queuedTime / GST_MSECOND, name);
        gst_object_unref(queue);
    }
}
} // namespace ptz::perf
