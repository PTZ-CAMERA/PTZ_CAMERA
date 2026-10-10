# 개발 도구

서비스 운영 코드와 분리된 계측·분석 도구입니다.

## 디렉토리와 처리 구조

```text
tools/
└── performance/
    ├── CMakeLists.txt          # C++ 계측 도구 빌드
    ├── run_suite.sh            # 조건별 비교 실행
    ├── servo_logging_test.cpp  # GPIO 출력 대체 계측
    ├── build/                 # 로컬 빌드 산출물
    └── results/               # 실행 결과 (Git 제외)

서비스 계측 / 비교 실행 → CSV → 요약·분석 → docs/performance
```

실행 방법과 제약은 [performance README](performance/README.md)에 있습니다.
시험 명령은 사용자가 실행할 때만 동작하며 서비스 설치 과정에서 자동 실행하지 않습니다.

## 현재 시스템 연동 기준 (2026-10-11)

Qt 라이브는 Pi 직접 RTSP, Web 라이브는 PC VMS 내부 libdatachannel WebRTC입니다. PC MediaMTX 게이트웨이는 제거했고 Pi RTSP 서버는 유지합니다. 녹화·SQLite·자연어 검색은 VMS 책임입니다.

최신 Pi 소스의 독립 /camera/control 추적 설정과 VMS의 기존 ONVIF Tracking 계약은 아직 맞춰야 합니다. 소스 업데이트가 설치 서비스 적용을 의미하지 않습니다. 전체 역할·검증 범위는 [저장소 README](../README.md)를 기준으로 확인하세요.
