#pragma once

#include <cstdint>

namespace ptz {
// active-low 버튼 등록, debounce, 등록 해제를 담당한다.
bool configureCenterButton();
void cleanupCenterButton();
void handleCenterButton(int gpio, int level, uint32_t tick, void* userdata);
} // namespace ptz
