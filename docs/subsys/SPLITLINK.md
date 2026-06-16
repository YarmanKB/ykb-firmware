# SplitLink

SplitLink is the transport layer between split keyboard halves.

It provides a small generic send/receive API and a sync layer for keyboard values, settings-adjacent state, battery state and backlight script slots.

## Layers

SplitLink has two layers:

- Transport - Bluetooth or ESB.
- Sync protocol - YKB protocol framing over the transport.

The transport only moves packets. The sync layer decides what those packets mean.

## Transports

Supported transports:

- `SPLITLINK_BT` - custom BLE GATT service.
- `SPLITLINK_YKB_ESB` - nRF ESB using `lib/ykb_esb`. **Still in development and currently doesn't work.**

Right now (for split wireless keyboards) Bluetooth is used for both to connect to the host as a HID keyboard and to communicate between the halves. In theory we can leverage Bluetooth for HID and ESB for inter-keyboard communication and by that possibly lowering the latency.

For Bluetooth:

- master normally uses `SPLITLINK_BT_CENTRAL`
- slave normally uses `SPLITLINK_BT_PERIPHERAL`

For ESB:

- master normally uses `SPLITLINK_YKB_ESB_PRX`
- slave normally uses `SPLITLINK_YKB_ESB_PTX`

## Sync roles

`SPLITLINK_SYNC_MASTER` runs on the half that talks to the host.

`SPLITLINK_SYNC_SLAVE` runs on the remote half.

The master receives slave key values and injects them into KBHandler as the second part of the global key array. The slave scans local keys and sends value batches to the master.

## Data synchronized

Current sync protocol handles:

- Slave key values.
- Slave key reset/disconnect.
- Settings payload from master to slave.
- Slave battery state.
- Backlight script manifest.
- Backlight script slot transfer.
- Optional RTT benchmark.

Large payloads use `lib/ykb_protocol` packetization. Bluetooth can use out-of-order tracking.

## Public API

```c
int splitlink_register_transport(const struct splitlink_transport_api *api);
bool splitlink_is_ready(void);
int splitlink_send(uint8_t *data, size_t data_len);
```

Transport implementations call:

```c
void splitlink_notify_connected(void);
void splitlink_notify_disconnected(void);
void splitlink_notify_receive(uint8_t *data, size_t data_len);
```

Subsystems can subscribe to sync events:

```c
SPLITLINK_SYNC_CB(name) = {
    .on_connected = ...,
    .on_disconnected = ...,
    .on_slave_battery_state = ...,
};
```

## Pair ID

SplitLink Bluetooth UUIDs are generated from the pair ID. If `SPLITLINK_PAIR_ID` is not set, the build uses a development fallback.

That is fine for local development. It is not fine for real devices because every build using the fallback shares the same split identity.

This is done entirely so that each master/slave pair would have unique common pair id and user would not have any issues with having multiple pairs of the split keyboards in the room.

## Important Kconfig

- `CONFIG_SPLITLINK_BT_PACKET_LENGTH` - packet size for BLE SplitLink.
- `CONFIG_SPLITLINK_BT_RX_QUEUE_DEPTH` - receive queue depth.
- `CONFIG_SPLITLINK_OUT_OF_ORDER_TRACK` - enables bitmap tracking for out-of-order packets.
- `CONFIG_SPLITLINK_BENCHMARK` - logs RTT benchmark packets.
- `CONFIG_SPLITLINK_YKB_ESB_ALIVE_TIMEOUT` - ESB disconnect timeout.
