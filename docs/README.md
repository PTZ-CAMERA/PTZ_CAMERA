# 프로젝트 문서

## 문서 구조

```text
docs/
├── PROJECT_ARCHITECTURE.md       # Edge / VMS / Client 책임과 이전 계획
├── EDGE_CAMERA_ARCHITECTURE.md   # Pi 내부 설계와 흐름
├── EDGE_CAMERA_SOFTWARE_REPORT.md # 최신 소스·적용 상태·VMS 계약
├── EDGE_CAMERA_IMPLEMENTATION.md # 이전 구현과 적용 기록
├── technology_decisions.md      # 기술 선택 이유
├── object_detection_concepts.md # confidence / IoU / NMS
├── onvif/                       # PTZ 계약과 응답 예시
├── performance/                 # 측정 조건·결과·향후 비교
└── sessions/                    # 날짜별 작업 기록
```

## 읽는 순서

전체 구조 → Pi 설계 → 최신 구현 보고서 → 각 서비스 README → 소스 순서로 읽습니다.
날짜별 기록은 당시 상태를 설명하며 현재 구현의 보장으로 사용하지 않습니다.

```text
프로젝트 설계
    └── Pi 내부 설계
         ├── 영상·탐지·자동 추적
         ├── PTZ·GPIO·제어 중재
         └── ONVIF·metadata·VMS 연결
              └── 구현 보고서 / 세션 기록 / 측정 문서
```

문제 조사 기록은 루트의 [troubleshotting.md](../troubleshotting.md)에 있습니다.
실물 검증과 장시간 시험은 사용자 수행 대상으로 남아 있습니다.

## 현재 시스템 연동 기준 (2026-10-11)

Qt 라이브는 Pi 직접 RTSP, Web 라이브는 PC VMS 내부 libdatachannel WebRTC입니다. PC MediaMTX 게이트웨이는 제거했고 Pi RTSP 서버는 유지합니다. 녹화·SQLite·자연어 검색은 VMS 책임입니다.

최신 Pi 소스의 독립 /camera/control 추적 설정과 VMS의 기존 ONVIF Tracking 계약은 아직 맞춰야 합니다. 소스 업데이트가 설치 서비스 적용을 의미하지 않습니다. 전체 역할·검증 범위는 [저장소 README](../README.md)를 기준으로 확인하세요.
