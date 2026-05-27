# Servo Troubleshooting Record - 2026-05-16

This document records the StackChan servo debugging session from 2026-05-16.
It is intended as a practical reference for future regressions.

## Summary

The custom firmware accepted servo HTTP commands, but the head did not visibly
move. The initial implementation talked directly to SCServo over UART1. That
was not enough for the official M5Stack StackChan hardware because the official
board support package performs additional board initialization, especially
servo power enable through the IO expander.

The fix was to use the official `m5stack/StackChan-BSP` library and route servo
commands through `M5StackChan.Motion`.

After the migration:

```text
POST /home -> {"success":true,"ack":true}
POST /move -> {"success":true,"ack":true}
```

Camera-based verification showed large scene changes after yaw commands:

```text
home vs right: changed>15 = 81.0%
home vs left:  changed>15 = 95.2%
right vs left: changed>15 = 92.2%
```

This confirmed that the head was actually moving.

## Symptoms

Before the fix:

- `POST /move` and `POST /home` returned successful HTTP responses.
- Serial logs showed target raw positions being computed and commands being
  sent.
- `/servo/status` reported `ready:true`, but no acknowledgements or feedback:

```json
{
  "ready": true,
  "last_command_ok": false,
  "last_yaw_result": 0,
  "last_pitch_result": 0,
  "yaw": { "ok": false, "position": -1 },
  "pitch": { "ok": false, "position": -1 }
}
```

- Camera snapshots before and after yaw commands had only small image
  differences, around `2%`, which was consistent with no meaningful movement.

## What Was Tried

1. Confirmed that the live device was reachable at `$STACKCHAN_IP`.
2. Sent small movement commands to avoid stressing the mechanism.
3. Added `/servo/status` for diagnostics.
4. Logged command targets, raw servo positions, and SCServo command results.
5. Tried to reduce command latency by shortening the SCServo IO timeout.
6. Reverted experimental `RegWritePos` / `SyncWritePos` behavior back toward
   the previous direct `WritePos` path.
7. Compared camera snapshots to objectively detect motion.
8. Checked official M5Stack StackChan documentation and the
   `m5stack/StackChan-BSP` repository.

## Key Finding

The official StackChan hardware needs board-level BSP initialization. The
official BSP initializes the IO expander and enables servo power:

- StackChan product documentation lists an IO expander and `VM_EN`.
- `StackChan-BSP` calls `io_expander_init()`.
- That initialization calls `setServoPowerEnabled(true)`.
- It then creates official yaw and pitch servo objects and initializes
  `M5StackChan.Motion`.

The direct SCServo implementation configured UART1 but did not perform this
official board initialization path.

## Final Implementation

The firmware now depends on:

```ini
m5stack/StackChan-BSP@^1.1.0
```

`platformio.ini` also needed:

```ini
build_unflags =
    -std=gnu++11
build_flags =
    -std=gnu++17
    -DUART_SCLK_DEFAULT=UART_SCLK_APB
```

Why:

- `StackChan-BSP` uses C++14+ features, so the old Arduino C++11 default was
  not enough.
- BSP 1.1.0 references `UART_SCLK_DEFAULT`, which is not defined by the
  current PlatformIO Arduino ESP32 package in this project. Mapping it to
  `UART_SCLK_APB` preserves the UART clock behavior used by the previous local
  driver.

`main.cpp` now initializes StackChan through the BSP:

```cpp
M5StackChan.begin();
...
M5StackChan.update();
```

`servo_service.cpp` now wraps official motion APIs:

```cpp
M5StackChan.Motion.move(yawAngle, pitchAngle, speed);
M5StackChan.Motion.goHome(speed);
M5StackChan.Motion.moveX(angle, speed);
M5StackChan.Motion.moveY(angle, speed);
```

API degrees are converted to BSP motion units:

```text
10 BSP units = 1 degree
```

Pitch is clamped to `5..85 degrees`, matching the product documentation's
recommendation to avoid extreme vertical angles.

## Verification Procedure

Use the live device IP:

```sh
export STACKCHAN_IP=$STACKCHAN_IP
```

Basic command check:

```sh
curl -sS -X POST "http://$STACKCHAN_IP/home"
curl -sS -X POST "http://$STACKCHAN_IP/move" \
  -H "Content-Type: application/json" \
  -d '{"x":45,"y":20,"speed":60}'
curl -sS "http://$STACKCHAN_IP/servo/status"
```

Expected result:

```json
{"success":true,"ack":true}
```

Camera-based movement check:

```sh
base=/tmp/stackchan_servo_test
mkdir -p "$base"

curl -sS -X POST "http://$STACKCHAN_IP/home"
sleep 2
curl -sS -o "$base/home.jpg" "http://$STACKCHAN_IP/snapshot"

curl -sS -X POST "http://$STACKCHAN_IP/move" \
  -H "Content-Type: application/json" \
  -d '{"x":45,"y":20,"speed":60}'
sleep 3
curl -sS -o "$base/right.jpg" "http://$STACKCHAN_IP/snapshot"

curl -sS -X POST "http://$STACKCHAN_IP/move" \
  -H "Content-Type: application/json" \
  -d '{"x":-45,"y":20,"speed":60}'
sleep 3
curl -sS -o "$base/left.jpg" "http://$STACKCHAN_IP/snapshot"

curl -sS -X POST "http://$STACKCHAN_IP/home"
```

Compare the images with a simple Pillow script:

```py
from pathlib import Path
from PIL import Image, ImageChops, ImageStat

base = Path("/tmp/stackchan_servo_test")
imgs = {p.stem: Image.open(p).convert("RGB") for p in base.glob("*.jpg")}

for a, b in [("home", "right"), ("home", "left"), ("right", "left")]:
    diff = ImageChops.difference(imgs[a], imgs[b])
    stat = ImageStat.Stat(diff)
    mean = sum(stat.mean) / 3
    rms = (sum(v * v for v in stat.rms) / 3) ** 0.5
    changed = sum(1 for px in diff.convert("L").getdata() if px > 15)
    total = imgs[a].size[0] * imgs[a].size[1]
    print(f"{a} vs {b}: mean={mean:.2f} rms={rms:.2f} changed>15={changed / total:.1%}")
```

When the servo is not moving, changed pixels were only around `2%`.
After the BSP migration, changed pixels were above `80%`.

## Notes For Future Debugging

- Do not treat `ack:false` alone as proof of wiring failure. Return behavior can
  depend on servo configuration.
- For official StackChan hardware, always preserve `M5StackChan.begin()` unless
  the replacement code explicitly handles IO expander initialization and servo
  power enable.
- Prefer `M5StackChan.Motion` for new movement behavior.
- Keep Y-axis motion inside `5..85 degrees` unless intentionally testing
  calibration.
- Use camera snapshots for remote movement verification when nobody is watching
  the device directly.

## References

- M5Stack StackChan product documentation:
  <https://docs.m5stack.com/en/stackchan>
- M5Stack StackChan-BSP:
  <https://github.com/m5stack/StackChan-BSP>
- Official Arduino servo example:
  <https://docs.m5stack.com/ja/arduino/stackchan/servo>
