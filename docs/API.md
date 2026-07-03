# CatchEye Capture API

CatchEye Capture 런타임 확인과 RGB 카메라 제어용 HTTP API다.

기본 주소:

```text
http://<host>:8090/api
```

## 엔드포인트

| Method | Path | 설명 |
| --- | --- | --- |
| `GET` | `/api/device-info` | 앱 식별 정보 조회 |
| `GET` | `/api/capture/status` | 캡처 런타임 상태 조회 |
| `POST` | `/api/capture/request` | GPIO 없이 다음 프레임 캡처 요청 |
| `GET` | `/api/captures/dates` | 저장된 JPEG 날짜 목록 조회 |
| `GET` | `/api/captures?date=YYYY-MM-DD&limit=100&cursor=<filename>` | 저장된 JPEG 목록 조회 |
| `GET` | `/api/captures/file/<date>/<filename>` | 저장된 JPEG 원본 조회 |
| `GET` | `/api/captures/latest` | 최신 저장 JPEG metadata 조회 |
| `GET` | `/api/recording` | Viewer 녹화 상태 조회 |
| `POST` | `/api/recording/start` | Viewer 녹화 시작 |
| `POST` | `/api/recording/pause` | Viewer 녹화 일시정지 |
| `POST` | `/api/recording/resume` | Viewer 녹화 재시작 |
| `POST` | `/api/recording/save` | Viewer 녹화 저장 |
| `POST` | `/api/recording/cancel` | Viewer 녹화 취소 |
| `GET` | `/api/rgb-camera/properties` | 지원되는 RGB 카메라 속성 조회 |
| `PUT` | `/api/rgb-camera/properties/<key>` | RGB 카메라 속성 변경 |

## `GET /api/device-info`

```bash
curl http://<host>:8090/api/device-info
```

응답 예:

```json
{
  "app": "catcheye-capture",
  "kind": "capture"
}
```

이 엔드포인트는 Guard/Pick/Capture 공통 식별 계약이다. 상태나 기능 목록을 섞지 않는다.

## `GET /api/capture/status`

```bash
curl http://<host>:8090/api/capture/status
```

응답 예:

```json
{
  "app": "catcheye-capture",
  "kind": "capture",
  "trigger_gpio_enabled": true,
  "complete_gpio_enabled": true,
  "busy": false,
  "capture_requested": false,
  "capture_count": 12,
  "ignored_trigger_count": 1,
  "last_saved_path": "/home/user/catcheye-capture/captures/2026-07-03/142530_015_000012.jpg",
  "last_error": ""
}
```

| 필드 | 설명 |
| --- | --- |
| `trigger_gpio_enabled` | PLC 캡처 요청 입력 GPIO 사용 여부 |
| `complete_gpio_enabled` | PLC 저장 완료 출력 GPIO 사용 여부 |
| `busy` | 현재 프레임 저장 처리 중인지 여부 |
| `capture_requested` | 다음 프레임에서 처리할 캡처 요청이 대기 중인지 여부 |
| `capture_count` | 현재 프로세스 실행 중 저장 성공한 JPEG 개수 |
| `ignored_trigger_count` | busy 또는 요청 대기 상태라 무시한 트리거 개수 |
| `last_saved_path` | 마지막 저장 성공 파일 경로 |
| `last_error` | 마지막 오류 메시지 |

## `POST /api/capture/request`

```bash
curl -X POST http://<host>:8090/api/capture/request
```

GPIO 입력 없이 다음 프레임 저장을 요청한다. 응답은 `GET /api/capture/status`와 같은 JSON이다.

## Capture Image API

저장된 JPEG는 `--capture-dir` 아래 `YYYY-MM-DD/*.jpg` 형식만 노출한다. 다른 경로나 확장자는 실패한다.

```bash
curl http://<host>:8090/api/captures/dates
curl 'http://<host>:8090/api/captures?date=2026-07-03&limit=100'
curl http://<host>:8090/api/captures/latest
curl -o frame.jpg http://<host>:8090/api/captures/file/2026-07-03/142530_015_000012.jpg
```

날짜 응답 예:

```json
{
  "storage": {
    "path": "/home/user/catcheye-capture/captures",
    "total_bytes": 250000000000,
    "available_bytes": 180000000000,
    "used_bytes": 70000000000,
    "used_percent": 28.0,
    "capture_bytes": 1234567890,
    "capture_count": 312
  },
  "dates": [
    {
      "date": "2026-07-03",
      "count": 12
    }
  ]
}
```

`storage`는 `--capture-dir`가 올라간 파일시스템 기준이다. `capture_bytes`와 `capture_count`는 이미지 뷰어가 인정하는 `YYYY-MM-DD/HHMMSS_mmm_NNNNNN.jpg` 파일만 합산한다.

목록 응답 예:

```json
{
  "date": "2026-07-03",
  "items": [
    {
      "filename": "142530_015_000012.jpg",
      "date": "2026-07-03",
      "captured_at": "2026-07-03T14:25:30.015+09:00",
      "sequence": 12,
      "size_bytes": 184223,
      "width": 2304,
      "height": 1296,
      "url": "/api/captures/file/2026-07-03/142530_015_000012.jpg"
    }
  ],
  "next_cursor": ""
}
```

`/api/captures/latest`는 같은 image metadata 객체 하나를 반환한다. JPEG 원본 응답은 `Content-Type: image/jpeg`다.

## Recording API

```bash
curl http://<host>:8090/api/recording
curl -X POST http://<host>:8090/api/recording/start
curl -X POST http://<host>:8090/api/recording/pause
curl -X POST http://<host>:8090/api/recording/resume
curl -X POST http://<host>:8090/api/recording/save
curl -X POST http://<host>:8090/api/recording/cancel
```

응답 예:

```json
{
  "state": "recording",
  "active_path": "/home/user/catcheye-capture/recordings/catcheye_capture_20260703_142530.mp4",
  "saved_path": "",
  "error": "",
  "written_frames": 42
}
```

| 필드 | 설명 |
| --- | --- |
| `state` | `idle`, `recording`, `paused` 중 하나 |
| `active_path` | 현재 녹화 중인 MP4 경로 |
| `saved_path` | 마지막 저장 완료 MP4 경로 |
| `error` | 마지막 녹화 오류 메시지 |
| `written_frames` | 현재 녹화에 기록한 프레임 수 |

## WebSocket metadata

`--ws` 실행 시 Studio는 기존 Guard/Pick과 같은 WebSocket 스트림을 받는다. 프레임 metadata의 `metadata` 객체에는 capture 상태가 들어간다.

```json
{
  "app": "catcheye-capture",
  "kind": "capture",
  "capture_count": 12,
  "ignored_trigger_count": 1
}
```

## `GET /api/rgb-camera/properties`

```bash
curl http://<host>:8090/api/rgb-camera/properties
```

응답은 현재 카메라 소스가 노출하는 속성만 JSON 객체로 반환한다.

## `PUT /api/rgb-camera/properties/<key>`

```bash
curl -X PUT \
  -H 'Content-Type: application/json' \
  -d '{"value": false}' \
  http://<host>:8090/api/rgb-camera/properties/ae-enable
```

지원 키:

| 키 | 타입 |
| --- | --- |
| `ae-enable` | boolean |
| `ae-metering-mode` | string |
| `ae-flicker-period` | integer |
| `exposure-time-mode` | string |
| `exposure-time` | integer |
| `exposure-value` | number |
| `analogue-gain-mode` | string |
| `analogue-gain` | number |
| `awb-enable` | boolean |
| `awb-mode` | string |
| `af-mode` | string |
| `lens-position` | number |
| `brightness` | number |
| `contrast` | number |
| `saturation` | number |
| `sharpness` | number |
| `gamma` | number |

실패하면 `{"error":"..."}` 형식으로 반환한다.
