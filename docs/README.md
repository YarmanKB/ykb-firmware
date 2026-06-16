# Overview

## Subsystems

Squeezing all the performance out of your MCU with suboptimal solutions.

- [BTConnect](subsys/BT_CONNECT.md) - Bluetooth connection
- [KBHandler](subsys/KB_HANDLER.md) - Main keyboard behavior handling
- [KBSettings](subsys/KB_SETTINGS.md) - Settings / keyboard state storage and state change notifier
- [SplitLink](subsys/SPLITLINK.md) - Handling split keyboards
- [USBConnect](subsys/USB_CONNECT.md) - USB connection
- [VendorHID](subsys/VENDOR_HID.md) - Subsystem/Library for transport with YKB Configurator
- [YKB Backlight](subsys/YKB_BACKLIGHT.md) - LumiscriptVM backlighting subsystem
- [YKB BattSense](subsys/YKB_BATTSENSE.md) - Battery state monitoring subsystem
- [YKB Metrics](subsys/YKB_METRICS.md) - Metrics (scan rate / sending rate / etc) subsystem
- [YKB Power](subsys/YKB_POWER.md) - Subsystem for managing power states and power buttons/LEDs

## Drivers

Giant's shoulders of this project.

- [KScan](drivers/KSCAN.md) - Key scanning behavior driver
- [MUX](drivers/MUX.md) - Multiplexers drivers

## Libraries

- [YKB ESB](lib/YKB_ESB.md) - Wrapper around nRF ESB subsystem to handle both ESB on the same core with the application and ESB on the net core
- [YKB Timeslot](lib/YKB_TIMESLOT.md) - Library for concurrent ESB and BT using timeslot behavior

## Net core for dual core MCUs

For some dual core MCU solutions (such as nRF5340) the RADIO peripheral is only available on a separate core.
For such cases and for custom radio behavior such as multiprotocol behavior or ESB (nRF Enhanced ShockBurst) there are separate custom images.

- [MPSL Net Core Image](cpunet/CPUNET_MPSL.md) - Multiprotocol (ESB + BT) net core image
- [ESB Net Core Image](cpunet/CPUNET_ESB.md) - ESB net core image

## Boards

- [Adding new board](boards/ADDING_NEW_BOARD.md) - Guidelines on adding new boards and devices
- [Skadi](boards/SKADI.md) - Reference nRF5340 split keyboard board
