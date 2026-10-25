#!/usr/bin/env python3
"""Copy an interrupted BirdNETMic PCM16 WAV into a new file with a repaired header.

Never edit the original. Recovery cannot detect audio gaps or reconstruct lost
samples. Remove the card and copy the .part file before using this tool.
"""
import argparse
from pathlib import Path
import struct


def recover(source: Path, destination: Path) -> int:
    with source.open('rb') as inp:
        header = bytearray(inp.read(44))
        if len(header) != 44 or header[:4] != b'RIFF' or header[8:16] != b'WAVEfmt ' or header[36:40] != b'data':
            raise ValueError('Not a BirdNETMic WAV file')
        fmt_size, codec, channels, rate, byte_rate, align, bits = struct.unpack_from('<IHHIIHH', header, 16)
        if (fmt_size, codec, channels, align, bits) != (16, 1, 1, 2, 16) or not 8000 <= rate <= 96000 or byte_rate != rate*2:
            raise ValueError('Expected PCM16 mono WAV at 8-96 kHz')
        size = source.stat().st_size - 44
        size -= size % 2  # Discard an incomplete final sample from a short write.
        if size > 0xffffffd0:
            raise ValueError('WAV exceeds RIFF size limit')
        struct.pack_into('<I', header, 4, 36+size)
        struct.pack_into('<I', header, 40, size)
        # Exclusive creation prevents replacing the original or an earlier recovery.
        with destination.open('xb') as out:
            out.write(header)
            remaining = size
            while remaining:
                block = inp.read(min(remaining, 1024*1024))
                if not block:
                    raise ValueError('Source changed during recovery')
                out.write(block)
                remaining -= len(block)
        return size//2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    try:
        frames = recover(args.source, args.destination)
    except (OSError, ValueError) as error:
        parser.exit(1, f'Recovery failed: {error}\n')
    print(f'Recovered {frames} samples into {args.destination}; original preserved.')


if __name__ == '__main__':
    main()
