# CatchEye Capture

PLC GPIO 신호를 받아 카메라 프레임을 JPEG로 저장하고, 저장 성공 후 PLC로 완료 GPIO 펄스를 보내는 Raspberry Pi용 캡처 앱이다.

## 목적

- Raspberry Pi 카메라, 이미지, 동영상 입력 처리
- PLC 입력 GPIO 상승 엣지 기반 사진 캡처
- 날짜별 디렉터리에 순번 JPEG 저장
- 저장 완료 GPIO 펄스 출력
- catcheye-studio viewer용 WebSocket 프레임 송출
- HTTP API 기반 수동 캡처, 녹화, RGB 카메라 속성 조회/변경

## 기본 포트

| 기능 | 기본 포트 | 주소 |
| --- | ---: | --- |
| WebSocket | `8080` | `ws://<host>:8080/` |
| HTTP API | `8090` | `http://<host>:8090/api/` |

## 디렉터리 구조

```text
.
├── CMakeLists.txt
├── cmake/
│   └── catcheye-capture-launcher.sh.in
├── docker/
│   ├── amd64/
│   └── arm64/
├── docs/
│   └── API.md
├── include/
│   └── catcheye/
├── scripts/
│   ├── catcheye-capture.service
│   ├── cmake.sh
│   ├── run.sh
│   └── sync-compile-commands.sh
├── src/
│   ├── capture/
│   ├── hardware/
│   ├── logger.cpp
│   └── main.cpp
├── tests/
└── third_party/catcheye-vision-sdk/
```

## 설치

ARM 장비에는 카메라 런타임, GStreamer, OpenCV, spdlog, libgpiod가 필요하다.

```bash
sudo apt update
sudo apt install -y \
  gstreamer1.0-libcamera \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libopencv-dev \
  libspdlog-dev \
  libgpiod-dev \
  gpiod
```

확인:

```bash
gst-inspect-1.0 libcamerasrc
gpioinfo /dev/gpiochip4
```

## 빌드

`third_party/catcheye-vision-sdk`는 `develop` 브랜치 기준으로 맞춘다.

```bash
git submodule update --init --recursive
git -C third_party/catcheye-vision-sdk checkout develop
git -C third_party/catcheye-vision-sdk pull --ff-only
```

amd64 개발 컨테이너:

```bash
docker compose -f docker/amd64/docker-compose.dev.yml build
docker compose -f docker/amd64/docker-compose.dev.yml up -d catcheye-capture-develop-amd64
```

arm64 하드웨어 컨테이너:

```bash
docker compose -f docker/arm64/docker-compose.dev.yml build
docker compose -f docker/arm64/docker-compose.dev.yml up -d catcheye-capture-develop-arm64
```

컨테이너 빌드:

```bash
scripts/cmake.sh configure release
scripts/cmake.sh build release
scripts/cmake.sh install release
scripts/cmake.sh verify release
```

수동 빌드:

```bash
cmake -S . -B build/release-amd64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=OFF \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build/release-amd64 --config Release -- -j "$(nproc)"
```

arm64:

```bash
cmake -S . -B build/release-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCATCHEYE_CAPTURE_ENABLE_LIBCAMERA=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

cmake --build build/release-arm64 --config Release -- -j "$(nproc)"
```

주요 CMake 옵션:

| 옵션 | 설명 |
| --- | --- |
| `CATCHEYE_CAPTURE_ENABLE_LIBCAMERA` | libcamera 입력 백엔드 빌드 여부다. amd64 기본값은 `OFF`, arm64 기본값은 `ON`이다. |
| `CATCHEYE_CAPTURE_BUILD_APP` | 실행 파일 빌드 여부다. 기본값은 `ON`이다. |
| `CATCHEYE_CAPTURE_BUILD_TESTS` | 테스트 빌드 여부다. 기본값은 `OFF`다. |

## 실행

CSI 카메라 + PLC GPIO:

```bash
./install/release/bin/catcheye-capture \
  --camera \
  --ws \
  --gpio-chip /dev/gpiochip4 \
  --trigger-gpio 23 \
  --complete-gpio 24 \
  --capture-dir /home/user/catcheye-capture/captures \
  --recording-dir /home/user/catcheye-capture/recordings
```

설치 패키지 실행 스크립트:

```bash
CATCHEYE_CAPTURE_TRIGGER_GPIO=23 \
CATCHEYE_CAPTURE_COMPLETE_GPIO=24 \
./install/release/scripts/run.sh
```

이미지 파일 입력 테스트:

```bash
./install/release/bin/catcheye-capture \
  --image ./frame.jpg \
  --ws \
  --trigger-gpio 23 \
  --complete-gpio 24
```

## 주요 옵션

| 옵션 | 설명 |
| --- | --- |
| `--camera` | 카메라 입력을 쓴다. 기본 입력이다. |
| `--image <path>` | 이미지 파일 입력을 쓴다. |
| `--video <path>` | 동영상 파일 입력을 쓴다. |
| `--camera-pipeline <pipe>` | GStreamer 카메라 pipeline을 직접 지정한다. |
| `--camera-device <path>` | 카메라 device path를 지정한다. |
| `--camera-width <pixels>` | 카메라 폭을 지정한다. 기본 CSI pipeline은 `2304`다. |
| `--camera-height <pixels>` | 카메라 높이를 지정한다. 기본 CSI pipeline은 `1296`다. |
| `--ws [port]` | WebSocket 송출을 켠다. 포트 생략 시 `8080`이다. |
| `--http-port <port>` | HTTP API 포트다. 기본값은 `8090`이다. |
| `--gpio-chip <path>` | GPIO chip path다. 기본값은 `/dev/gpiochip4`다. |
| `--trigger-gpio <line>` | PLC 캡처 요청 입력 GPIO line이다. `-1`이면 비활성화한다. |
| `--trigger-active-low` | 캡처 요청 입력을 active-low로 해석한다. |
| `--trigger-debounce-ms <ms>` | 캡처 요청 입력 디바운스 시간이다. 기본값은 `200`이다. |
| `--complete-gpio <line>` | PLC 저장 완료 출력 GPIO line이다. `-1`이면 비활성화한다. |
| `--complete-active-low` | 저장 완료 출력을 active-low로 구동한다. |
| `--complete-pulse-ms <ms>` | 저장 완료 출력 펄스 시간이다. 기본값은 `200`이다. |
| `--capture-dir <path>` | JPEG 저장 루트 디렉터리다. 기본값은 `captures`다. |
| `--recording-dir <path>` | Viewer 녹화 MP4 저장 디렉터리다. 기본값은 `recordings`다. |
| `--jpeg-quality <1-100>` | JPEG 저장 품질이다. 기본값은 `95`다. |

## 저장 규칙

저장 파일은 날짜별 디렉터리에 생성된다.

```text
captures/
└── 2026-07-03/
    ├── 142530_015_000001.jpg
    └── 142531_220_000002.jpg
```

파일명은 `HHMMSS_mmm_NNNNNN.jpg` 형식이다. 앱 시작 시 오늘 날짜 디렉터리의 기존 JPEG를 스캔해서 가장 큰 `NNNNNN` 다음 번호부터 저장한다. 앱이나 카메라 런타임이 재시작돼도 같은 날짜 안에서는 sequence를 0부터 다시 쓰지 않는다.

기존 최종 JPEG 경로가 이미 있으면 덮어쓰지 않고 저장 실패로 처리한다. JPEG는 같은 디렉터리의 `.tmp.<pid>.<sequence>.jpg` 임시 파일로 먼저 쓴 뒤 최종 경로로 승격하고, 성공 후 임시 파일을 삭제한다.

동시에 트리거가 들어오면 큐를 만들지 않는다. 저장 중이거나 이미 요청이 대기 중이면 해당 트리거는 무시하고 `ignored_trigger_count`만 증가한다.

## HTTP API

장비 식별 정보 조회:

```bash
curl http://<host>:8090/api/device-info
```

캡처 상태 조회:

```bash
curl http://<host>:8090/api/capture/status
```

수동 캡처 요청:

```bash
curl -X POST http://<host>:8090/api/capture/request
```

Viewer 녹화:

```bash
curl http://<host>:8090/api/recording
curl -X POST http://<host>:8090/api/recording/start
curl -X POST http://<host>:8090/api/recording/pause
curl -X POST http://<host>:8090/api/recording/resume
curl -X POST http://<host>:8090/api/recording/save
curl -X POST http://<host>:8090/api/recording/cancel
```

카메라 속성 조회:

```bash
curl http://<host>:8090/api/rgb-camera/properties
```

카메라 속성 변경:

```bash
curl -X PUT \
  -H 'Content-Type: application/json' \
  -d '{"value": false}' \
  http://<host>:8090/api/rgb-camera/properties/ae-enable
```

자세한 API는 [docs/API.md](docs/API.md)를 봐.
