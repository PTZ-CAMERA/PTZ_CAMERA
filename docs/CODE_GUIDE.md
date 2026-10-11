# Pi 코드 구조와 책임

설치 스크립트·service·기존 pi/ 소스 위치와 edge-camera/ 통합 CMake는 유지한다. 이번 정리는 설명 주석과 문서뿐이며 GPIO·명령·추적 정책·설정·실행 파일을 변경하지 않는다.

| 위치 | 책임 |
|---|---|
| edge-camera | 통합 빌드·공통 JSON 설정 |
| pi/camera_stream | 단일 캡처·H.264 발행·최신 프레임 분석·추적 계산 |
| pi/servo_test | 제어 중재·이동 계획·단독 GPIO 출력 |
| pi/onvif_service | Digest·SOAP·Events·독립 JSON 제어 |
| common | 설정·제한·metadata·상태·rate limiter·계측 |
| tools / docs | 진단·측정 / 계약·배포 차이·시험 기록 |

## 읽는 순서

CaptureManager → PersonDetector/TargetSelector → TrackingController → ServoClient → ControlArbiter → PtzController/ServoDriver → EventBroker 순서로 흐름을 확인한다.

## 유지 규칙

- camera_stream/ONVIF는 GPIO를 직접 제어하지 않는다. servo service의 IPC를 사용한다.
- Tracking 요청 ON과 현재 자동 출력 허용은 다르다. 수동 개입은 generation으로 오래된 추론 결과를 거른다.
- Capture UTC 추정·분석 UTC·publisher PTS·streamEpoch/frameId·제어 generation은 다른 정보다.
- ONVIF property의 최신 상태와 Runtime 발생 기록을 구분한다. 센서 노출 시각·모터 실제 위치로 해석하지 않는다.
- DB·녹화·검색·LLM은 PC VMS 책임이다. 소스 변경과 설치 서비스 적용을 구분한다.

기존 학습 주석에 책임·단위·수명 설명을 보완했다. C++에서 주석/공백을 제거한 토큰 비교로 변경 전후 로직 동일성을 확인한다. 발열과 물리 장치 영향을 피하기 위해 이번 정리에서 Pi 빌드·서비스 재시작·모터 시험은 실행하지 않는다.

[프로젝트 README](../README.md) · [배포·제어 계약](EDGE_CAMERA_SOFTWARE_REPORT.md)
