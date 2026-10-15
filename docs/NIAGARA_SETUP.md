# Niagara 시스템 만들기 — `NS_PoseHands`

C++ 드라이버(`UPoseNiagaraDriverComponent`)가 매 프레임 아래 User 파라미터 5개를 채워요. Niagara 쪽은 이 값을 **읽기만** 하면 돼요. 상태 판단과 크로스페이드는 C++에서 끝나요 (DESIGN_DECISIONS D13).

| User 파라미터 | 타입 | 값 |
|---|---|---|
| `LeftHandPos`, `RightHandPos` | **Position** | 볼륨 안의 손 월드 위치 (렌더 보간 적용, 재획득 시 스냅) |
| `LeftHandWeight`, `RightHandWeight` | Float | 0~1 크로스페이드 (Tracking 1 · Holding 0.6 · Lost 0) |
| `InteractiveWeight` | Float | 0~1 크로스페이드 (Interactive 1 · Idle 0) |

> 메뉴 이름은 UE 5.x Niagara 에디터 기준 기억이에요. 5.7에서 이름이 다르면 비슷한 항목을 쓰고, 다른 점을 TROUBLESHOOTING에 남겨 주세요.

## 1. 시스템 만들기

1. 에디터에서 `unreal/PoseOscUnreal.uproject`를 열어요.
2. 콘텐츠 브라우저에 `FX` 폴더를 만들어요 (`/Game/FX`).
3. `FX` 폴더에서 우클릭 → **FX → Niagara System** → **New system from selected emitter(s)** → 이미터 목록에서 **Fountain**과 **Hanging Particulates**를 함께 선택 → 이름은 **`NS_PoseHands`** (경로가 정확히 `/Game/FX/NS_PoseHands`여야 액터가 자동으로 찾아요).

## 2. User 파라미터 5개

시스템 에디터의 **Parameters**(또는 **User Parameters**) 패널에서 **User Exposed** 옆 `+`를 눌러 추가해요.

- `+` → **Position** → `LeftHandPos` / 다시 `+` → Position → `RightHandPos`
- `+` → **Float** → `LeftHandWeight`, `RightHandWeight`, `InteractiveWeight`

## 3. 왼손 이미터 (Fountain → `LeftHand`)

1. 이미터 이름을 더블클릭해서 `LeftHand`로 바꿔요.
2. **Emitter Properties**에서 **Local Space가 꺼져 있는지** 확인해요 (손 위치가 월드 좌표라서 꺼져 있어야 해요. 기본값이 꺼짐).
3. **Emitter Update → Spawn Rate → SpawnRate** 값 옆 ▼ → **New Expression** → `User.LeftHandWeight * 300`
4. **Particle Spawn** 섹션의 `+` → **Set new or existing parameter directly** → 생긴 노드의 `+` → **Particles.Position** → 값 옆 ▼ → **Link Inputs → User → LeftHandPos**. 이 노드는 Initialize Particle **아래**에 두세요.
5. 분수처럼 튀지 않게 다듬어요 (권장값):
   - **Gravity Force**: Gravity Z를 `-50` 정도로 (또는 모듈 체크 해제)
   - **Add Velocity in Cone**(또는 Add Velocity): 속도 `30~80`, 각도 넓게
   - **Initialize Particle**: Lifetime `0.5~1.2`, Sprite Size `3~6`, Color를 HDR로 밝게 (예: `R 0.2, G 1.5, B 3.0` → 블룸으로 빛남)

## 4. 오른손 이미터

1. `LeftHand` 이미터 우클릭 → **Duplicate** → 이름 `RightHand`
2. SpawnRate 식을 `User.RightHandWeight * 300`으로, Particles.Position 링크를 `User.RightHandPos`로 바꿔요.
3. 색을 다르게 (예: `R 3.0, G 0.3, B 2.0`)

## 5. 기본 연출 이미터 (Hanging Particulates → `Ambient`)

Idle일 때, 그리고 손이 Lost일 때 공간을 채우는 연출이에요 (D6 "기본 연출").

1. 이름을 `Ambient`로 바꿔요.
2. **Spawn Rate → SpawnRate** → New Expression:
   `(1.0 - User.InteractiveWeight) * 30.0 + (2.0 - User.LeftHandWeight - User.RightHandWeight) * 10.0 + 3.0`
   - Idle → 약 53/초, 양손 Tracking → 3/초, 한 손 Lost → 약 13/초
3. **Shape Location**(Box)의 크기를 볼륨에 맞춰요: `X 100, Y 320, Z 180` (cm)
4. 색은 어둡게 (예: `0.3, 0.4, 0.6`)

## 6. 저장하고 확인

1. `Ctrl+S`로 저장해요. 레벨에 아무것도 배치하지 않아도 돼요.
2. Play → Python 송신부 실행 (`--source synthetic`으로 해도 돼요).
3. Output Log에서 다음을 확인해요.
   - `LogPoseOsc: Driving Niagara system NS_PoseHands` → 연결 성공
   - `Niagara User parameter 'X' is missing` / `has type ...` → 이름·타입 오타 (위 표와 비교)
4. 화면: 손을 따라 파티클이 나오고, 가리면 해당 손 파티클이 옅어졌다가(Holding) 사라지고(Lost), 송신부를 끄면 1초 뒤 Ambient가 공간을 채워요.

디버그 드로잉(박스·구)은 액터의 `NiagaraDriver` 컴포넌트에서 **Draw Debug**를 끄면 사라져요. 시연 녹화 때는 꺼 두세요.

## 커밋

`.uasset`은 Git LFS로 커밋돼요 (`.gitattributes`). 저장한 뒤 알려 주시면 확인하고 커밋할게요.
