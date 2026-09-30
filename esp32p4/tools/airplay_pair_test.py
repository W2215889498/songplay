#!/usr/bin/env python3
"""End-to-end AirPlay pair-setup / pair-verify test against the xcertplay board.

Run from a machine connected to the board's softAP (xcertplay-CarPlay):

    python airplay_pair_test.py [host] [port]

It performs the same handshake an iPhone would up to the control channel:
  GET /info  ->  /pair-setup (SRP-6a + M5/M6)  ->  /pair-verify (X25519 + Ed25519)
"""

import hashlib
import hmac
import os
import plistlib
import socket
import sys
import time

from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

VIDEO_BLOB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_video.h264p")

N_HEX = (
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
    "995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
    "85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
    "F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
    "7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
    "E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF")
N = int(N_HEX, 16)
NLEN = (N.bit_length() + 7) // 8          # 384
G = 5
SRP_USER = b"Pair-Setup"
SRP_PASS = b"3939"

TLV_METHOD, TLV_ID, TLV_SALT, TLV_PK, TLV_PROOF, TLV_ENC, TLV_STATE, TLV_ERR, TLV_SIG = (
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0A)


def sha512(*parts):
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return h.digest()


def hkdf(ikm, salt, info, length=32):
    prk = hmac.new(salt, ikm, hashlib.sha512).digest()
    okm, t = b"", b""
    i = 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha512).digest()
        okm += t
        i += 1
    return okm[:length]


def pad(x):
    return x.to_bytes(NLEN, "big")


def to_bytes(x):
    if x == 0:
        return b"\x00"
    return x.to_bytes((x.bit_length() + 7) // 8, "big")


def tlv_encode(**items):
    out = b""
    for t, v in items.items():
        t = int(t)
        if not v:
            out += bytes([t, 0])
            continue
        for i in range(0, len(v), 255):
            chunk = v[i:i + 255]
            out += bytes([t, len(chunk)]) + chunk
    return out


def tlv_decode(data):
    vals = {}
    i = 0
    while i + 2 <= len(data):
        t, ln = data[i], data[i + 1]
        vals.setdefault(t, b"")
        vals[t] += data[i + 2:i + 2 + ln]
        i += 2 + ln
    return vals


class CtrlCipher:
    """ChaCha20-Poly1305 framing used by the CarPlay control channel."""

    def __init__(self, read_key, write_key):
        self.rk, self.wk = read_key, write_key
        self.rc = 0
        self.wc = 0

    def encrypt(self, data):
        out = b""
        off = 0
        while True:
            chunk = data[off:off + 0x4000]
            hdr = len(chunk).to_bytes(2, "little")
            nonce = b"\x00" * 4 + self.wc.to_bytes(8, "little")
            out += hdr + ChaCha20Poly1305(self.wk).encrypt(nonce, chunk, hdr)
            self.wc += 1
            off += 0x4000
            if off >= len(data):
                return out

    def decrypt_frame(self, hdr, ct):
        nonce = b"\x00" * 4 + self.rc.to_bytes(8, "little")
        self.rc += 1
        return ChaCha20Poly1305(self.rk).decrypt(nonce, ct, hdr)


class Conn:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), 10)
        self.cipher = None

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            d = self.sock.recv(n - len(buf))
            if not d:
                raise RuntimeError("connection closed")
            buf += d
        return buf

    def _recv_frame(self):
        hdr = self._recv_exact(2)
        ln = int.from_bytes(hdr, "little")
        ct = self._recv_exact(ln + 16)
        return self.cipher.decrypt_frame(hdr, ct)

    def request(self, path, body=b"", ctype="application/pairing+tlv8"):
        if body:
            raw = (f"POST {path} RTSP/1.0\r\nContent-Type: {ctype}\r\n"
                   f"Content-Length: {len(body)}\r\n\r\n").encode() + body
        else:
            raw = f"GET {path} RTSP/1.0\r\n\r\n".encode()

        if self.cipher is None:
            self.sock.sendall(raw)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = self.sock.recv(4096)
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
                rest += self.sock.recv(4096)
            else:
                rest += self._recv_frame()
        return status, rest[:clen]


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "192.168.4.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 7000
    c = Conn(host, port)
    print(f"connected to {host}:{port}")

    status, body = c.request("/info")
    print(f"GET /info -> {status} ({len(body)} bytes)")
    assert "200" in status, "info failed"

    # ---------------- pair-setup ----------------
    a = int.from_bytes(os.urandom(32), "big")
    A = pow(G, a, N)

    status, body = c.request("/pair-setup", tlv_encode(**{"6": bytes([1])}))
    t = tlv_decode(body)
    assert t.get(TLV_STATE) == bytes([2]), f"bad state {t.get(TLV_STATE)}"
    B = int.from_bytes(t[TLV_PK], "big")
    salt = t[TLV_SALT]
    print(f"pair-setup M2: B={len(t[TLV_PK])}B salt={len(salt)}B")

    x = int.from_bytes(sha512(salt, sha512(SRP_USER + b":" + SRP_PASS)), "big")
    k = int.from_bytes(sha512(to_bytes(N) + pad(G)), "big")
    u = int.from_bytes(sha512(pad(A) + pad(B)), "big")
    S = pow((B - k * pow(G, x, N)) % N, a + u * x, N)
    K = sha512(to_bytes(S))
    hn, hg, hi = sha512(to_bytes(N)), sha512(to_bytes(G)), sha512(SRP_USER)
    hxor = bytes(p ^ q for p, q in zip(hn, hg))
    proof = sha512(hxor, hi, salt, pad(A), pad(B), K)

    status, body = c.request("/pair-setup",
                             tlv_encode(**{"6": bytes([3]), "3": pad(A), "4": proof}))
    t = tlv_decode(body)
    assert t.get(TLV_STATE) == bytes([4]), f"bad state {t.get(TLV_STATE)}"
    expect = sha512(pad(A), proof, K)
    assert t[TLV_PROOF] == expect, "server proof mismatch"
    print("pair-setup M4: server proof OK")

    # M5: prove controller identity
    ctrl_key = ed25519.Ed25519PrivateKey.generate()
    ctrl_pub = ctrl_key.public_key().public_bytes_raw()
    ctrl_id = b"python-pair-test"
    csk = hkdf(K, b"Pair-Setup-Controller-Sign-Salt", b"Pair-Setup-Controller-Sign-Info")
    csig = ctrl_key.sign(csk + ctrl_id + ctrl_pub)
    inner = tlv_encode(**{"1": ctrl_id, "3": ctrl_pub, "10": csig})
    ekey = hkdf(K, b"Pair-Setup-Encrypt-Salt", b"Pair-Setup-Encrypt-Info")
    sealed = ChaCha20Poly1305(ekey).encrypt(b"\x00\x00\x00\x00" + b"PS-Msg05", inner, None)
    status, body = c.request("/pair-setup", tlv_encode(**{"6": bytes([5]), "5": sealed}))
    t = tlv_decode(body)
    assert t.get(TLV_STATE) == bytes([6]), f"bad state {t.get(TLV_STATE)}"
    plain = ChaCha20Poly1305(ekey).decrypt(b"\x00\x00\x00\x00" + b"PS-Msg06", t[TLV_ENC], None)
    acc = tlv_decode(plain)
    ask = hkdf(K, b"Pair-Setup-Accessory-Sign-Salt", b"Pair-Setup-Accessory-Sign-Info")
    ed25519.Ed25519PublicKey.from_public_bytes(acc[TLV_PK]).verify(
        acc[TLV_SIG], ask + acc[TLV_ID] + acc[TLV_PK])
    acc_ltpk = acc[TLV_PK]
    print(f"pair-setup M6: accessory id={acc[TLV_ID].decode()} signature OK  -> PAIRED")

    # ---------------- pair-verify ----------------
    eph = x25519.X25519PrivateKey.generate()
    eph_pub = eph.public_key().public_bytes_raw()
    status, body = c.request("/pair-verify", tlv_encode(**{"6": bytes([1]), "3": eph_pub}))
    t = tlv_decode(body)
    assert t.get(TLV_STATE) == bytes([2]), f"bad state {t.get(TLV_STATE)}"
    acc_eph = t[TLV_PK]
    shared = eph.exchange(x25519.X25519PublicKey.from_public_bytes(acc_eph))
    vkey = hkdf(shared, b"Pair-Verify-Encrypt-Salt", b"Pair-Verify-Encrypt-Info")
    plain = ChaCha20Poly1305(vkey).decrypt(b"\x00\x00\x00\x00" + b"PV-Msg02", t[TLV_ENC], None)
    m2 = tlv_decode(plain)
    ed25519.Ed25519PublicKey.from_public_bytes(acc_ltpk).verify(
        m2[TLV_SIG], acc_eph + m2[TLV_ID] + eph_pub)
    print("pair-verify M2: accessory signature OK")

    vsig = ctrl_key.sign(eph_pub + ctrl_id + acc_eph)
    inner = tlv_encode(**{"1": ctrl_id, "10": vsig})
    sealed = ChaCha20Poly1305(vkey).encrypt(b"\x00\x00\x00\x00" + b"PV-Msg03", inner, None)
    status, body = c.request("/pair-verify", tlv_encode(**{"6": bytes([3]), "5": sealed}))
    t = tlv_decode(body)
    assert t.get(TLV_STATE) == bytes([4]), f"bad state {t.get(TLV_STATE)}"
    print("pair-verify M4: OK")

    read_key = hkdf(shared, b"Control-Salt", b"Control-Read-Encryption-Key")
    write_key = hkdf(shared, b"Control-Salt", b"Control-Write-Encryption-Key")
    print(f"control keys derived: read={read_key.hex()[:16]}... write={write_key.hex()[:16]}...")

    # ---------------- /auth-setup (MFi SAP) over the encrypted channel --------
    # The accessory decrypts with readKey and encrypts with writeKey, so the
    # controller uses them the other way around.
    c.cipher = CtrlCipher(read_key=write_key, write_key=read_key)
    akey = x25519.X25519PrivateKey.generate()
    apub = akey.public_key().public_bytes_raw()
    status, body = c.request("/auth-setup", b"\x01" + apub,
                             ctype="application/octet-stream")
    print(f"POST /auth-setup -> {status} ({len(body)} bytes) [encrypted channel]")
    assert "200" in status, "auth-setup failed"

    off = 0
    board_pub = body[off:off + 32]; off += 32
    clen = int.from_bytes(body[off:off + 4], "big"); off += 4
    cert = body[off:off + clen]; off += clen
    slen = int.from_bytes(body[off:off + 4], "big"); off += 4
    enc_sig = body[off:off + slen]; off += slen

    ashared = akey.exchange(x25519.X25519PublicKey.from_public_bytes(board_pub))
    aes_key = hashlib.sha1(b"AES-KEY" + ashared).digest()[:16]
    aes_iv = hashlib.sha1(b"AES-IV" + ashared).digest()[:16]
    dec = Cipher(algorithms.AES(aes_key), modes.CTR(aes_iv)).decryptor()
    mfi_sig = dec.update(enc_sig) + dec.finalize()

    assert cert[:2] == b"\x30\x82" and 256 <= clen <= 1024, (cert[:2], clen)
    assert len(mfi_sig) == 64, len(mfi_sig)
    print(f"/auth-setup: MFi certificate {clen}B (DER ok), "
          f"encrypted signature {slen}B -> {len(mfi_sig)}B decrypted")

    # ---------------- /stream SETUP + video channel ----------------
    setup = plistlib.dumps({"streams": [{"type": 110, "streamConnectionID": 1}]},
                           fmt=plistlib.FMT_BINARY)
    status, body = c.request("/stream", setup, ctype="application/x-apple-binary-plist")
    print(f"POST /stream -> {status} ({len(body)} bytes)")
    resp = plistlib.loads(body)
    vstreams = resp.get("streams", [])
    assert vstreams and isinstance(vstreams[0], dict) and "dataPort" in vstreams[0], resp
    vport = int(vstreams[0]["dataPort"])
    print(f"/stream: screen type={vstreams[0].get('type')} dataPort={vport}")

    vkey = hkdf(shared, b"DataStream-Salt1", b"DataStream-Output-Encryption-Key")

    blob = open(VIDEO_BLOB, "rb").read()
    off = 0
    avcc_len = int.from_bytes(blob[off:off + 4], "little"); off += 4
    avcc = blob[off:off + avcc_len]; off += avcc_len
    nsamples = int.from_bytes(blob[off:off + 4], "little"); off += 4
    samples = []
    for _ in range(nsamples):
        n = int.from_bytes(blob[off:off + 4], "little"); off += 4
        samples.append(blob[off:off + n]); off += n

    vsock = socket.create_connection(("192.168.4.1", vport), 10)

    def send_msg(opcode, payload):
        hdr = bytearray(128)
        hdr[0:4] = len(payload).to_bytes(4, "little")
        hdr[4] = opcode
        vsock.sendall(bytes(hdr) + payload)

    send_msg(1, b"\x00\x00\x00\x00avcC" + avcc)
    counter = 0
    for smp in samples:
        hdr = bytearray(128)
        hdr[0:4] = (len(smp) + 16).to_bytes(4, "little")
        hdr[4] = 0
        nonce = b"\x00" * 4 + counter.to_bytes(8, "little")
        ct = ChaCha20Poly1305(vkey).encrypt(nonce, smp, bytes(hdr))
        vsock.sendall(bytes(hdr) + ct)
        counter += 1
    print(f"video: sent codec config + {counter} encrypted H.264 frames ({avcc_len}B avcC)")
    time.sleep(1.5)
    vsock.close()

    print("\nRESULT: pairing, verification, control cipher, MFi auth-setup and video channel all OK")


if __name__ == "__main__":
    main()
