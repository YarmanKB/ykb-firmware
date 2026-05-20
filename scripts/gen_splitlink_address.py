#!/usr/bin/env python3

import argparse
import hashlib
from pathlib import Path


def derive_digest(pair_id: str) -> bytes:
    return hashlib.sha256(pair_id.encode("utf-8")).digest()


def derive_esb_address(pair_id: str) -> bytearray:
    address = bytearray(derive_digest(pair_id)[:8])

    # ESB base addresses are 4 + 4 bytes here. Avoid an all-zero address and
    # keep the first byte non-zero for easier debugging.
    if all(byte == 0 for byte in address):
        address[0] = 0xA5
    elif address[0] == 0:
        address[0] = 0xA5

    return address


def derive_uuid(seed: str) -> bytearray:
    digest = bytearray(hashlib.sha256(seed.encode("utf-8")).digest()[:16])
    digest[6] = (digest[6] & 0x0F) | 0x40
    digest[8] = (digest[8] & 0x3F) | 0x80
    return digest


def format_bytes(values: bytearray) -> str:
    return ", ".join(f"0x{byte:02X}" for byte in values)


def generate_esb_header(pair_id: str) -> str:
    address = derive_esb_address(pair_id)
    bytes_literal = format_bytes(address)
    return f"""#ifndef GENERATED_SPLITLINK_ADDRESS_H
#define GENERATED_SPLITLINK_ADDRESS_H

#include <stdint.h>

#define GENERATED_SPLITLINK_PAIR_ID "{pair_id}"

static const uint8_t generated_splitlink_esb_address[8] = {{{bytes_literal}}};

#endif // GENERATED_SPLITLINK_ADDRESS_H
"""


def generate_bt_header(pair_id: str) -> str:
    svc_uuid = derive_uuid(f"{pair_id}:svc")
    tx_uuid = derive_uuid(f"{pair_id}:tx")
    rx_uuid = derive_uuid(f"{pair_id}:rx")
    return f"""#ifndef GENERATED_SPLITLINK_BT_IDENTITY_H
#define GENERATED_SPLITLINK_BT_IDENTITY_H

#include <stdint.h>

#define GENERATED_SPLITLINK_PAIR_ID "{pair_id}"

#define GENERATED_SPLITLINK_BT_UUID_SVC_VAL {format_bytes(svc_uuid)}
#define GENERATED_SPLITLINK_BT_UUID_TX_VAL {format_bytes(tx_uuid)}
#define GENERATED_SPLITLINK_BT_UUID_RX_VAL {format_bytes(rx_uuid)}

#endif // GENERATED_SPLITLINK_BT_IDENTITY_H
"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pair-id", required=True)
    parser.add_argument("--out-h", required=True)
    parser.add_argument("--mode", choices=("esb", "bt"), default="esb")
    args = parser.parse_args()

    if args.mode == "esb":
        header = generate_esb_header(args.pair_id)
    else:
        header = generate_bt_header(args.pair_id)

    Path(args.out_h).write_text(header)


if __name__ == "__main__":
    main()
