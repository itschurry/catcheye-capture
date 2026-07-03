# CatchEye Capture API

CatchEye Capture 런타임 확인과 RGB 카메라 제어용 HTTP API다.

기본 주소:

```text
http://<host>:8090/api
```

## 엔드포인트

| Method | Path | 설명 |
| --- | --- | --- |
| `GET` | `/api/device-info` | 앱 상태와 캡처 상태 조회 |
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
  "kind": "capture",
  "person_roi_alert_disabled": false,
  "capabilities": {
    "viewer": true,
    "monitor": true,
    "roi": false,
    "recording": false,
    "camera_properties": true,
    "camera_geometry": false
  },
  "capture": {
    "app": "catcheye-capture",
    "kind": "capture",
    "trigger_gpio_enabled": true,
    "complete_gpio_enabled": true,
    "busy": false,
    "capture_requested": false,
    "capture_count": 12,
    "ignored_trigger_count": 1,
    "capture_connected": true,
    "roi_enabled": false,
    "recording_enabled": false,
    "last_saved_path": "/home/user/catcheye-capture/captures/2026-07-03/142530_015_000012.jpg",
    "last_error": ""
  }
}
```

| 필드 | 설명 |
| --- | --- |
| `trigger_gpio_enabled` | PLC 캡처 요청 입력 GPIO 사용 여부 |
| `complete_gpio_enabled` | PLC 저장 완료 출력 GPIO 사용 여부 |
| `busy` | 현재 프레임 저장 처리 중인지 여부 |
| `capture_requested` | 다음 프레임에서 처리할 캡처 요청이 대기 중인지 여부 |
| `capture_count` | 저장 성공한 JPEG 개수 |
| `ignored_trigger_count` | busy 또는 요청 대기 상태라 무시한 트리거 개수 |
| `capture_connected` | Studio가 capture 연결 상태로 해석할 수 있는 플래그 |
| `roi_enabled` | capture에서는 항상 `false` |
| `recording_enabled` | capture에서는 항상 `false` |
| `last_saved_path` | 마지막 저장 성공 파일 경로 |
| `last_error` | 마지막 오류 메시지 |

## WebSocket metadata

`--ws` 실행 시 Studio는 기존 Guard/Pick과 같은 WebSocket 스트림을 받는다. 프레임 metadata의 `metadata` 객체에는 capture 상태가 들어간다.

```json
{
  "app": "catcheye-capture",
  "kind": "capture",
  "capture_connected": true,
  "roi_enabled": false,
  "recording_enabled": false,
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
