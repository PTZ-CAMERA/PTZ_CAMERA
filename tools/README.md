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
