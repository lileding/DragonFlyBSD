"""Host-native descriptor packets for VMMFS runtime tests, not a text ABI adapter."""
import struct

HEADER = struct.Struct("=IHHHHIBBHHHHHIQ")
BAR = struct.Struct("=QBB6x")
DOORBELL = struct.Struct("=QQBBB5x")
REGISTER = struct.Struct("=QBBB5x")
CAPABILITY = struct.Struct("=QQIIHBBBBBB")
EXT_CAPABILITY = struct.Struct("=IIHB5x")
assert (HEADER.size, BAR.size, DOORBELL.size, REGISTER.size,
        CAPABILITY.size, EXT_CAPABILITY.size) == (40, 16, 24, 16, 32, 16)

def descriptor(*, bars=(), doorbells=(), configs=(), caps=(), ecaps=(), data=b""):
    header = HEADER.pack(1, 0x1234, 1, 0x1234, 1, 0xff0000, 0, 0,
                         len(doorbells), len(configs), len(caps), len(ecaps),
                         0, len(data), 0)
    return (header + b"".join(BAR.pack(*bar) for bar in bars) +
            bytes((6 - len(bars)) * BAR.size) +
            b"".join(DOORBELL.pack(*item) for item in doorbells) +
            b"".join(REGISTER.pack(*item) for item in configs) +
            b"".join(CAPABILITY.pack(*item) for item in caps) +
            b"".join(EXT_CAPABILITY.pack(*item) for item in ecaps) + data)
