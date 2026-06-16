# ESB Net Core Image

The ESB net core image is for nRF5340 designs that need custom ESB radio behavior on CPUNET.

Currently this image is for testing only.

## Why it exists

On nRF5340, ESB cannot run on CPUAPP because CPUAPP has no direct RADIO peripheral ownership.

YKB ESB solves this by using a CPUAPP wrapper and a CPUNET implementation connected through IPC/RPC.

## When to use it

Use this path when SplitLink or another feature needs ESB on nRF5340.

Do not use it for normal Bluetooth HID. Bluetooth HID uses the MPSL/HCI IPC network image instead.

## API boundary

Application code should still use:

```c
ykb_esb_init(...)
ykb_esb_send(...)
```

The implementation decides whether it is direct ESB or nRF53 RPC-backed ESB.

## Gotchas

CPUAPP and CPUNET builds must agree on RPC IDs and payload sizes.

If packets disappear silently, check both images. Debugging only the application core is not enough for nRF5340 radio issues.
