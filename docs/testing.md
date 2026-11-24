# Testing

## The examples

| Example | What it shows |
|---|---|
| `basic` | timers, promises, uncaught errors that don't kill the loop |
| `network` | udp, posix-socket and WebSocket against an echo server, plus idle CPU |
| `bench` | a few workloads with wall time and CPU cycles |
| `guard` | about 20 hostile scripts against the arena and the limits |
| `bytecode` | host-compiled vs chip-compiled bytecode, broken containers |

Each has `sdkconfig.defaults.<chip>` for S3 and S31; `basic`, `bench` and `network` also for P4.

## esp-emu

No board? [esp-emu](https://github.com/espressif/esp-emulator) runs these fine. Build, merge, run:

```sh
cd examples/guard
idf.py --preview set-target esp32s31 build      # --preview on every idf.py call for S31
cd build && esptool --chip esp32s31 merge-bin --format raw -o merged.bin @flash_args
esp-emu --chip esp32s31 --firmware merged.bin --psram-size 16M --exit-on "GUARD PASS|GUARD FAIL"
```

For `network`, start the echo server first and give esp-emu a network:

```sh
python3 examples/network/tools/echo_server.py &     # UDP 9000, TCP 9001, WS 9002
esp-emu ... --net user,net=10.0.2.0/24 --exit-on "network: done"
```

esp-emu has a soft AP (`myssid` / `mypassword`) and the host is `10.0.2.1`; the example's
`sdkconfig.defaults` already points there. Change both for real hardware.

## What esp-emu won't tell you

- **Timing.** It's faster than real chips, so a timing that looks fine there can be worse on
  silicon.
- **Sleep.** Light sleep and idle CPU don't show up. Measure task CPU with run-time stats instead
  (`examples/network` has a `taskTimes()` helper).

## Gotchas

- After adding a Kconfig option, an existing build dir can keep a stale `sdkconfig.h`. Run
  `idf.py reconfigure`.
- If the Mac sleeps mid-run, esp-emu's `--timeout` doesn't fire. Kill it and run again.
