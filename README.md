# pose-osc-unreal

웹캠 포즈 추정(MediaPipe) 결과를 OSC(UDP)로 Unreal Engine 5.7에 보내서, 손 위치에 Niagara 파티클이 반응하는 공간 인터랙티브 미니 프로젝트예요.
모델 출력이 흔들리거나 파이프라인이 끊겨도 연출이 깨지지 않도록 폴백을 처음부터 설계에 넣었어요.

## 시연 영상

[![pose-osc-unreal 시연 영상](https://img.youtube.com/vi/f2koXpoyq-s/maxresdefault.jpg)](https://youtu.be/f2koXpoyq-s)

카메라 없이 정해진 손 움직임(합성 입력)을 보내면서 장애를 하나씩 주입한 54초 영상이에요. 장면마다 자막이 먼저 나오고 1~2초 뒤에 주입돼요. 왼쪽 위는 Unreal 화면의 상태 표시를 키워서 겹친 거예요.

| 장면 | 주입 | 보이는 것 |
|---|---|---|
| [① 0:07](https://youtu.be/f2koXpoyq-s?t=7) | 왼손 신뢰도를 0.42~0.58 사이로 흔듦 | 히스테리시스 구간이라 `TRACKING` 유지, 꽃이 깜빡이지 않음 |
| [② 0:14](https://youtu.be/f2koXpoyq-s?t=14) | 오른손 가림 (신뢰도 0.1) | `HOLDING` → 0.5초 뒤 `LOST`, 오른손 꽃이 닫힘 → 해제하면 바로 `TRACKING` |
| [③ 0:26](https://youtu.be/f2koXpoyq-s?t=26) | 송신 4초 중단 | 1초 뒤 `IDLE`, 꽃이 모두 닫힘 → 재개 0.2초 뒤 `INTERACTIVE` |
| [④ 0:37](https://youtu.be/f2koXpoyq-s?t=37) | 손을 멈추고 좌표 노이즈 σ 0.035 | 필터 OFF면 파티클이 떨리고 ON이면 가라앉음 (송신부 지터 약 0.11 → 0.011) |

- ④의 노이즈는 영상에서 잘 보이도록 아래 시연 시나리오 3의 0.02보다 키웠어요.
- 장면 순서와 길이는 녹화용 시간표로 맞췄어요. 같은 상황은 아래 [실행 방법](#실행-방법)의 시연 시나리오 옵션으로 재현할 수 있어요.

## 진행 상황

- [x] 설계 결정 — [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md) · 막힘과 해결 — [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)
- [x] Python 송신부 — 캡처 스레드 · Pose Landmarker · One Euro Filter · OSC 송신 · 장애 주입 옵션 · 수신 검사기 (좌우 라벨 실측 확인)
- [x] Unreal 수신부 — OSC 수신 컴포넌트 · 상태 머신 · 디버그 표시 (에디터 PIE 빠른 재시작, 실제 카메라 가림 확인)
- [x] 연출 — Niagara 연결 · 인터랙션 볼륨 매핑 · 크로스페이드 · `NS_PoseHands` 이펙트
- [x] 꽃밭 — 봉오리 꽃밭에서 손목 근처의 꽃만 피어나는 미디어 아트 연출 (`UPoseFlowerFieldComponent`, D14)
- [x] 시연 — 장애 주입 시나리오 3종 + 히스테리시스 녹화 — [시연 영상](#시연-영상) ([YouTube](https://youtu.be/f2koXpoyq-s))

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
  Config/DefaultGame.ini   수신 주소·포트·허용 송신자, 기본 Niagara 시스템·꽃 머티리얼 경로 (Config=Game)
  Scripts/create_flower_material.py  M_PoseFlower 생성 (에디터 Python, D14)
  Source/PoseOscUnreal/
    PoseOscReceiverComponent   전송: UOSCServer 수명, 형식 검증, seq, 최신 값 1칸 (D5)
    PoseOscMessageDecoder      /dstrict/hands 엄격 디코드 (D3)
    PoseSeqTracker             seq 규칙 (순수 C++)
    PoseInteractionStateMachine 해석: 손별 Tracking/Holding/Lost + 전역 Interactive/Idle (순수 C++, D6)
    PoseInteractionComponent   연결: 매 Tick 최신 프레임 → 상태 머신, 화면 디버그 표시
    PoseNiagaraDriverComponent 연출: 볼륨 매핑 · 렌더 보간 · 크로스페이드 → Niagara User 파라미터 (D13)
    PoseFlowerFieldComponent   연출: 볼륨 앞면 꽃밭 · 손목 거리로 개화 · 인스턴스 커스텀 데이터 (D14)
    PosePresentationMath       연출 계산 순수 함수 (매핑, exp 보간, 가중치, 개화 감쇠)
    PoseInteractionActor, PoseOscGameMode  루트 Box = 인터랙션 볼륨 + 카메라 + Niagara + 꽃밭, 빈 맵이면 자동 생성
    Private/Tests/PoseOscSpecs.cpp       Automation Spec (27개)
  Content/FX/NS_PoseHands    Niagara 시스템 (에디터에서 작성, Git LFS)
  Content/FX/M_PoseFlower    꽃잎 발광 머티리얼 (스크립트로 생성, Git LFS)
docs/
  DESIGN_DECISIONS.md     설계 결정 — 선택지, 채택 이유, 검증 필요 항목
  TROUBLESHOOTING.md      막힘과 해결 기록
```

## 환경

- Unreal Engine 5.7.x (C++) · Rider · Windows
- Python 3.11 + venv · MediaPipe Tasks API (CPU 추론)
- 웹캠: 내장 카메라가 1920×1080 @ 60fps만 제공해서 그대로 캡처하고, 처리·송신은 30Hz로 고정 (DESIGN_DECISIONS D9 `변경` 메모)
- GPU는 Unreal 렌더 전용 (RTX 4050 Laptop · RTX 4070 Laptop에서 확인)

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

**3) Niagara 시스템** (DESIGN_DECISIONS D13)

`unreal/Content/FX/NS_PoseHands`는 에디터에서 직접 만든 에셋이에요. C++ 드라이버(`UPoseNiagaraDriverComponent`)가 매 프레임 User 파라미터 5개를 채우고, Niagara는 이 값을 읽기만 해요. 상태 판단과 크로스페이드는 C++에서 끝나요.

| User 파라미터 | 타입 | 값 |
|---|---|---|
| `LeftHandPos`, `RightHandPos` | Position | 볼륨 안의 손 월드 위치 (렌더 보간 적용, 재획득 시 스냅) |
| `LeftHandWeight`, `RightHandWeight` | Float | 0~1 크로스페이드 (Tracking 1 · Holding 0.6 · Lost 0) |
| `InteractiveWeight` | Float | 0~1 크로스페이드 (Interactive 1 · Idle 0) |

- 이미터 작성 기준: `LeftHand`·`RightHand`(Fountain 기반)는 SpawnRate를 `User.LeftHandWeight * 300`(오른손도 같은 방식)으로, `Particles.Position`을 `User.LeftHandPos`에 연결하고 Local Space는 꺼요. 속도·중력을 낮추고 수명을 짧게 해서 파티클이 손 주변에 머물게 했어요. `Ambient`(Hanging Particulates 기반)는 SpawnRate를 `(1 - InteractiveWeight) * 30 + (2 - LeftHandWeight - RightHandWeight) * 10 + 3`으로 둬서 Idle이거나 손이 Lost일 때 공간을 채워요.
- 경로는 정확히 `/Game/FX/NS_PoseHands`여야 해요 (`DefaultGame.ini`의 `DefaultSystem`). 콘텐츠 브라우저의 `Content`가 곧 `/Game`이라서, `Game` 폴더를 따로 만들면 액터가 찾지 못해요 (T-008).
- Output Log로 연결을 확인해요: `Driving Niagara system NS_PoseHands`는 연결 성공, `Niagara system ... not found ...; debug drawing only`는 경로 문제, `Niagara User parameter 'X' is missing` / `has type ...`은 이름·타입 오타예요.
- 디버그 드로잉은 액터의 `NiagaraDriver` 컴포넌트에서 **Draw Debug**로 켜고 꺼요. 박스는 볼륨, 색 구는 보간된 손 위치(색 = 상태, 크기 = 가중치), 흰 점은 보간 전 수신 목표예요. 녹화할 때는 꺼 두세요. 시나리오 3에서는 흰 점이 필터 효과를 가장 잘 보여 줘서 켜 둬도 돼요.

**4) 꽃밭** (DESIGN_DECISIONS D14)

볼륨 앞면에 남색 봉오리 220송이가 숨 쉬듯 깜빡이고, 손목 근처(20cm 안은 활짝, 55cm까지 서서히)의 꽃만 피어나요. 손이 지나가면 1초 남짓에 걸쳐 천천히 닫혀서 꽃길이 남아요. `UPoseFlowerFieldComponent`는 Niagara 드라이버의 손 위치·가중치만 읽기 때문에 폴백이 그대로 이어져요 — Holding이면 반쯤 핀 채로 남고, Lost·Idle이면 닫혀요.

- 머티리얼 `/Game/FX/M_PoseFlower`는 스크립트로 만들어요. 에셋은 커밋돼 있으니, 그래프를 바꿨을 때만 에디터를 닫고 다시 실행하면 돼요.
  ```powershell
  & "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\PoseOscUnreal.uproject" -run=pythonscript "-script=$PWD\unreal\Scripts\create_flower_material.py" -unattended -nosplash
  ```
- Output Log: `Flower field uses M_PoseFlower`와 `Flower field: 220 flowers x (6 petals + core) = 1540 instances`가 나오면 정상이에요. `Flower material ... not found`가 나오면 꽃은 보이지만 빛나지 않아요 (위 스크립트를 실행하세요).
- 튜닝은 PIE 중 액터의 `Flowers` 컴포넌트 Details에서 해요. Bloom(반지름·열림/닫힘 시간), Look(팔레트·세기·꽃잎 각도·숨쉬기), Layout(개수·범위·크기·기울기·시드). Layout·꽃잎 수·팔레트를 바꾸면 꽃밭을 바로 다시 만들어요. **Draw Bloom Debug**를 켜면 손 주위에 개화 반지름 원이 보여요.

**5) 시연 시나리오** (DESIGN_DECISIONS.md §6)

송신부는 하나만 띄워요. 둘이면 먼저 연결된 쪽만 받고 화면에 `SECOND SENDER ignored`가 떠요 (T-007).

| 시나리오 | 하는 일 | 화면에서 보이는 것 |
|---|---|---|
| 1. 신뢰도 폴백 | 손으로 카메라를 가림 (카메라 없이: `--source synthetic`의 6.0~7.5초 구간) | 해당 손 `TRACKING → HOLDING`(노랑) → 0.5초 뒤 `LOST`(빨강) → 손을 떼면 `TRACKING` |
| 2. 패킷 끊김 폴백 | Python을 Ctrl+C로 종료 → 다시 실행 (한 번의 실행으로: `--freeze-after 5 --freeze-for 3`) | 1.0초 뒤 `IDLE`, 양손 `LOST` → 재실행 0.2초 뒤 `INTERACTIVE` (로그에 `Seq baseline 0`) |
| 3. 필터 비교 | `--noise 0.02 --no-filter --preview`로 실행하고 `f`로 토글 (카메라 없이: `--source synthetic --synthetic-no-events`를 추가) | 필터 OFF에서는 손을 멈춰도 흰 점과 파티클이 떨리고, ON으로 바꾸면 가라앉아요. 손 상태는 `TRACKING` 그대로예요 (노이즈는 좌표에만 들어가요). 송신부 로그의 `jitter raw→필터`가 정량 비교예요 |

- `f`는 Python 콘솔이나 미리보기 창이 포커스일 때만 먹어요. Unreal 창이 포커스면 키가 Unreal로 가요.
- 필터를 켜고 손을 빠르게 움직이면 조금 늦게 따라와요. 떨림을 줄이는 대신 반응이 느려지는 트레이드오프예요 (`--min-cutoff`, `--beta`로 조정).
- 미리보기 창 위쪽의 `filter ON/OFF` 표시와 Unreal 화면을 나란히 놓고 녹화하면, 언제 토글했는지 영상에서 바로 보여요.

**6) 테스트 (헤드리스)**

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\unreal\PoseOscUnreal.uproject" "-ExecCmds=Automation RunTests PoseOscUnreal;Quit" -unattended -nullrhi -nosplash
```

기대 출력: 마지막 줄 `**** TEST COMPLETE. EXIT CODE: 0 ****`, `unreal\Saved\Logs\PoseOscUnreal.log`에 `Result={Success}` 27개. 시작 직후의 `LogAutomationTest: Error: Condition failed` 4줄은 엔진 쪽 출력이에요 (DESIGN_DECISIONS D12 참고).

### 좌우 라벨 확인 (DESIGN_DECISIONS.md §8의 3번)

`--preview`로 실행하고 **왼손만** 들어요. 미러 화면에서 "L" 원이 들고 있는 손에 붙고, 수신 검사기의 `L (x, ...)`가 0.5보다 작아야 맞아요 (화면 왼쪽 = 작은 x).

## 저장소 메모

- `.uasset`/`.umap`은 Git LFS로 관리해요. Git for Windows에는 git-lfs가 기본 포함돼 있어요. 설치 때 꺼 두었다면 `git lfs install`을 한 번 실행해 주세요.
- MediaPipe 모델 파일(`.task`)은 커밋하지 않고 다운로드 스크립트로 받아요.
- 시연 영상은 레포에 넣지 않고 링크로 남겨요.
