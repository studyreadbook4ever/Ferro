# FERRO Agent CLI

에이전트는 음표를 클릭할 필요 없이 JSON 계획으로 프레이즈·화음·멜로디·클립을 입력할 수 있습니다. 파일을 편집하고 WAV를 만드는 명령은 화면이나 오디오 장치 없이 실행됩니다. `--live`는 실행 중인 FERRO 창의 곡을 대상으로 합니다.

## 먼저 명령과 형식을 확인하기

저장소 루트에서 빌드한 뒤 실행합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
./ferro-agent help
./ferro-agent schema
```

`ferro-agent`를 설치했다면 `./ferro-agent` 대신 `ferro-agent`를 사용합니다. 같은 기능을 `./build/ferro agent ...`로도 실행할 수 있습니다. 현재 실행 파일의 `help`와 `schema`가 명령·필드·범위의 기준입니다.

모든 에이전트 명령의 표준 출력은 JSON입니다. 성공 여부는 `ok`, `inspect`의 곡 정보는 `data`, 프로젝트 revision은 응답의 최상위 `revision`을 확인합니다. `schema`는 최상위 `schema`에 JSON Schema를 반환하며, `apply`는 작업별 결과를 `operations`에 반환합니다. 실패하면 종료 코드도 0이 아닙니다. 실패한 명령의 결과를 다음 단계의 입력으로 사용하지 마세요.

## 설치하기

소스 폴더에서 쓸 때는 빌드 후 `./ferro-agent`를 바로 실행합니다. 다른 폴더에서도 명령 이름으로 호출하려면 다음 두 방법 중 하나를 사용합니다.

```sh
# 현재 소스 폴더의 빌드를 사용하는 CLI와 앱 메뉴 등록
./install-local.sh

# 또는 빌드 결과를 사용자 경로에 별도로 설치
cmake --install build --prefix "$HOME/.local"
```

두 방법 모두 `~/.local/bin/ferro-agent`를 제공합니다. `install-local.sh`로 등록한 CLI는 현재 저장소를 가리키므로 소스를 옮기면 다시 등록합니다. CMake 설치는 `bin/ferro`, `bin/ferro-agent`와 함께 `share/ferro/docs/AGENT_CLI.md`, `share/ferro/examples/agent-four-bars.json`, `share/ferro/examples/Neon-Signal-2011.ferro`를 설치합니다. 설치된 실행 파일에는 쇼케이스 곡이 포함되어 있습니다.

`~/.local/bin`이 `PATH`에 있다면 `ferro-agent help`를 사용합니다. 없다면 `~/.local/bin/ferro-agent help`처럼 전체 경로로 실행하거나, 현재 쉘에서 `export PATH="$HOME/.local/bin:$PATH"`를 실행합니다. CMake 설치 후 아래 곡 만들기 예제를 다른 폴더에서 실행한다면 `./ferro-agent`는 `ferro-agent`로, 계획 파일 경로는 `"$HOME/.local/share/ferro/examples/agent-four-bars.json"`으로 바꾸세요.

## 4마디 곡을 파일로 만들기

아래 명령은 새로운 임시 폴더에 빈 프로젝트를 만들고, 저장소에 포함된 [4마디 계획](../examples/agent-four-bars.json)을 검증·적용한 뒤 WAV로 출력합니다. 각 명령이 성공해야 다음 명령으로 진행합니다. `set -e`는 명령 실패 시 스크립트를 멈춥니다.

```sh
set -e
song_dir="$(mktemp -d)"

./ferro-agent new --project "$song_dir/Neon-Four.ferro"
./ferro-agent inspect --project "$song_dir/Neon-Four.ferro" > "$song_dir/before.json"
song_revision="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["revision"])' < "$song_dir/before.json")"

./ferro-agent validate --project "$song_dir/Neon-Four.ferro" --file examples/agent-four-bars.json
./ferro-agent apply --project "$song_dir/Neon-Four.ferro" --file examples/agent-four-bars.json --expect "$song_revision" --dry-run
./ferro-agent apply --project "$song_dir/Neon-Four.ferro" --file examples/agent-four-bars.json --expect "$song_revision"

./ferro-agent inspect --project "$song_dir/Neon-Four.ferro"
./ferro-agent render --project "$song_dir/Neon-Four.ferro" --output "$song_dir/Neon-Four.wav"
printf 'Project and WAV: %s\n' "$song_dir"
```

Python 3 대신 `song_revision="$(jq -er '.revision' "$song_dir/before.json")"`으로 revision을 읽을 수도 있습니다. 응답을 쉘 코드로 실행하거나 JSON 전체를 명령줄 문자열에 끼워 넣지 마세요. 계획은 파일로 전달하면 인용부호·공백·특수문자가 안전하게 유지됩니다.

dry run의 `revision`은 **적용했을 때의 예상 결과**입니다. 실제 적용에는 앞서 `inspect`에서 읽은 기존 revision을 그대로 사용합니다. 적용에 성공한 뒤에는 다시 검사해 새 revision을 읽으세요.

이 예제는 **126 BPM, Am–F–C–G, 4마디, 여운 2초**입니다. 킥·클랩·하이햇의 1마디 프레이즈를 4마디 클립으로 반복하고, 베이스·화음·후크는 각각 4마디 프레이즈를 사용합니다. Lead가 노래의 중심 멜로디를 연주하며 보컬 녹음은 포함하지 않습니다. 곡 길이는 여운까지 약 9.62초입니다.

GUI로 보려면 `./build/ferro --open "$song_dir/Neon-Four.ferro"`를 실행합니다. 창이 이미 열려 있다면 아래의 `open --live`를 사용합니다. 파일 모드에서 바꾼 내용이 기존 창에 자동 반영되지는 않습니다.

## 실행 중인 창에 적용하기

일반 창은 음악 작업 화면으로 열리고 에이전트 전용 패널은 표시하지 않습니다. 같은 사용자 세션에서 CLI가 실행 중인 창에 연결합니다.

```sh
./ferro-agent status --live
./ferro-agent inspect --live
```

`inspect`에서 현재 곡·트랙·프레이즈·클립과 프레이즈 사용처를 확인하고, 의도한 편집을 계획 파일에 적습니다. 예를 들어 현재 곡의 템포만 127 BPM으로 바꾸려면 다음 계획을 사용합니다.

```sh
cat > tempo-plan.json <<'JSON'
{
  "version": 1,
  "operations": [
    {"op": "song.set", "tempo": 127}
  ]
}
JSON

live_snapshot="$(mktemp)"
./ferro-agent inspect --live > "$live_snapshot"
live_revision="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["revision"])' < "$live_snapshot")"
./ferro-agent apply --live --file tempo-plan.json --expect "$live_revision" --dry-run
./ferro-agent apply --live --file tempo-plan.json --expect "$live_revision"
./ferro-agent inspect --live
```

`apply --live`에는 **방금 검사한 revision을 `--expect`로 전달해야 합니다.** 사용자가 그사이 곡을 편집했다면 명령이 실패합니다. 다시 검사하고 변경 내용을 반영해 계획을 고친 뒤 새 revision으로 진행하세요. 검사나 dry run이 다른 편집을 막아 주는 것은 아닙니다.

사용자가 GUI의 숫자 입력칸을 편집 중이면 실제 `apply`, `open`, `new`, `save`는 **`ui_edit_in_progress`** 오류를 반환하고 입력하던 글자와 포커스를 유지합니다. 사용자가 Enter로 확정하거나 편집을 마칠 때까지 기다린 뒤 다시 검사하세요. `--force`로도 이 보호를 건너뛸 수 없습니다. `inspect`, `status`, `apply --dry-run`은 그대로 사용할 수 있습니다.

적용한 계획 전체는 GUI에서 **Undo 한 번**으로 되돌릴 수 있습니다. 성공해도 자동 재생하지 않습니다. 듣기·정지·저장은 각각 명시적으로 실행합니다.

```sh
./ferro-agent play --live
./ferro-agent stop --live
./ferro-agent save --live
```

아직 저장 경로가 없는 곡은 `save --live --output /absolute/path/My-song.ferro`를 사용합니다. GUI에 적용한 수정은 저장 전까지 일반 편집과 같은 미저장 상태입니다. `render --live --output /absolute/path/Preview.wav`는 현재 곡의 스냅샷을 출력하며 프로젝트 저장을 대신하지 않습니다.

기존 파일을 창에 열 때는 `open --live --project /absolute/path/My-song.ferro`를 사용합니다. 미저장 편집이 있으면 기본적으로 거절하며, 정말 버리고 바꿀 때만 `--force`를 추가합니다. 예상한 곡인지 함께 확인하려면 `--expect REV`도 전달할 수 있습니다.

## 명령 목록

`PROJECT`, `PLAN`, `WAV`에는 실제 경로를 넣고 공백이 있으면 쉘 따옴표로 감쌉니다. `--project`와 `--live`는 대상을 고르는 별도 방식입니다.

| 명령 | 용도 |
| --- | --- |
| `help` / `schema` / `version` | 사용법 / 지원하는 계획 형식 / 앱·프로토콜 버전 확인 |
| `new --project PROJECT [--demo] [--force]` | 빈 곡을 새 파일로 저장; `--demo`면 기본 쇼케이스 곡 사용 |
| `inspect --project PROJECT` | 저장된 곡과 revision 확인 |
| `inspect --live` | 창에서 현재 편집 중인 곡과 revision 확인 |
| `validate --project PROJECT [--file PLAN 또는 --stdin] [--expect REV]` | 프로젝트 검사; 계획을 주면 적용 가능 여부도 검사 |
| `apply --project PROJECT --file PLAN [--expect REV] [--dry-run]` | 검증 후 프로젝트 파일에 한 번에 적용 |
| `apply --live --file PLAN --expect REV [--dry-run]` | 검증 후 창에 한 번에 적용 |
| `apply ... --stdin` | `--file PLAN` 대신 표준 입력으로 계획 전달 |
| `render --project PROJECT --output WAV [--force]` | 저장된 곡 전체를 WAV로 출력 |
| `render --live --output WAV [--force]` | 창의 현재 곡 스냅샷을 WAV로 출력 |
| `open --live --project PROJECT [--force] [--expect REV]` | 창에서 프로젝트 파일 열기 |
| `new --live [--demo] [--force] [--expect REV]` | 창에서 빈 곡 또는 쇼케이스 곡 열기 |
| `save --live [--output PROJECT] [--force]` | 창의 곡 저장 |
| `play --live` / `stop --live` | 창의 재생 시작 / 정지 |
| `status --live` | 연결과 창 상태 확인 |

`--force`는 기존 출력 파일을 덮어쓰거나 미저장 곡을 교체하려는 명시적 선택입니다. 오류가 났다고 자동으로 붙이지 마세요. `--dry-run`과 `validate`는 곡을 바꾸지 않습니다. 표준 입력 예시는 `./ferro-agent apply --project My-song.ferro --stdin < plan.json`입니다.

## 계획의 형식과 좌표

계획은 아래 구조를 갖는 JSON 객체입니다. `operations`는 순서대로 처리하므로 프레이즈를 만든 뒤 같은 계획에서 음과 클립을 추가할 수 있습니다. 모든 작업을 검사한 후 하나의 변경으로 반영하며, 하나라도 잘못되면 전체를 적용하지 않습니다.

```json
{
  "version": 1,
  "operations": [
    {"op": "phrase.create", "track": 3, "name": "Opening", "bars": 1},
    {
      "op": "notes.set",
      "track": 3,
      "phrase": "Opening",
      "notes": [
        {"bar": 1, "step": 1, "pitches": ["C4", "E4", "G4"], "velocity": 96}
      ]
    },
    {"op": "clip.create", "track": 3, "phrase": "Opening", "bar": 1, "bars": 4}
  ]
}
```

위 예시는 빈 프로젝트의 3번 트랙에 1마디 프레이즈를 만들고 도·미·솔을 동시에 입력한 뒤 1마디부터 4마디 동안 반복합니다. 같은 이름의 프레이즈나 겹치는 클립이 이미 있다면 실패하므로 기존 곡에서는 먼저 검사하세요.

| 값 | 기준 |
| --- | --- |
| `track` | 1–6. 기본 악기는 차례로 Kick, Bass, Saw, Lead, Clap, Hats |
| 음의 `bar` | 선택한 프레이즈 내부의 마디, 1부터 시작 |
| 클립의 `bar` | 곡 전체 타임라인의 시작 마디, 1부터 시작 |
| `step` | 각 마디의 1–16. 네 박 시작은 1·5·9·13 |
| `pitches` | MIDI 정수 0–127 또는 음이름 배열. `C4`=60, `F#3`=54, `Bb2`=46 |
| `velocity` | 정수 0–127. 생략하면 새 음은 100, 기존 음은 현재 세기 유지. 0은 음을 남겨 둔 무음 |
| `phrase` | 해당 트랙의 고유한 프레이즈 이름 |
| `clip` | `inspect`나 생성 응답에서 확인한 클립 ID. 임의로 추측하지 않음 |

음표 길이·게이트 필드는 지원하지 않습니다. 한 음은 지정한 시점에 시작하고 악기와 `decay`에 따라 줄어듭니다. 예를 들어 긴 화음을 원하면 트랙이나 음의 `decay`를 조절합니다. `duration` 같은 미지원 필드를 넣으면 실패합니다.

## 작업별 필드

필수 필드 외의 설정은 필요한 것만 보내세요. 알 수 없는 키와 잘못된 자료형은 오류이며 숫자를 문자열로 보내지 않습니다.

| `op` | 필드와 의미 |
| --- | --- |
| `song.set` | `title`, `tempo`(40–300 BPM), `bars`(1–256), `tail`(0–8초), `output`/`delay`/`bass_duck`(0–100), `click`(불리언) 중 바꿀 항목 |
| `track.set` | 필수 `track`; 선택 `name`, `instrument`(`Kick`/`Bass`/`Saw`/`Lead`/`Clap`/`Hats`), `level`(0–150), `pan`(−100–100), `tone`/`decay`/`drive`(0–100), `mute`/`solo`(불리언) |
| `phrase.create` | `track`, `name`, `bars`(1–16). 새 이름이어야 함 |
| `phrase.clone` | `track`, `phrase`(원본 이름), `name`(새 이름). 음과 음별 설정을 복제 |
| `phrase.resize` | `track`, `phrase`, `bars`; 줄이면서 음이 삭제된다면 `truncate: true`를 명시 |
| `phrase.clear` | `track`, `phrase`. 그 프레이즈의 모든 음을 삭제 |
| `notes.set` | `track`, `phrase`, `notes` 배열. 항목은 `bar`, `step`, `pitches`, 선택 `velocity`, 선택 `sound` |
| `notes.remove` | `track`, `phrase`, `notes` 배열. 항목은 `bar`, `step`, `pitches`; 지정한 음이 실제로 있어야 함 |
| `clip.create` | `track`, `phrase`, `bar`, `bars`. 클립 ID는 자동 생성 |
| `clip.set` | 필수 `track`, `clip`; 선택 `bar`, `bars`, `phrase` |
| `clip.remove` | `track`, `clip` |
| `song.fit` | 추가 필드 없음. 마지막 클립 끝에 곡 길이를 맞춤; 최소 1마디 |

CLI에서 샘플 불러오기·교체는 현재 지원하지 않습니다. `track.set`에 샘플 필드를 넣으면 실패합니다. **`instrument`를 명시하면 GUI의 `Use instrument`처럼 샘플 사용을 해제하고 지정한 내장 악기로 돌아갑니다.** 샘플을 유지하며 소리만 조절하려면 `instrument`를 생략하고 level·tone 등 필요한 항목만 보내세요.

### 기존 음과 소리 설정 바꾸기

`notes.set`은 지정한 위치의 지정한 음높이만 추가하거나 갱신합니다. 같은 스텝의 다른 화음 구성음과 다른 스텝은 유지됩니다. 같은 `notes` 배열 안에서 한 위치·음높이를 중복 지정하면 실패합니다. 음이름과 MIDI 숫자로 같은 음을 두 번 보내는 것도 중복입니다.

`sound`에는 `level`/`pan`/`tone`/`decay`/`drive`를 UI 단위로 보냅니다. 기존 음에서 `sound`를 생략하면 음별 설정을 유지하며, 일부 키를 보내면 해당 항목만 바꿉니다. `null`은 그 항목을 트랙 기본값으로 되돌립니다. 숫자 `0`은 실제 0으로 설정합니다.

```json
{
  "op": "notes.set",
  "track": 1,
  "phrase": "Kick Pulse",
  "notes": [
    {
      "bar": 1,
      "step": 13,
      "pitches": ["C2"],
      "velocity": 108,
      "sound": {"decay": 60, "tone": null}
    }
  ]
}
```

이 작업은 13번 킥의 decay만 60으로 고정하고 tone은 트랙 값을 따르게 합니다. 다른 기존 override는 유지합니다. `velocity`를 생략하면 기존 음의 세기를 유지하며, 새 음에만 기본값 100을 사용합니다.

### 공유 프레이즈와 독립 변주

하나의 프레이즈를 여러 클립이 사용할 수 있습니다. `notes.set`, `notes.remove`, `phrase.clear`, `phrase.resize`는 그 프레이즈를 사용하는 모든 클립에 영향을 줍니다. `inspect`의 사용처를 먼저 확인하세요. 클립 하나만 바꾸려면 프레이즈를 복제하고 대상 클립의 `phrase`를 새 이름으로 바꾼 뒤 복제본을 편집합니다.

클립의 `bars`는 타임라인에서 재생할 길이이고 프레이즈의 `bars`는 반복 단위입니다. 둘은 별도로 지정합니다. 1마디 프레이즈와 8마디 클립이면 8번 반복하며, 프레이즈보다 짧은 클립이면 클립 끝에서 재생이 잘립니다. 프레이즈 길이를 바꾸면 반복 주기도 달라집니다.

## 범위와 실패 처리

- 고정 4/4박자, 마디당 16스텝, 6트랙입니다.
- 프레이즈는 1–16마디, 트랙당 최대 128개입니다.
- 클립은 트랙당 최대 256개이며 같은 트랙에서는 겹칠 수 없습니다.
- 곡은 1–256마디이며 모든 클립이 곡 끝 안에 있어야 합니다. 새 클립을 뒤에 놓으려면 `song.set`으로 곡을 먼저 늘리세요.
- 곡과 여운을 합쳐 최대 10분입니다. 낮은 템포에서는 256마디보다 먼저 한도에 도달할 수 있습니다.
- 프레이즈·트랙 이름은 최대 80자, 곡 제목은 최대 256자입니다.
- 파일 편집과 저장은 검증 후 원자적으로 교체합니다. 거절된 계획으로 프로젝트 파일의 일부만 바뀌지 않습니다.

revision 불일치는 다시 검사한 뒤 계획을 조정합니다. 겹치는 클립은 의도한 시작 마디·길이를 다시 정합니다. 이름이 없는 프레이즈나 클립은 실제 검사 결과를 사용합니다. 인식하지 못한 키는 `schema`에서 지원 여부를 확인하고 제거하거나 지원되는 작업으로 표현합니다. 미저장 변경·기존 파일 보호 오류는 저장·새 경로·명시적인 덮어쓰기 중 목적에 맞는 방법을 선택합니다.

파일 모드의 실제 `apply`는 GUI에서 열려 있는 것으로 확인된 프로젝트 파일을 수정하지 않습니다. GUI가 바빠서 어떤 파일을 편집 중인지 확인할 수 없는 경우에도 미저장 작업을 보호하기 위해 거절합니다. 실행 중인 곡에는 `inspect --live`와 revision을 사용하는 live 작업 흐름을 사용하고, GUI가 응답하지 않았다면 응답할 때까지 기다린 뒤 다시 검사하세요.

CLI 성공 후에도 `inspect`로 프레이즈·클립·세기·mute/solo를 확인하고 WAV를 들어 보세요. 저장 경로, 적용한 계획, 출력 WAV 경로를 사용자에게 알려 주면 사람이 GUI에서 이어서 작업할 수 있습니다.
