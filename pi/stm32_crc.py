import sys

POLY = 0x04C11DB7

def stm32_crc32(data: bytes) -> int:
    crc = 0xFFFFFFFF

    if len(data) % 4 != 0:
        raise ValueError("image size must be a multiple of 4 bytes")

    for i in range(0, len(data), 4):
        # stm32 crc unit consumes 32-bit words, big-endian within a word
        word = int.from_bytes(data[i:i+4], 'little')
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF

    return crc

if __name__ == "__main__":
    path = sys.argv[1]
    with open(path, 'rb') as f:
        data = f.read()

    print(f"size: {len(data)} bytes")
    print(f"crc32: 0x{stm32_crc32(data):08X}")