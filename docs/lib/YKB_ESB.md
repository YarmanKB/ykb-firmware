# YKB ESB

YKB ESB wraps Nordic ESB so the rest of the firmware can use one API on nRF52 and nRF53.

On nRF52 it talks to ESB directly on the application core.

On nRF5340 it can proxy ESB between CPUAPP and CPUNET using IPC/RPC because the radio is on the network core.

## API

```c
int ykb_esb_init(ykb_esb_config_t *config, ykb_esb_callback_t callback);
int ykb_esb_send(ykb_esb_data_t *tx_packet);
int ykb_esb_rpc_start(void);
```

Events:

- `YKB_ESB_EVT_TX_SUCCESS`
- `YKB_ESB_EVT_TX_FAIL`
- `YKB_ESB_EVT_RX`

Modes:

- `YKB_ESB_MODE_PTX`
- `YKB_ESB_MODE_PRX`

## Configuration

`ykb_esb_config_t` contains:

- mode
- user pointer
- base address 0
- base address 1

Payload size is controlled by `CONFIG_ESB_MAX_PAYLOAD_LENGTH`.

The project defaults this to 252 when YKB ESB is enabled.

## MPSL / timeslot

On single-core chips with Bluetooth and ESB sharing the radio, YKB ESB can use YKB Timeslot.

This is selected by `CONFIG_LIB_YKB_ESB_MPSL`.

Do not attempt to run raw ESB and Bluetooth radio access independently. MPSL must arbitrate radio time.

## nRF5340

On nRF5340 CPUAPP, YKB ESB selects the nRF53 app implementation and enables CPUNET support.

The actual ESB radio work happens on CPUNET. CPUAPP communicates over RPC.

## Used by

SplitLink can use YKB ESB as a transport with `CONFIG_SPLITLINK_YKB_ESB`.
