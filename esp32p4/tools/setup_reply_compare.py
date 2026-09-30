#!/usr/bin/env python3
"""Capture the board's device-info SETUP reply and compare it byte-for-byte with
xcertplay's BplistCodec.encode output (ported 1:1 from BplistCodec.kt).

Run from a machine on the board's softAP:

    python setup_reply_compare.py [host] [port]

Sends the same session SETUP an iPhone sends (name/deviceID/macAddress/model/
timingPort) over an encrypted control channel and dumps the exact reply bytes.
"""

import hashlib
import os
import plistlib
import socket
import sys

from airplay_pair_test import (
    Conn,
    hkdf,
    pad,
    sha512,
    tlv_decode,
    tlv_encode,
    to_bytes,
    G,
    N,
)


class XcConn(Conn):
    """Conn whose requests carry a CSeq, like the iPhone's RTSP client."""

    def __init__(self, host, port):
        super().__init__(host, port)
        self.cseq = 0

    def request(self, path, body=b"", ctype="application/pairing+tlv8", method=None):
        self.cseq += 1
        if method is None:
            method = "POST" if body else "GET"
        if body:
            raw = (
                f"{method} {path} RTSP/1.0\r\nContent-Type: {ctype}\r\n"
                f"Content-Length: {len(body)}\r\nCSeq: {self.cseq}\r\n\r\n"
            ).encode() + body
        else:
            raw = f"{method} {path} RTSP/1.0\r\nCSeq: {self.cseq}\r\n\r\n".encode()
        if self.cipher is None:
            self.sock.sendall(raw)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = self.sock.recv(65536)
                if not d:
                    raise RuntimeError("connection closed")
                buf += d
        else:
            self.sock.sendall(self.cipher.encrypt(raw))
            buf = self._recv_frame()
        head, rest = buf.split(b"\r\n\r\n", 1)
        status = head.split(b"\r\n")[0].decode()
        clen = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                clen = int(line.split(b":")[1])
        while len(rest) < clen:
            if self.cipher is None:
                rest += self.sock.recv(65536)
            else:
                rest += self._recv_frame()
        return status, head, rest[:clen]


# ---------------------------------------------------------------------------
# xcertplay BplistCodec.kt encode() ported 1:1 (node order, no interning).
# ---------------------------------------------------------------------------


class _Leaf:
    def __init__(self, body):
        self.body = body


class _Container:
    def __init__(self, head, refs):
        self.head = head
        self.refs = refs


def _be(value, size):
    return value.to_bytes(size, "big")


def _marker(type_, count):
    if count < 0x0F:
        return bytes([(type_ << 4) | count])
    size = 4 if count > 0xFFFF else 2 if count > 0xFF else 1
    log = {4: 2, 2: 1, 1: 0}[size]
    return bytes([(type_ << 4) | 0x0F, 0x10 | log]) + _be(count, size)


def _encode_string(value):
    if all(ord(c) <= 0x7F for c in value):
        return _marker(0x5, len(value)) + value.encode("ascii")
    return _marker(0x6, len(value)) + value.encode("utf-16-be")


def _encode_int(number):
    if number > 0xFFFFFFFF:
        size, log = 8, 3
    elif number > 0xFFFF:
        size, log = 4, 2
    elif number > 0xFF:
        size, log = 2, 1
    else:
        size, log = 1, 0
    return bytes([0x10 | log]) + _be(number, size)


def xcertplay_bplist_encode(root):
    nodes = []

    def add(value):
        index = len(nodes)
        nodes.append(_Leaf(b""))
        if isinstance(value, bool):
            nodes[index] = _Leaf(bytes([0x09 if value else 0x08]))
        elif isinstance(value, int):
            assert value >= 0
            nodes[index] = _Leaf(_encode_int(value))
        elif isinstance(value, str):
            nodes[index] = _Leaf(_encode_string(value))
        elif isinstance(value, (bytes, bytearray)):
            nodes[index] = _Leaf(_marker(0x4, len(value)) + bytes(value))
        elif isinstance(value, (list, tuple)):
            refs = [add(v) for v in value]
            nodes[index] = _Container(_marker(0xA, len(value)), refs)
        elif isinstance(value, dict):
            entries = list(value.items())
            keyrefs = [add(k) for k, _ in entries]
            valrefs = [add(v) for _, v in entries]
            nodes[index] = _Container(_marker(0xD, len(entries)), keyrefs + valrefs)
        else:
            raise TypeError(f"unsupported {value!r}")
        return index

    top = add(root)
    refsize = 4 if len(nodes) > 0xFFFF else 2 if len(nodes) > 0xFF else 1
    parts = [b"bplist00"]
    cursor = 8
    offsets = []
    for node in nodes:
        offsets.append(cursor)
        if isinstance(node, _Leaf):
            serialized = node.body
        else:
            serialized = node.head + b"".join(_be(r, refsize) for r in node.refs)
        parts.append(serialized)
        cursor += len(serialized)
    table_offset = cursor
    offset_size = 4 if cursor > 0xFFFF else 2 if cursor > 0xFF else 1
    for offset in offsets:
        parts.append(_be(offset, offset_size))
    trailer = bytearray(32)
    trailer[6] = offset_size
    trailer[7] = refsize
    trailer[8:16] = _be(len(nodes), 8)
    trailer[16:24] = _be(top, 8)
    trailer[24:32] = _be(table_offset, 8)
    parts.append(bytes(trailer))
    return b"".join(parts)


# ---------------------------------------------------------------------------
# xc_bplist.c serializer ported 1:1 for the device-info SETUP reply build order
# (global interning, alphabetical dict-key sort, 2-byte offset table, 33-byte
# trailer whose first byte is never read because parsers take the last 32).
# ---------------------------------------------------------------------------


def xc_bplist_setup_reply(timing_port, event_port, keepalive_port=None):
    objects = []

    def add_str(s):
        for i, o in enumerate(objects):
            if o[0] == "str" and o[1] == s:
                return i
        objects.append(("str", s))
        return len(objects) - 1

    def add_int(v):
        for i, o in enumerate(objects):
            if o[0] == "int" and o[1] == v:
                return i
        objects.append(("int", v))
        return len(objects) - 1

    keys = []
    vals = []
    keys.append(add_str("timingPort"))
    vals.append(add_int(timing_port))
    keys.append(add_str("eventPort"))
    vals.append(add_int(event_port))
    if keepalive_port is not None:
        keys.append(add_str("keepAlivePort"))
        vals.append(add_int(keepalive_port))
    feats = [add_str("iAPChannel"), add_str("viewAreas")]
    keys.append(add_str("enabledFeatures"))
    vals.append(len(objects))
    objects.append(("arr", feats))
    top = len(objects)
    objects.append(("dict", list(zip(keys, vals))))

    def obj_size(o):
        if o[0] == "str":
            n = len(o[1])
            return 1 + (0 if n < 15 else 1 + int_size(n)) + n
        if o[0] == "int":
            return 1 + int_size(o[1])
        if o[0] == "arr":
            n = len(o[1])
            return 1 + (0 if n < 15 else 1 + int_size(n)) + n
        if o[0] == "dict":
            n = len(o[1])
            return 1 + (0 if n < 15 else 1 + int_size(n)) + 2 * n
        raise TypeError(o)

    def int_size(v):
        if v <= 0xFF:
            return 1
        if v <= 0xFFFF:
            return 2
        if v <= 0xFFFFFFFF:
            return 4
        return 8

    offsets = []
    cursor = 8
    for o in objects:
        offsets.append(cursor)
        cursor += obj_size(o)
    table_offset = cursor
    offset_int_size = 1 if table_offset <= 0xFF else 2 if table_offset <= 0xFFFF else 4
    refsize = 1 if len(objects) <= 0xFF else 2
    out = bytearray(b"bplist00")

    def write_int(v):
        nb = int_size(v)
        log = {1: 0, 2: 1, 4: 2, 8: 3}[nb]
        out.append(0x10 | log)
        out.extend(_be(v, nb))

    def write_marker(base, count):
        if count < 15:
            out.append(base | count)
        else:
            out.append(base | 0x0F)
            write_int(count)

    for o in objects:
        if o[0] == "str":
            write_marker(0x50, len(o[1]))
            out.extend(o[1].encode())
        elif o[0] == "int":
            write_int(o[1])
        elif o[0] == "arr":
            write_marker(0xA0, len(o[1]))
            for r in o[1]:
                out.extend(_be(r, refsize))
        else:
            write_marker(0xD0, len(o[1]))
            for k, _ in o[1]:
                out.extend(_be(k, refsize))
            for _, v in o[1]:
                out.extend(_be(v, refsize))
    for off in offsets:
        out.extend(_be(off, offset_int_size))
    out.extend(b"\x00" * 5)        # unused
    out.append(0)                  # sortVersion
    out.append(offset_int_size)
    out.append(refsize)
    out.extend(_be(len(objects), 8))
    out.extend(_be(top, 8))
    out.extend(_be(table_offset, 8))
    return bytes(out)


def cseq_check(head):
    lines = head.split(b"\r\n")
    return {
        "status": lines[0].decode(),
        "has_cseq": any(l.lower().startswith(b"cseq:") for l in lines[1:]),
        "headers": [l.decode() for l in lines[1:] if l],
    }


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7000
    c = XcConn(host, port)

    status, _, _ = c.request("/info")
    assert "200" in status, status
    print(f"GET /info -> {status}")

    # pair-setup (same dance as airplay_pair_test.py)
    a = int.from_bytes(os.urandom(32), "big")
    A = pow(G, a, N)
    status, _, body = c.request("/pair-setup", tlv_encode(**{"6": bytes([1])}))
    t = tlv_decode(body)
    B = int.from_bytes(t[3], "big")
    salt = t[2]
    x = int.from_bytes(sha512(salt, sha512(b"Pair-Setup:3939")), "big")
    k = int.from_bytes(sha512(to_bytes(N) + pad(G)), "big")
    u = int.from_bytes(sha512(pad(A) + pad(B)), "big")
    S = pow((B - k * pow(G, x, N)) % N, a + u * x, N)
    K = sha512(to_bytes(S))
    hn, hg, hi = sha512(to_bytes(N)), sha512(to_bytes(G)), sha512(b"Pair-Setup")
    hxor = bytes(p ^ q for p, q in zip(hn, hg))
    proof = sha512(hxor, hi, salt, pad(A), pad(B), K)
    status, _, body = c.request("/pair-setup", tlv_encode(**{"6": bytes([3]), "3": pad(A), "4": proof}))
    t = tlv_decode(body)
    assert t.get(6) == bytes([4]), t
    from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

    ctrl_key = ed25519.Ed25519PrivateKey.generate()
    ctrl_pub = ctrl_key.public_key().public_bytes_raw()
    ctrl_id = b"python-setup-compare"
    csk = hkdf(K, b"Pair-Setup-Controller-Sign-Salt", b"Pair-Setup-Controller-Sign-Info")
    csig = ctrl_key.sign(csk + ctrl_id + ctrl_pub)
    inner = tlv_encode(**{"1": ctrl_id, "3": ctrl_pub, "10": csig})
    ekey = hkdf(K, b"Pair-Setup-Encrypt-Salt", b"Pair-Setup-Encrypt-Info")
    sealed = ChaCha20Poly1305(ekey).encrypt(b"\x00\x00\x00\x00PS-Msg05", inner, None)
    status, _, body = c.request("/pair-setup", tlv_encode(**{"6": bytes([5]), "5": sealed}))
    t = tlv_decode(body)
    plain = ChaCha20Poly1305(ekey).decrypt(b"\x00\x00\x00\x00PS-Msg06", t[5], None)
    acc = tlv_decode(plain)
    acc_ltpk = acc[3]
    print("pair-setup OK")

    # pair-verify
    eph = x25519.X25519PrivateKey.generate()
    eph_pub = eph.public_key().public_bytes_raw()
    status, _, body = c.request("/pair-verify", tlv_encode(**{"6": bytes([1]), "3": eph_pub}))
    t = tlv_decode(body)
    acc_eph = t[3]
    shared = eph.exchange(x25519.X25519PublicKey.from_public_bytes(acc_eph))
    vsig = ctrl_key.sign(eph_pub + ctrl_id + acc_eph)
    inner = tlv_encode(**{"1": ctrl_id, "10": vsig})
    vkey = hkdf(shared, b"Pair-Verify-Encrypt-Salt", b"Pair-Verify-Encrypt-Info")
    sealed = ChaCha20Poly1305(vkey).encrypt(b"\x00\x00\x00\x00PV-Msg03", inner, None)
    status, _, body = c.request("/pair-verify", tlv_encode(**{"6": bytes([3]), "5": sealed}))
    t = tlv_decode(body)
    assert t.get(6) == bytes([4]), t
    read_key = hkdf(shared, b"Control-Salt", b"Control-Read-Encryption-Key")
    write_key = hkdf(shared, b"Control-Salt", b"Control-Write-Encryption-Key")
    from airplay_pair_test import CtrlCipher

    c.cipher = CtrlCipher(read_key=write_key, write_key=read_key)
    print("pair-verify OK, control cipher enabled")

    # the session SETUP the iPhone sends
    setup_req = plistlib.dumps(
        {
            "name": "iPhone",
            "deviceID": "6E:2F:CC:EE:D3:46",
            "macAddress": "6e:2f:cc:ee:d3:46",
            "model": "iPhone14,5",
            "timingPort": 60467,
            "keepAliveSendStatsAsBody": False,
            "keepAliveLowPower": False,
        },
        fmt=plistlib.FMT_BINARY,
    )
    status, head, body = c.request(
        "rtsp://192.168.4.1/17570211294418390928",
        setup_req,
        ctype="application/x-apple-binary-plist",
        method="SETUP",
    )
    info = cseq_check(head)
    print(f"\nSETUP reply status      : {info['status']}")
    print(f"SETUP reply headers     : {info['headers']}")
    print(f"SETUP reply has CSeq    : {info['has_cseq']}")
    print(f"SETUP reply plist bytes : {len(body)}")
    print(f"SETUP reply plist hex   : {body.hex()}")

    decoded = plistlib.loads(body)
    print(f"SETUP reply decoded     : {decoded}")

    # rebuild the same logical dict in xcertplay's insertion order and compare
    ref = {}
    if "timingPort" in decoded:
        ref["timingPort"] = int(decoded["timingPort"])
    if "eventPort" in decoded:
        ref["eventPort"] = int(decoded["eventPort"])
    if "keepAlivePort" in decoded:
        ref["keepAlivePort"] = int(decoded["keepAlivePort"])
    if "enabledFeatures" in decoded:
        ref["enabledFeatures"] = [str(x) for x in decoded["enabledFeatures"]]
    ref_bytes = xcertplay_bplist_encode(ref)
    print(f"\nxcertplay ref bytes     : {len(ref_bytes)}")
    print(f"xcertplay ref hex       : {ref_bytes.hex()}")

    c_port_map = {
        k: int(decoded[k]) for k in ("timingPort", "eventPort", "keepAlivePort") if k in decoded
    }
    c_bytes = xc_bplist_setup_reply(
        c_port_map.get("timingPort", 0),
        c_port_map.get("eventPort", 0),
        c_port_map["keepAlivePort"] if "keepAlivePort" in c_port_map else None,
    )
    print(f"\nxc_bplist.c model bytes : {len(c_bytes)}")
    print(f"xc_bplist.c model hex   : {c_bytes.hex()}")
    print(f"model == captured       : {c_bytes == body}")

    print("\nbyte diff (captured vs xcertplay ref):")
    ref_decoded = plistlib.loads(ref_bytes)
    print(f"  semantic equality (decoded dicts): {ref_decoded == decoded}")
    n = max(len(body), len(ref_bytes))
    diffs = 0
    for i in range(n):
        cb = body[i] if i < len(body) else None
        rb = ref_bytes[i] if i < len(ref_bytes) else None
        if cb != rb:
            diffs += 1
            if diffs <= 40:
                print(f"  +{i:3d} captured={cb if cb is None else format(cb, '02x')} "
                      f"xcertplay={rb if rb is None else format(rb, '02x')}")
    if diffs == 0 and len(body) == len(ref_bytes):
        print("  identical")
    else:
        print(f"  {diffs} differing byte positions; "
              f"lengths captured={len(body)} ref={len(ref_bytes)}")

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "setup_reply_captured.bin")
    with open(out_path, "wb") as fh:
        fh.write(body)
    print(f"\ncaptured plist saved to {out_path}")


if __name__ == "__main__":
    main()
