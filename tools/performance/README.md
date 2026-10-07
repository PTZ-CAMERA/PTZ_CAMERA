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
