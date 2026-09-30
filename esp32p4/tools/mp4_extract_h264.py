#!/usr/bin/env python3
"""Extract the avcC record and the first N length-prefixed video samples from an MP4.

Output format (little-endian):
    u32 avcC length | avcC bytes | u32 sample count | (u32 length + bytes) * N

Used to replay a real H.264 stream into the board's video data port.
"""

import struct
import sys


def boxes(data, start, end):
    i = start
    while i + 8 <= end:
        size, typ = struct.unpack(">I4s", data[i:i + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[i + 8:i + 16])[0]
            hdr = 16
        if size < hdr or i + size > end:
            return
        yield typ, i + hdr, i + size
        i += size


def child(data, start, end, typ):
    for t, s, e in boxes(data, start, end):
        if t == typ:
            return s, e
    return None


def descendants(data, start, end, path):
    """Yield (start, end) of the first container following the path."""
    cur = (start, end)
    for name in path:
        nxt = child(data, cur[0], cur[1], name)
        if nxt is None:
            return None
        cur = nxt
    return cur


def main():
    src, dst = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    data = open(src, "rb").read()

    moov = child(data, 0, len(data), b"moov")
    if moov is None:
        print("no moov")
        return 1

    avcc = None
    sizes = []
    for typ, ts, te in boxes(data, moov[0], moov[1]):
        if typ != b"trak":
            continue
        stbl = descendants(data, ts, te, [b"mdia", b"minf", b"stbl"])
        if stbl is None:
            continue
        stsd = child(data, stbl[0], stbl[1], b"stsd")
        if stsd is None:
            continue
        is_video = False
        for _, es, ee in boxes(data, stsd[0] + 8, stsd[1]):
            entry_type = data[es - 4:es]
            if entry_type in (b"avc1", b"avc3", b"hvc1", b"hev1"):
                is_video = True
                for t2, s2, e2 in boxes(data, es + 78, ee):
                    if t2 in (b"avcC", b"hvcC"):
                        avcc = data[s2:e2]
                        break
        if not is_video or avcc is None:
            continue

        stsz = child(data, stbl[0], stbl[1], b"stsz")
        if stsz is None:
            continue
        default = struct.unpack(">I", data[stsz[0] + 4:stsz[0] + 8])[0]
        count = struct.unpack(">I", data[stsz[0] + 8:stsz[0] + 12])[0]
        if default:
            sizes = [default] * count
        else:
            sizes = list(struct.unpack(f">{count}I", data[stsz[0] + 12:stsz[0] + 12 + 4 * count]))

        # Chunk offsets: stco (32-bit) or co64 (64-bit).
        stco = child(data, stbl[0], stbl[1], b"stco")
        co64 = child(data, stbl[0], stbl[1], b"co64")
        offsets = []
        if stco is not None:
            n = struct.unpack(">I", data[stco[0] + 4:stco[0] + 8])[0]
            offsets = list(struct.unpack(f">{n}I", data[stco[0] + 8:stco[0] + 8 + 4 * n]))
        elif co64 is not None:
            n = struct.unpack(">I", data[co64[0] + 4:co64[0] + 8])[0]
            offsets = list(struct.unpack(f">{n}Q", data[co64[0] + 8:co64[0] + 8 + 8 * n]))

        # Sample-to-chunk table.
        stsc = child(data, stbl[0], stbl[1], b"stsc")
        if stsc is None or not offsets:
            continue
        n = struct.unpack(">I", data[stsc[0] + 4:stsc[0] + 8])[0]
        runs = []
        for i in range(n):
            first, per_chunk, _ = struct.unpack(">III", data[stsc[0] + 8 + 12 * i:stsc[0] + 20 + 12 * i])
            runs.append((first, per_chunk))

        def per_chunk_for(chunk_index):
            per = 1
            for first, p in runs:
                if chunk_index >= first:
                    per = p
                else:
                    break
            return per

        # Walk chunks and slice samples out of the file.
        sample_index = 0
        samples = []
        for chunk_index, base in enumerate(offsets):
            if len(samples) >= limit:
                break
            off = base
            for _ in range(per_chunk_for(chunk_index)):
                if sample_index >= len(sizes) or len(samples) >= limit:
                    break
                size = sizes[sample_index]
                samples.append(data[off:off + size])
                off += size
                sample_index += 1
        break

    if avcc is None:
        print("no avcC/hvcC found")
        return 1
    if not samples:
        print("no samples extracted")
        return 1

    # Keep only samples whose length-prefixed NALUs walk exactly to the end.
    nal_size = (avcc[4] & 3) + 1
    valid = []
    for smp in samples:
        p = 0
        ok = True
        while p + nal_size <= len(smp):
            n = int.from_bytes(smp[p:p + nal_size], "big")
            p += nal_size
            if n == 0 or p + n > len(smp):
                ok = False
                break
            p += n
        if ok and p == len(smp) and len(smp) > nal_size:
            valid.append(smp)
    print(f"kept {len(valid)}/{len(samples)} well-formed samples")

    with open(dst, "wb") as f:
        f.write(struct.pack("<I", len(avcc)))
        f.write(avcc)
        f.write(struct.pack("<I", len(valid)))
        for smp in valid:
            f.write(struct.pack("<I", len(smp)))
            f.write(smp)

    print(f"avcC {len(avcc)} bytes (NAL length size {nal_size}), {len(valid)} samples written")
    print(f"first sample {len(valid[0])} bytes, total {sum(len(s) for s in valid)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
