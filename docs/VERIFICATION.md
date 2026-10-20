# FERRO 검증 안내

이 문서는 프로젝트 v3의 화음·타임라인 편집, 전체 곡 렌더링, 오디오 재생과 워커 생명주기를 확인하는 방법을 설명합니다. 빌드에 필요한 Linux 패키지는 [README의 설치 안내](../README.md)를 참고하세요. 아래 명령은 저장소 최상위 폴더에서 실행합니다.

2026-10-20 화음 지원 변경 후 Arch Linux / GNU C++ 16.1.1에서 Release 빌드와 CTest `engine`·`gui` **2/2가 통과했으며 총 12.17초**가 걸렸습니다. 같은 칸의 화음 입력·개별 수정·삭제·실행 취소, 무음 칸으로 음높이 이동, v3 저장·복원, 합성·샘플 화음 및 워커 전달을 검사했습니다. 실행 중인 사용자 세션과 분리하도록 테스트에는 별도 `XDG_DATA_HOME`을 사용했습니다.

같은 빌드로 v2 `examples/First-song.ferro`를 출력한 WAV는 제공된 `First-song.wav`와 바이트 단위로 일치했습니다. v1 `examples/Foundry-200.ferro`의 WAV도 변경 전 실행 파일로 출력한 WAV와 바이트 단위로 일치했습니다. 두 비교 모두 SHA-256이 같았습니다.

2026-10-19 공개 직전에는 게시할 파일만 별도 폴더에 복사해 Linux / GNU C++ 16.1.1 환경에서 Release 빌드를 새로 만들었습니다. 빌드에 성공했고 CTest의 `engine`·`gui` **2/2가 통과했으며 총 3.57초**가 걸렸습니다. 이전 빌드나 제외된 개발 파일에 의존하지 않는 상태에서 확인한 결과입니다. 이 날짜의 결과는 v2 기준이며 이후 추가된 화음 편집의 검증 결과를 뜻하지 않습니다.

같은 환경에서 `examples/First-song.ferro`를 새 WAV로 렌더했습니다. 제공된 `First-song.wav`와 SHA-256이 일치했고, 48 kHz·스테레오·PCM24, 1,632,000프레임·34초, 마지막 스테레오 샘플 0을 확인했습니다.

## 빌드와 자동 테스트

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

CTest는 `engine`과 `gui` 두 테스트를 실행합니다. `engine`은 프로젝트·DSP·오디오 콜백·워커 프로세스를 검사하고, `gui`는 Qt의 offscreen 플랫폼에서 실제 마우스·키보드 이벤트로 편집 동작을 검사합니다. 두 테스트가 모두 통과하면 `100% tests passed, 0 tests failed out of 2`가 표시됩니다. 오디오 콜백 테스트는 출력 장치 없이 실행할 수 있습니다.

각 테스트를 직접 실행하거나 GUI 검사 화면을 저장하려면 다음 명령을 사용합니다.

```bash
./build/ferro --self-test
QT_QPA_PLATFORM=offscreen ./build/ferro --smoke-gui --screenshot /tmp/ferro-preview.png
```

`--self-test`의 성공 출력은 `FERRO tests: 0 failures`입니다. GUI 검사는 편집 동작을 실행한 뒤 검사 화면을 저장합니다.

## 테스트가 확인하는 계약

- [프로젝트 테스트](../tests/project_tests.cpp): v3 직렬화 왕복과 v2 단음 프로젝트 읽기, 빈 곡, 수치·타입·길이 한계, 프레이즈·클립 개수 한계, 중복 ID, 잘못된 참조, 같은 트랙의 겹침, 곡 밖 배치를 검사합니다. 같은 스텝의 여러 음이 세기를 유지하며 동시에 예약되는지, 다중 마디 프레이즈의 반복과 빈 구간, 마지막 반복의 절단, 소수 BPM의 절대 프레임 계산, 다른 트랙 편집 시 이벤트 ID 유지, 곡 길이와 출력 꼬리도 확인합니다.
- 프로젝트 테스트의 v1 변환 검사: 구버전의 저장 음과 실제 재생 이벤트, 제목·템포·마스터·음색·mute/solo·샘플 경로를 비교합니다. 상대 샘플 경로의 저장·복원, 잘못된 프로젝트 저장 시 기존 파일 보존, 손상된 JSON과 16 MiB 초과 입력을 거절할 때 현재 프로젝트 보존도 검사합니다. 구버전 fixture는 테스트 안에서 생성합니다.
- [DSP 테스트](../tests/dsp_tests.cpp): 클립 시간과 반복을 따르는 합성, 결정성·유한성, 악기 선택, 팬·duck·mute/solo, 끝의 소리가 앞부분으로 감기지 않는 동작, 효과음 꼬리, 무음, 샘플 피치 변환·계산 한도와 24비트 WAV 출력을 확인합니다.
- [오디오 테스트](../tests/audio_tests.cpp): 실제 콜백 구현을 호출해 버퍼 게시·회수, 마디 경계 교체, crossfade, 새 세션 무효화, 반영된 템포, 끝에서 정지, 선택적 Loop, 절대 위치 탐색을 검사합니다. 제어 스레드와 콜백의 동시 접근은 아래 ThreadSanitizer 검사에서도 확인합니다.
- [프로세스 테스트](../tests/supervisor_tests.cpp): 실제 `--worker` 자식 프로세스로 충돌·멈춤 회수, 최신 작업 재시도, 오래된 결과 폐기, graceful reload, recycling, 종료 뒤 자식 회수를 확인합니다.
- [GUI 검사](../src/window.cpp): 클립 생성·이동·복사·반복 길이 조절·독립 분리, 피아노 롤 음 입력·삭제, 실행 취소와 저장 왕복을 실제 Qt 이벤트로 확인합니다. 같은 칸의 화음 추가·선택·개별 삭제, 선택한 음만 수정하는 동작, 같은 음높이로 겹치는 변경의 거절도 확인합니다. Ctrl 클릭만 하고 놓을 때의 겹침 방지, 눈금 더블클릭, 긴 이름, 미배치 프레이즈 보존과 재삽입, 렌더 대기 중 위치 탐색도 검사합니다.

## 메모리와 스레드 검사

AddressSanitizer와 UndefinedBehaviorSanitizer를 사용하는 별도 빌드입니다.

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DFERRO_SANITIZE=ON
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure
```

오디오 제어 스레드와 콜백의 경합 검사는 C++ 컴파일러의 ThreadSanitizer로 별도 실행합니다. 스크립트는 임시 폴더에 오디오 테스트 실행 파일을 만들고 종료 시 정리합니다.

```bash
./tests/run-tsan.sh
```

Sanitizer 실행에는 해당 컴파일러의 sanitizer 런타임이 필요합니다. 결과를 공유할 때는 운영체제·컴파일러 버전과 전체 오류 출력을 함께 기록하세요.

## 예제 프로젝트와 WAV 출력

다음 명령은 제공된 예제 파일을 덮어쓰지 않고 임시 폴더에 새 데모와 WAV를 만듭니다.

```bash
./build/ferro --write-demo /tmp/ferro-demo.ferro
./build/ferro --render /tmp/ferro-demo.ferro --output /tmp/ferro-demo.wav
./build/ferro --render examples/First-song.ferro --output /tmp/ferro-first-song.wav
./build/ferro --render examples/Foundry-200.ferro --output /tmp/ferro-legacy.wav
```

새 데모는 120 BPM·16마디와 2초 꼬리로 구성됩니다. 예상 WAV는 **34초, 1,632,000프레임, 48 kHz, 스테레오, 24비트 PCM**입니다. 마지막 명령은 구버전 v1 세션의 읽기와 변환을 확인합니다. 렌더 명령은 입력 세션을 수정하지 않습니다.

전체 곡 출력은 타임라인을 한 번 재생한 결과에 꼬리를 더합니다. GUI의 Loop 설정은 출력 반복 횟수를 바꾸지 않습니다. 이전 `--repeat` 옵션을 주면 오류로 안내합니다.

## 실제 장치와 수동 확인

```bash
./build/ferro --probe-audio
./run.sh
```

`--probe-audio`는 기본 출력 장치에서 약 5초간 무음을 재생하며 장치 이름, 경과 시간, 진행 프레임, XRUN과 최대 콜백 CPU 부하를 JSON으로 출력합니다. 장치를 열지 못하면 오류로 종료합니다.

GUI에서는 다음 동작을 직접 확인할 수 있습니다.

1. Loop를 끄고 데모를 재생해 곡 끝에서 멈추는지 확인합니다. Loop를 켜면 곡 끝에서 다시 재생되는지 비교합니다.
2. 빈 곡에 클립을 만들고 몸통 이동, 오른쪽 끝 반복, Ctrl 드래그 복사, `Make independent` 후 음 편집을 확인합니다.
3. 재생 중 템포와 곡 길이를 바꾸고 눈금으로 탐색해 새 렌더가 준비된 뒤 목표 위치에서 재생되는지 확인합니다.
4. WAV를 출력해 파일의 길이와 실제 청취 결과를 확인합니다. 메트로놈을 출력에 넣지 않으려면 Click을 끕니다.
5. `Ctrl+R`로 워커를 교체하고, 앱을 닫은 뒤 해당 실행에서 생성한 워커 자식이 남지 않는지 확인합니다.
6. Saw 또는 Lead의 한 클립에서 같은 1번 스텝에 C4·E4·G4 / MIDI 60·64·67을 입력합니다. 세 음이 모두 남고 동시에 울리는지, 가운데 E4만 선택해 세기를 바꾸거나 우클릭으로 삭제해도 나머지 두 음이 유지되는지 확인합니다. 실행 취소 후 저장·다시 열기로 화음이 보존되는지도 확인합니다.
7. 화음이 있는 트랙에 샘플을 가져와 기존 음높이가 유지되는지 확인합니다. 단음만 있는 트랙에서는 음높이가 MIDI 60으로 맞춰지는 기존 동작과 비교합니다.

실제 장치 결과에는 선택한 PortAudio 장치, 재생 시간과 XRUN을 함께 기록하세요. 무음 장치 점검과 오프라인 테스트만으로 청취 음질, 모든 장치에서의 무중단 재생이나 최대 길이 곡의 즉시 편집 반영을 판단할 수는 없습니다.

## 2026-10-19 개발 환경의 이전 검증 결과

아래는 타임라인 개편 당시 **Arch Linux 개발 환경에서 수행한 결과**입니다. 공개 직전 새 빌드 결과는 문서 첫머리에 따로 기록했습니다. 실행 시간과 장치 결과는 환경에 따라 달라집니다.

| 항목 | 당시 결과 |
| --- | --- |
| 프로젝트 모델 단독 테스트 | 실패 0개 |
| DSP 단독 테스트 | 실패 0개 |
| 전체 Release 빌드 | 경고 없이 완료 |
| Release CTest | 2/2 통과, 4.41초 |
| ASan/UBSan CTest | leak detection·halt-on-error 활성화, 2/2 통과, 15.05초 |
| 오디오 ThreadSanitizer | 통과 |
| 새 데모 WAV | 34초, 48 kHz·stereo·PCM24, peak 0.595256, RMS 0.103264, 클리핑 0개, 마지막 스테레오 샘플 0 |
| 기본 출력 장치 무음 점검 | 5.018초, XRUN 0 |
| 실제 장치의 곡 종료 | 무음 0.5초·24,000프레임 후 재생 정지, 최종 위치 유지, peak 0 |
| 데스크톱 종료 | 정상 GUI를 창 닫기로 종료한 뒤 워커 2개 회수 |
| 구버전 호환성 | 제공된 `Foundry-200.ferro`를 변경하지 않고 WAV 출력 |

이전 1.0의 A–D 패턴·자동 반복·반복 횟수 출력 검증은 v2 동작의 통과 근거로 사용하지 않습니다.
