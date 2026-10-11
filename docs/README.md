# Pi 문서 안내

## 현재 구현

1. [프로젝트 실행·기능](../README.md)
2. [Pi / VMS / Qt / Web 전체 구조](SYSTEM_ARCHITECTURE.md)
3. [Pi 모듈과 분석·제어 흐름](EDGE_CAMERA_ARCHITECTURE.md)
4. [코드 읽는 순서](CODE_GUIDE.md)

## 계약·학습·검증

| 주제 | 문서 |
|---|---|
| 최신 소스의 JSON 설정·Tracking 제어 계약 | [소프트웨어 보고서](EDGE_CAMERA_SOFTWARE_REPORT.md) |
| ONVIF PTZ | [계약](onvif/qt_ptz_contract.md) |
| TCP/UDP | [전송 방식](rtsp_tcp_udp.md) |
| 기술 선택 | [technology_decisions](technology_decisions.md) |
| confidence·IoU·NMS | [탐지 개념](object_detection_concepts.md) |
| 성능 측정 조건·한계 | [performance](performance/README.md) |

## 초기 설계와 과거 기록

[PROJECT_ARCHITECTURE](PROJECT_ARCHITECTURE.md)는 초기 migration 계획,
초기 Pi 설계는 Git 이력에서 확인할 수 있다.
[EDGE_CAMERA_IMPLEMENTATION](EDGE_CAMERA_IMPLEMENTATION.md), 날짜별 sessions와
[troubleshotting](../troubleshotting.md)은 작성 당시의 구현·시험·문제 기록이다.
과거 수치를 현재 추적 개선의 성능 측정으로 사용하지 않는다.
