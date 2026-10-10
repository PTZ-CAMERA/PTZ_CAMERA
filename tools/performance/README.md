# PTZ 성능 시험 도구

C++17로 구현한 이벤트 요약기, Linux 자원 수집기, GStreamer/OpenCV 비교 시험이다.
조건과 측정값의 정확한 의미는 [측정 안내](../../docs/performance/README.md)에 있다.

```sh
# 빠른 확인: 조건별 20초, 1회, 처음 5초 제외
./tools/performance/run_suite.sh 20 1 5

# 반복 비교: 조건별 180초, 3회, 처음 5초 제외
./tools/performance/run_suite.sh 180 3 5
```

시험은 합성 영상을 사용하고 실제 카메라나 GPIO에 접근하지 않는다.
출력은 `results/`에 저장하며 Git에서 제외한다. 시험마다 모델 해시, 소스 상태,
실행 환경, CSV, 조건별 Markdown 요약을 남긴다.

## 현재 시스템 연동 기준 (2026-10-11)

Qt 라이브는 Pi 직접 RTSP, Web 라이브는 PC VMS 내부 libdatachannel WebRTC입니다. PC MediaMTX 게이트웨이는 제거했고 Pi RTSP 서버는 유지합니다. 녹화·SQLite·자연어 검색은 VMS 책임입니다.

최신 Pi 소스의 독립 /camera/control 추적 설정과 VMS의 기존 ONVIF Tracking 계약은 아직 맞춰야 합니다. 소스 업데이트가 설치 서비스 적용을 의미하지 않습니다. 전체 역할·검증 범위는 [저장소 README](../../README.md)를 기준으로 확인하세요.
