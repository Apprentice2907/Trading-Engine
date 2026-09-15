#!/usr/bin/env python3
"""Generate adversarial seed corpus for fuzz_decoder."""

import struct, os, pathlib

OUT = pathlib.Path(__file__).parent

def write(name, data):
    (OUT / name).write_bytes(data)

# Constants (from AngelConstants in market_data.hpp)
PACKET_LTP        = 51
PACKET_QUOTE      = 147
PACKET_SNAP_QUOTE = 347

def base_ltp():
    buf = bytearray(PACKET_LTP)
    buf[0] = 1  # MODE_LTP
    buf[1] = 1  # EXCH_NSE_CM
    buf[2:27] = b'3045\x00' + b'\x00'*20  # token
    # seq (8 bytes @ 27) = 1
    struct.pack_into('<Q', buf, 27, 1)
    # ts_ms (8 bytes @ 35) = 1710000000000
    struct.pack_into('<q', buf, 35, 1710000000000)
    # ltp (8 bytes @ 43) = 83050
    struct.pack_into('<q', buf, 43, 83050)
    return buf

# Seed 1: Minimal valid LTP
write("ltp_valid.bin", base_ltp())

# Seed 2: LTP length exactly PACKET_LTP - 1 (must return false)
b = base_ltp()[:PACKET_LTP-1]
write("ltp_short_by_1.bin", b)

# Seed 3: Mode = 0 (invalid) — should return false
buf = bytearray(PACKET_LTP)
buf[0] = 0
write("mode_zero.bin", buf)

# Seed 4: Mode = 4 (depth, not supported) — should return false
buf = bytearray(PACKET_LTP)
buf[0] = 4
write("mode_depth.bin", buf)

# Seed 5: Mode = 0xFF — should return false
buf = bytearray(PACKET_LTP)
buf[0] = 0xFF
write("mode_ff.bin", buf)

# Seed 6: Token field = all 9s (25 bytes), no null terminator
buf = base_ltp()
buf[2:27] = b'9' * 25
write("token_all_nines.bin", buf)

# Seed 7: Token = 10 digits (should truncate at 9)
buf = base_ltp()
buf[2:27] = b'1234567890\x00' + b'\x00'*14
write("token_10digits.bin", buf)

# Seed 8: Negative timestamp
buf = base_ltp()
struct.pack_into('<q', buf, 35, -1)
write("ts_negative.bin", buf)

# Seed 9: INT64_MAX timestamp
buf = base_ltp()
struct.pack_into('<q', buf, 35, (1 << 63) - 1)
write("ts_max.bin", buf)

# Seed 10: INT64_MIN timestamp
buf = base_ltp()
struct.pack_into('<q', buf, 35, -(1 << 63))
write("ts_min.bin", buf)

# Seed 11: Quote mode, exactly PACKET_QUOTE bytes
buf = bytearray(PACKET_QUOTE)
buf[0] = 2  # MODE_QUOTE
buf[1] = 1
buf[2:27] = b'3045\x00' + b'\x00'*20
struct.pack_into('<Q', buf, 27, 1)
struct.pack_into('<q', buf, 35, 1710000000000)
struct.pack_into('<q', buf, 43, 83050)
struct.pack_into('<Q', buf, 51, 100)   # last_qty
struct.pack_into('<Q', buf, 67, 50000) # volume
write("quote_valid.bin", buf)

# Seed 12: Quote mode, PACKET_QUOTE - 1 (must fail)
write("quote_short.bin", bytes(buf[:PACKET_QUOTE-1]))

# Seed 13: SnapQuote mode, minimum valid
buf = bytearray(PACKET_SNAP_QUOTE)
buf[0] = 3  # MODE_SNAP_QUOTE
buf[1] = 1
buf[2:27] = b'3045\x00' + b'\x00'*20
struct.pack_into('<Q', buf, 27, 2)
struct.pack_into('<q', buf, 35, 1710000000000)
struct.pack_into('<q', buf, 43, 83050)
struct.pack_into('<Q', buf, 51, 100)
struct.pack_into('<Q', buf, 67, 50000)
struct.pack_into('<Q', buf, 149, 500)   # best_bid_qty
struct.pack_into('<q', buf, 157, 83045) # best_bid_price
struct.pack_into('<Q', buf, 249, 600)   # best_ask_qty
struct.pack_into('<q', buf, 257, 83055) # best_ask_price
write("snap_valid.bin", buf)

# Seed 14: SnapQuote mode, PACKET_SNAP_QUOTE - 1 (must fail)
write("snap_short.bin", bytes(buf[:PACKET_SNAP_QUOTE-1]))

# Seed 15: Unknown exchange type
buf = base_ltp()
buf[1] = 99
write("unknown_exchange.bin", buf)

# Seed 16: Empty input
write("empty.bin", b"")

# Seed 17: Single byte
write("one_byte.bin", b"\x01")

# Seed 18: All zeros, LTP size
write("all_zeros_ltp.bin", bytes(PACKET_LTP))

# Seed 19: All 0xFF
write("all_ff.bin", bytes([0xFF] * PACKET_LTP))

print(f"Generated {len(list(OUT.glob('*.bin')))} seed files in {OUT}")
