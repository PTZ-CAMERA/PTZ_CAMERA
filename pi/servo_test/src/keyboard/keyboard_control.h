#pragma once

#include <csignal>
#include <termios.h>

namespace ptz {
bool enableKeyboard(termios& savedTerminal);
void restoreKeyboard(const termios& savedTerminal);
// 키를 PTZ 프로토콜로 변환해 TCP 입력과 같은 제어 동작을 사용한다.
const char* commandForKey(char key);
void handleLocalKey(char key, volatile std::sig_atomic_t& stopRequested);
int runKeyboardClient(int fd, volatile std::sig_atomic_t& stopRequested);
} // namespace ptz
