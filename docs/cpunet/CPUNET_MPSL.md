# MPSL Net Core Image

This is the CPUNET image used when the nRF5340 network core needs to run both the normal Bluetooth controller stack and ESB.

Currently still in development, since it is not that simple.

## Why it exists

On nRF5340, the RADIO peripheral is owned by the network core.

The application core cannot run Bluetooth controller code or ESB code directly. It talks to the network core through IPC/RPC.

Right now (for split wireless keyboards) Bluetooth is used for both to connect to the host as a HID keyboard and to communicate between the halves. In theory we can leverage Bluetooth for HID and ESB for inter-keyboard communication and by that possibly lowering the latency.
