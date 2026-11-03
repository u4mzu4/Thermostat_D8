Room thermostat on ESP32-S3

## Build

ElegantOTA 4.x defaults to the synchronous `WebServer`; this sketch uses `AsyncWebServer`, so the async mode
must be enabled for the library's own sources too (a `#define` in the sketch does not reach them):

```
arduino-cli compile --fqbn esp32:esp32:esp32s3 --build-property "compiler.cpp.extra_flags=-DELEGANTOTA_USE_ASYNC_WEBSERVER=1" --output-dir <dir> .
```

## Self-healing

- Task watchdog (2 min) on the loop task and core-0 idle -> panic + restart.
- Restart after 15 min without a successful Influx write, or 5 min without WiFi.
- The main cycle runs in `loop()` (not a Ticker / esp_timer callback), so the blocking Influx calls are watched.
- After each boot the `resetReason` field (`esp_reset_reason_t`) is written once to `thermostat`.
