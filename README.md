# pose-osc-unreal

웹캠 포즈 추정(MediaPipe) 결과를 OSC(UDP)로 Unreal Engine 5.7에 보내서, 손 위치에 Niagara 파티클이 반응하는 공간 인터랙티브 미니 프로젝트예요.
모델 출력이 흔들리거나 파이프라인이 끊겨도 연출이 깨지지 않도록 폴백을 처음부터 설계에 넣었어요.

## 진행 상황

- [x] 설계 결정 — [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md)
- [x] Python 송신부 — 캡처 스레드 · Pose Landmarker · One Euro Filter · OSC 송신 · 장애 주입 옵션 · 수신 검사기 (남은 확인: 좌우 라벨 실측)
- [x] Unreal 수신부 — OSC 수신 컴포넌트 · 상태 머신 · 디버그 표시 (남은 확인: 에디터 PIE 재시작, 실제 카메라 가림)
- [ ] 연출 — Niagara 연결 · 인터랙션 볼륨 매핑 · 크로스페이드 (C++·`NS_PoseHands` 연결 완료, 이펙트 다듬기 남음 — [docs/NIAGARA_SETUP.md](docs/NIAGARA_SETUP.md) 7절)
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

## 폴더 구조

```text
python/
  pose_osc/                송신부 패키지 (python -m pose_osc)
    capture.py             캡처 스레드 + 최신 프레임 1칸 + 카메라 장애 감시 (D2, D11)
    pose.py                Pose Landmarker 래퍼 (D1)
    one_euro.py            One Euro Filter (D4)
    hand_filter.py         손별 게이트·리셋·축별 필터 (D4, D11)
    faults.py              장애 주입 (drop / freeze / noise)
    osc_sender.py          /dstrict/hands 인코딩·UDP 송신 (D3)
    synthetic.py           카메라 없이 정해진 움직임·이벤트를 내는 소스
    app.py, config.py      메인 루프, 설정(dataclass + argparse)
  tools/
    osc_receive_check.py   수신 검사기 (표준 라이브러리만, 송신 코드와 독립)
    download_model.py      모델 다운로드 + SHA-256 검증
  tests/                   pytest (단위 + localhost 루프백)
unreal/                    UE 5.7 C++ 프로젝트 (PoseOscUnreal.uproject, 게임 모듈 1개)
  Config/DefaultGame.ini   수신 주소·포트·허용 송신자 (Config=Game)
  Source/PoseOscUnreal/
    PoseOscReceiverComponent   전송: UOSCServer 수명, 형식 검증, seq, 최신 값 1칸 (D5)
    PoseOscMessageDecoder      /dstrict/hands 엄격 디코드 (D3)
    PoseSeqTracker             seq 규칙 (순수 C++)
    PoseInteractionStateMachine 해석: 손별 Tracking/Holding/Lost + 전역 Interactive/Idle (순수 C++, D6)
    PoseInteractionComponent   연결: 매 Tick 최신 프레임 → 상태 머신, 화면 디버그 표시
    PoseNiagaraDriverComponent 연출: 볼륨 매핑 · 렌더 보간 · 크로스페이드 → Niagara User 파라미터 (D13)
    PosePresentationMath       연출 계산 순수 함수 (매핑, exp 보간, 가중치)
    PoseInteractionActor, PoseOscGameMode  루트 Box = 인터랙션 볼륨 + 카메라 + Niagara, 빈 맵이면 자동 생성
    Private/Tests/PoseOscSpecs.cpp       Automation Spec (24개)
  Content/FX/NS_PoseHands    Niagara 시스템 (에디터에서 작성, Git LFS)
docs/
  DESIGN_DECISIONS.md     설계 결정 — 선택지, 채택 이유, 검증 필요 항목
  TROUBLESHOOTING.md      막힘과 해결 기록
  NEXT_SESSION_PROMPT.md  다음 작업 세션 시작 프롬프트
  NIAGARA_SETUP.md        Niagara 시스템 NS_PoseHands 만드는 순서
```

## 환경

- Unreal Engine 5.7.x (C++) · Rider · Windows
- Python 3.11 + venv · MediaPipe Tasks API (CPU 추론)
- 웹캠 1280×720 @ 30fps · GPU RTX 4050 Laptop (Unreal 렌더 전용)

## 실행 방법

### Python 송신부 (PowerShell, 레포 루트 기준)

**1) 설치** — Python 3.11이 필요해요 (`py -0`으로 확인, 없으면 `winget install --id Python.Python.3.11 --scope user`).

```powershell
py -3.11 -m venv python\.venv
python\.venv\Scripts\python -m pip install -r python\requirements-dev.txt
cd python
.venv\Scripts\python tools\download_model.py          # models\pose_landmarker_lite.task (SHA-256 검증)
```

**2) 수신 검사기** (터미널 1) — Unreal 없이 송신을 확인해요. Unreal과 같은 `0.0.0.0:8000`을 쓰기 때문에 둘을 동시에 띄우면 나중 쪽이 바인딩 오류로 멈춰요 (의도된 동작, TROUBLESHOOTING T-006).

```powershell
.venv\Scripts\python tools\osc_receive_check.py
```

**3) 송신부** (터미널 2)

```powershell
.venv\Scripts\python -m pose_osc --preview                    # 웹캠
.venv\Scripts\python -m pose_osc --source synthetic            # 카메라·모델 없이
```

- 키: `f` 필터 토글, `q` 종료 (콘솔 또는 미리보기 창), Ctrl+C도 돼요.
- 처리·송신은 기본 30Hz예요 (`--max-rate`, 0이면 제한 없음). 내장 카메라가 1080p@60만 줘서 이렇게 정했어요 (DESIGN_DECISIONS D9 `변경` 메모).
- 기대 출력 (송신부, 2초마다):
  ```text
  capture 60.0fps (덮어씀 60, read 실패 누적 0) | infer 19.0ms (max 42.6) | 송신 29.8Hz (drop 0, freeze 0) | 지연 cap→send 27.9ms ... | L vis 0.90 pres 1.00 jitter 0.0170→0.0026 | ... | filter ON
  ```
- 기대 출력 (수신 검사기, 1초마다):
  ```text
  [   3.0s]  30.0Hz (간격 avg 33.3 / max 47.0ms) | 유실  0.0% (0/30) | seq 76 person 1 | L (0.256, 0.433, -0.100) c0.95 | R (...) | 누적 형식오류 0 값경고 0 중복 0 역순 0 재시작 0
  ```

**4) 장애 주입** (DESIGN_DECISIONS.md §6, D11)

| 옵션 | 수신 검사기에서 보이는 것 |
|---|---|
| `--drop-rate 0.3` | 유실 약 30%, 약 21Hz |
| `--freeze-after 5` (`--freeze-for 3`) | 5초 뒤 `IDLE`. `--freeze-for`를 주면 3초 뒤 재개하고 `seq 기준 초기화 (idle_reset)` |
| `--noise 0.02` | 좌표가 흔들림. 송신부 로그의 `jitter raw→필터`로 효과 비교, `f`로 토글 |
| `--source synthetic` | 12초 주기: 왼손 c0.50(히스테리시스 구간) → 오른손 c0.10(가림) → `person 0`(사람 없음) |

**5) 테스트**

```powershell
.venv\Scripts\python -m pytest            # 전체 (루프백 포함 약 40초)
.venv\Scripts\python -m pytest -m "not e2e"  # 단위 테스트만
```

### Unreal 수신부 (UE 5.7, 레포 루트 기준)

**1) 빌드** — VS 2022(17.14 / MSVC 14.44에서 확인)와 Windows SDK가 필요해요. 에디터에서 `unreal\PoseOscUnreal.uproject`를 열고 "다시 빌드" 확인 창에서 예를 눌러도 되고, 명령으로 빌드해도 돼요.

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" PoseOscUnrealEditor Win64 Development "-Project=$PWD\unreal\PoseOscUnreal.uproject" -WaitMutex
```

- OSC 플러그인은 `.uproject`에서 켜 두었어요 (엔진 기본값은 꺼짐). Build.cs 의존: Public `Core`·`CoreUObject`·`Engine`, Private `OSC`·`Sockets`·`Networking`.
- Rider에서는 `.uproject`를 바로 열면 돼요. `.sln`은 생성물이라 커밋하지 않아요.

**2) 실행** — 에디터를 열고 Play(PIE). 시작 맵은 엔진의 빈 맵(`/Engine/Maps/Entry`)이고, `PoseOscGameMode`가 `PoseInteractionActor`를 자동으로 만들어요. 그다음 Python 송신부를 실행하면 화면 왼쪽 위에 상태가 표시돼요.

```text
[PoseOsc] INTERACTIVE 6.5s | last packet 0.01s ago | streak 6.74s | packets continuous for 0.24s >= 0.20s
listening 0.0.0.0:8000 | 30.0 Hz, loss 0.0% | accepted 203 lost 0 dup 0 stale 0 restarts 0 rejected 0
L: TRACKING 6.5s | conf 0.95 | target (0.279, 0.409) | conf 0.95 >= 0.60
R: LOST     0.3s | conf 0.10 | target (0.683, 0.454) | Holding 0.51s > 0.50s
```

- 전환은 Output Log의 `LogPoseOsc`에도 찍혀요 (예: `Right hand Holding -> Lost (Holding 0.503s > 0.50s)`).
- D6 수치(0.6 / 0.4 / 0.5초 / 0.2초 / 0.15초 / 1.0초)는 PIE 중 액터의 `Interaction` 컴포넌트 Details에서 바로 바꿀 수 있어요.
- 수신 주소는 `unreal\Config\DefaultGame.ini`. 엔진 OSC 플러그인이 `127.0.0.1`을 LAN IP로 바꿔 바인딩하기 때문에 `0.0.0.0` + 허용 송신자 `127.0.0.1`로 두었어요 (DESIGN_DECISIONS D3 `변경` 메모, T-005).

**3) 시연 시나리오** (DESIGN_DECISIONS.md §6)

| 시나리오 | 하는 일 | 화면에서 보이는 것 |
|---|---|---|
| 1. 신뢰도 폴백 | 손으로 카메라를 가림 (카메라 없이: `--source synthetic`의 6.0~7.5초 구간) | 해당 손 `TRACKING → HOLDING`(노랑) → 0.5초 뒤 `LOST`(빨강) → 손을 떼면 `TRACKING` |
| 2. 패킷 끊김 폴백 | Python을 Ctrl+C로 종료 → 다시 실행 (한 번의 실행으로: `--freeze-after 5 --freeze-for 3`) | 1.0초 뒤 `IDLE`, 양손 `LOST` → 재실행 0.2초 뒤 `INTERACTIVE` (로그에 `Seq baseline 0`) |
| 3. 필터 비교 | `--noise 0.02`로 실행하고 `f`로 토글 | 3단계(Niagara)에서 확인 |

**4) 테스트 (헤드리스)**

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\PoseOscUnreal.uproject" "-ExecCmds=Automation RunTests PoseOscUnreal;Quit" -unattended -nullrhi -nosplash
```

기대 출력: 마지막 줄 `**** TEST COMPLETE. EXIT CODE: 0 ****`, `unreal\Saved\Logs\PoseOscUnreal.log`에 `Result={Success}` 24개. 시작 직후의 `LogAutomationTest: Error: Condition failed` 4줄은 엔진 쪽 출력이에요 (DESIGN_DECISIONS D12 참고).

### 좌우 라벨 확인 (DESIGN_DECISIONS.md §8의 3번)

`--preview`로 실행하고 **왼손만** 들어요. 미러 화면에서 "L" 원이 들고 있는 손에 붙고, 수신 검사기의 `L (x, ...)`가 0.5보다 작아야 맞아요 (화면 왼쪽 = 작은 x).

## 저장소 메모

- `.uasset`/`.umap`은 Git LFS로 관리해요. Git for Windows에는 git-lfs가 기본 포함돼 있어요. 설치 때 꺼 두었다면 `git lfs install`을 한 번 실행해 주세요.
- MediaPipe 모델 파일(`.task`)은 커밋하지 않고 다운로드 스크립트로 받아요.
- 시연 영상은 레포에 넣지 않고 링크로 남겨요.
