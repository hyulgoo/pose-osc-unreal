# pose-osc-unreal

웹캠 포즈 추정(MediaPipe) 결과를 OSC(UDP)로 Unreal Engine 5.7에 보내서, 손 위치에 Niagara 파티클이 반응하는 공간 인터랙티브 미니 프로젝트예요.
모델 출력이 흔들리거나 파이프라인이 끊겨도 연출이 깨지지 않도록 폴백을 처음부터 설계에 넣었어요.

## 진행 상황

- [x] 설계 결정 — [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md)
- [ ] Python 송신부 — 캡처 스레드 · Pose Landmarker · One Euro Filter · OSC 송신 · 장애 주입 옵션
- [ ] Unreal 수신부 — OSC 수신 컴포넌트 · 상태 머신 · 디버그 표시
- [ ] 연출 — Niagara 연결 · 인터랙션 볼륨 매핑 · 크로스페이드
- [ ] 시연 — 장애 주입 시나리오 3종 녹화

## 파이프라인

```text
웹캠 → [Python] 캡처 스레드 → Pose Landmarker → One Euro Filter → OSC 송신
                                                          │ UDP · /dstrict/hands · 매 프레임 1메시지
       [Unreal 5.7] OSC 수신 → 최신 값 1칸 → 상태 머신 → 보간 → Niagara 파라미터
```

## 폴백 설계 요약

| 상황 | 동작 |
|---|---|
| 손 신뢰도 저하 (가림·화면 밖) | 마지막 유효 위치 유지 → 0.5초 뒤 해당 손만 기본 연출 |
| 패킷 끊김 (추론 프로세스 중단) | 1.0초 뒤 비인터랙티브 모드, 다시 0.2초 연속 수신하면 복귀 |
| 출력 떨림 | 신뢰도 히스테리시스(0.6/0.4) + One Euro Filter + 프레임레이트 독립 보간 |

모든 전환은 가중치 크로스페이드로 처리해서, 폴백이 끊김이 아니라 연출로 보이게 해요.

## 폴더 구조 (계획)

```text
python/   Python 송신부 — 포즈 추정, 필터, OSC 송신, 수신 테스트 스크립트
unreal/   UE 5.7 C++ 프로젝트
docs/
  DESIGN_DECISIONS.md     설계 결정 — 선택지, 채택 이유, 검증 필요 항목
  TROUBLESHOOTING.md      막힘과 해결 기록
  NEXT_SESSION_PROMPT.md  다음 작업 세션 시작 프롬프트
```

## 환경

- Unreal Engine 5.7.x (C++) · Rider · Windows
- Python 3.11 + venv · MediaPipe Tasks API (CPU 추론)
- 웹캠 1280×720 @ 30fps · GPU RTX 4050 Laptop (Unreal 렌더 전용)

## 실행 방법

구현 후 추가해요.

## 저장소 메모

- `.uasset`/`.umap`은 Git LFS로 관리해요. Git for Windows에는 git-lfs가 기본 포함돼 있어요. 설치 때 꺼 두었다면 `git lfs install`을 한 번 실행해 주세요.
- MediaPipe 모델 파일(`.task`)은 커밋하지 않고 다운로드 스크립트로 받아요.
- 시연 영상은 레포에 넣지 않고 링크로 남겨요.
