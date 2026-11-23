"""Hash a production v13 fire checkpoint for comparison across producer builds."""

import argparse
import hashlib
import re
import struct
from pathlib import Path


def fnv_update(value, data):
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def comparison_digest(path):
    with path.open("rb") as stream:
        header = stream.read(48)
        magic, version, endian, size, expected = struct.unpack("<16s4Q", header)
        if (magic != b"RISEFIRECHKPT1\0\0" or version != 13 or
                endian != 0x0102030405060708 or size != path.stat().st_size - 48):
            raise ValueError("unexpected checkpoint header")
        prefix = stream.read(144)
        if not re.fullmatch(rb"\x40\x00{7}[0-9a-f]{64}\x40\x00{7}[0-9a-f]{64}", prefix):
            raise ValueError("invalid case or producer id")
        digest = hashlib.sha256(header[:40] + bytes(8) + prefix[:80] + bytes(64))
        checksum = fnv_update(1469598103934665603, prefix)
        remaining = size - len(prefix) - 8
        if remaining < 0:
            raise ValueError("short checkpoint payload")
        while remaining:
            block = stream.read(min(1 << 20, remaining))
            if not block:
                raise ValueError("truncated checkpoint")
            remaining -= len(block)
            digest.update(block)
            checksum = fnv_update(checksum, block)
        binding = stream.read(8)
        if binding != struct.pack("<Q", checksum ^ 0x63b96d44f1a72ec8):
            raise ValueError("invalid payload binding")
        if fnv_update(checksum, binding) != expected or stream.read(1):
            raise ValueError("invalid checkpoint checksum or trailing bytes")
        digest.update(bytes(8))
        return digest.hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path, nargs="+")
    for checkpoint in parser.parse_args().checkpoint:
        try:
            print(comparison_digest(checkpoint), checkpoint, flush=True)
        except (OSError, ValueError, struct.error) as error:
            parser.exit(1, f"{checkpoint}: {error}\n")
