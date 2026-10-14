#!/usr/bin/env python3
# Tests mssh against a scripted SSH server and against its trust decisions:
#     python3 mssh/test_hostile.py [--keepalive] [path/to/mssh]
#                                   (default path: ./mssh next to this file)
# --keepalive also runs the keepalive scenarios, which wait out the real 150 s and
# 180 s timers: about seven minutes, so they are not part of the default run.
#
# The server (in a thread of this process) speaks real SSH-2 -- curve25519-sha256,
# ssh-ed25519, chacha20-poly1305@openssh.com, publickey auth of any key -- so that
# the packets it sends have valid MACs and mssh gets far enough to reach the code
# under test. Each scenario then does one hostile thing. Needs the Python package
# `cryptography`. Nothing is installed system-wide; everything lives in a temp dir.
import base64, os, pty, select, shutil, socket, struct, subprocess, sys, tempfile
import threading, time
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms
from cryptography.hazmat.primitives.poly1305 import Poly1305
from cryptography.hazmat.primitives import hashes

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = {a for a in sys.argv[1:] if a.startswith("--")}
_ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
MSSH = _ARGS[0] if _ARGS else os.path.join(HERE, "mssh")
SERVER_VER = b"SSH-2.0-mssh_test_server"

# ── wire helpers ────────────────────────────────────────────────────────────

def s_(b):  return struct.pack(">I", len(b)) + b
def u32(v): return struct.pack(">I", v)
def mpint(b):
    while len(b) > 1 and b[0] == 0: b = b[1:]
    if b[0] & 0x80: b = b"\0" + b
    return s_(b)
def sha256(*parts):
    h = hashes.Hash(hashes.SHA256())
    for p in parts: h.update(p)
    return h.finalize()
def derive(K, H, letter, sid, need):
    out = sha256(K, H, letter, sid)
    while len(out) < need: out += sha256(K, H, out)
    return out[:need]
def chacha(key, nonce8, ic, data):
    n16 = struct.pack("<I", ic & 0xffffffff) + struct.pack("<I", ic >> 32) + nonce8
    return Cipher(algorithms.ChaCha20(key, n16), None).encryptor().update(data)
def pub_raw(priv): return priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
def hostkey_blob(pk): return s_(b"ssh-ed25519") + s_(pub_raw(pk))
def kexinit(kex=("curve25519-sha256",)):
    nl = lambda *n: s_(",".join(n).encode())
    return (bytes([20]) + os.urandom(16) + nl(*kex) + nl("ssh-ed25519")
            + nl("chacha20-poly1305@openssh.com") * 2 + nl("hmac-sha2-256") * 2
            + nl("none") * 2 + nl() * 2 + b"\0" + u32(0))

class Conn:
    def __init__(self, sock):
        self.s = sock; self.buf = b""
        self.send_seq = self.recv_seq = 0
        self.send_key = self.recv_key = None
        self.strict = False; self.sid = None; self.client_ver = b""
    def send(self, b): self.s.sendall(b)
    def read(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d: raise EOFError
            self.buf += d
        r, self.buf = self.buf[:n], self.buf[n:]
        return r
    def send_pkt(self, payload):
        pad = 8 - ((1 + len(payload)) % 8)
        if pad < 4: pad += 8
        body = bytes([pad]) + payload + os.urandom(pad)
        if self.send_key is None:
            self.send(u32(len(body)) + body)
        else:
            self.send_raw_enc(len(body), body)
            return
        self.send_seq = (self.send_seq + 1) & 0xffffffff
    def send_raw_enc(self, plen, body, mac_ok=True):
        k = self.send_key; nonce = struct.pack(">Q", self.send_seq)
        enc_len = chacha(k[32:], nonce, 0, u32(plen))
        enc_body = chacha(k[:32], nonce, 1, body)
        pk = Poly1305(chacha(k[:32], nonce, 0, bytes(64))[:32]); pk.update(enc_len + enc_body)
        self.send(enc_len + enc_body + (pk.finalize() if mac_ok else bytes(16)))
        self.send_seq = (self.send_seq + 1) & 0xffffffff
    def recv_pkt(self):
        if self.recv_key is None:
            pl = struct.unpack(">I", self.read(4))[0]
            body = self.read(pl)
            self.recv_seq = (self.recv_seq + 1) & 0xffffffff
            return body[1:pl - body[0]]
        k = self.recv_key; nonce = struct.pack(">Q", self.recv_seq)
        enc_len = self.read(4)
        pl = struct.unpack(">I", chacha(k[32:], nonce, 0, enc_len))[0]
        enc_body = self.read(pl); tag = self.read(16)
        pk = Poly1305(chacha(k[:32], nonce, 0, bytes(64))[:32]); pk.update(enc_len + enc_body)
        pk.finalize()                                   # raises InvalidSignature on a bad MAC
        body = chacha(k[:32], nonce, 1, enc_body)
        self.recv_seq = (self.recv_seq + 1) & 0xffffffff
        return body[1:pl - body[0]]

# ── the server's side of the protocol ───────────────────────────────────────

def handshake(c, hk, strict_on=True, before_kexinit=None, in_kex=None, sig_tag=b"ssh-ed25519",
              kexinit_override=None):
    cli = c.recv_pkt()
    c.strict = strict_on and b"kex-strict-c-v00@openssh.com" in cli
    srv = kexinit_override if kexinit_override is not None else kexinit(
        ("curve25519-sha256",) + (("kex-strict-s-v00@openssh.com",) if c.strict else ()))
    if before_kexinit: c.send_pkt(before_kexinit)
    c.send_pkt(srv)
    ecdh = c.recv_pkt()
    if ecdh[0] != 30: raise RuntimeError("expected KEX_ECDH_INIT")
    Qc = ecdh[5:5 + struct.unpack(">I", ecdh[1:5])[0]]
    eph = X25519PrivateKey.generate(); Qs = pub_raw(eph)
    K = mpint(eph.exchange(X25519PublicKey.from_public_bytes(Qc)))
    hkb = hostkey_blob(hk)
    H = sha256(s_(c.client_ver), s_(SERVER_VER), s_(cli), s_(srv), s_(hkb), s_(Qc), s_(Qs), K)
    if in_kex: c.send_pkt(in_kex)
    c.send_pkt(bytes([31]) + s_(hkb) + s_(Qs) + s_(s_(sig_tag) + s_(hk.sign(H))))
    c.send_pkt(bytes([21]))
    if c.strict: c.send_seq = 0
    c.sid = H
    c2s = derive(K, H, b"C", H, 64); s2c = derive(K, H, b"D", H, 64)
    if c.recv_pkt()[0] != 21: raise RuntimeError("expected NEWKEYS")
    c.recv_key = c2s
    if c.strict: c.recv_seq = 0
    c.send_key = s2c

def rekey(c, signer, srv_kexinit=None):
    """Server-initiated re-key: the host key advertised is the one that signs."""
    srv = srv_kexinit or kexinit()
    c.send_pkt(srv)
    cli = c.recv_pkt()
    while cli[0] == 96: cli = c.recv_pkt()  # mssh's EOF, when its stdin has ended, can come first
    ecdh = c.recv_pkt(); assert ecdh[0] == 30
    Qc = ecdh[5:5 + struct.unpack(">I", ecdh[1:5])[0]]
    eph = X25519PrivateKey.generate(); Qs = pub_raw(eph)
    K = mpint(eph.exchange(X25519PublicKey.from_public_bytes(Qc)))
    hkb = hostkey_blob(signer)
    H = sha256(s_(c.client_ver), s_(SERVER_VER), s_(cli), s_(srv), s_(hkb), s_(Qc), s_(Qs), K)
    c.send_pkt(bytes([31]) + s_(hkb) + s_(Qs) + s_(s_(b"ssh-ed25519") + s_(signer.sign(H))))
    c.send_pkt(bytes([21]))
    if c.strict: c.send_seq = 0
    c2s = derive(K, H, b"C", c.sid, 64); s2c = derive(K, H, b"D", c.sid, 64)
    if c.recv_pkt()[0] != 21: raise RuntimeError("expected NEWKEYS")
    c.recv_key = c2s
    if c.strict: c.recv_seq = 0
    c.send_key = s2c

def serve_session(c):
    """After the handshake: userauth, one session channel, exec.  Returns the channel id."""
    assert c.recv_pkt()[0] == 5
    c.send_pkt(bytes([6]) + s_(b"ssh-userauth"))
    assert c.recv_pkt()[0] == 50
    c.send_pkt(bytes([60]) + s_(b"ssh-ed25519") + s_(b"x"))
    assert c.recv_pkt()[0] == 50
    c.send_pkt(bytes([52]))
    m = c.recv_pkt(); assert m[0] == 90
    sender = struct.unpack(">I", m[5 + struct.unpack(">I", m[1:5])[0]:][:4])[0]
    c.send_pkt(bytes([91]) + u32(sender) + u32(0) + u32(1 << 20) + u32(32768))
    while True:
        m = c.recv_pkt()
        if m[0] != 98: continue
        tl = struct.unpack(">I", m[5:9])[0]; typ = m[9:9 + tl]; want = m[9 + tl]
        if want: c.send_pkt(bytes([99]) + u32(sender))
        if typ in (b"exec", b"shell"): return sender

def chan_data(c, ch, data): c.send_pkt(bytes([94]) + u32(ch) + s_(data))
def close_with(c, ch, status=0):
    c.send_pkt(bytes([96]) + u32(ch))
    c.send_pkt(bytes([98]) + u32(ch) + s_(b"exit-status") + b"\0" + u32(status))
    c.send_pkt(bytes([97]) + u32(ch))
    try:
        while c.recv_pkt()[0] != 97: pass
    except Exception: pass

# ── scenarios: each takes (conn, hostkey) and does the hostile part ─────────

def sc_hello(c, hk):  # the baseline: a well-behaved session
    handshake(c, hk); ch = serve_session(c)
    chan_data(c, ch, b"hello from test server\n"); close_with(c, ch, 7)

def sc_hello_nonstrict(c, hk):
    handshake(c, hk, strict_on=False); ch = serve_session(c)
    chan_data(c, ch, b"hello from test server\n"); close_with(c, ch, 0)

def sc_junk_banner(c, hk):
    sc_hello_nonstrict(c, hk)

def sc_bad_mac(c, hk):
    handshake(c, hk); ch = serve_session(c)
    c.send_raw_enc(0x20, bytes(0x20), mac_ok=False); time.sleep(0.5)

def sc_pl_zero(c, hk):
    handshake(c, hk); serve_session(c)
    c.send_raw_enc(0, b"")

def sc_pl_huge(c, hk):
    handshake(c, hk); serve_session(c)
    c.send_raw_enc(0x7fffffff, b"x" * 16, mac_ok=False)

def sc_bad_padding(c, hk):
    handshake(c, hk); serve_session(c)
    c.send_raw_enc(16, bytes([255]) + bytes(15)); time.sleep(0.5)

def sc_truncated_str(c, hk):   # CHANNEL_DATA claiming 1 GiB in a 9-byte payload
    handshake(c, hk); ch = serve_session(c)
    payload = bytes([94]) + u32(ch) + u32(0x40000000)
    pad = 8 - ((1 + len(payload)) % 8)
    if pad < 4: pad += 8
    c.send_raw_enc(1 + len(payload) + pad, bytes([pad]) + payload + os.urandom(pad))

def sc_disconnect_text(c, hk):
    handshake(c, hk)
    c.send_pkt(bytes([1]) + u32(2) + s_(b"\x1b]0;evil\x07\x1b[31mred\x00") + s_(b""))
    time.sleep(0.5)

def sc_auth_banner_text(c, hk):
    handshake(c, hk); ch = serve_session(c)
    chan_data(c, ch, b"ok\n"); close_with(c, ch, 0)

def sc_rekey_ok(c, hk):
    handshake(c, hk); ch = serve_session(c)
    chan_data(c, ch, b"pre\n"); rekey(c, hk); chan_data(c, ch, b"post-rekey\n"); close_with(c, ch, 3)

def sc_rekey_hostkey_change(c, hk):
    handshake(c, hk); ch = serve_session(c)
    chan_data(c, ch, b"before\n")
    rekey(c, Ed25519PrivateKey.generate())            # a different host key than the one accepted
    time.sleep(0.5)

def sc_ignore_before_kexinit_strict(c, hk):
    handshake(c, hk, before_kexinit=bytes([2]) + s_(b"x"))

def sc_ignore_before_kexinit_nonstrict(c, hk):
    handshake(c, hk, strict_on=False, before_kexinit=bytes([2]) + s_(b"x")); ch = serve_session(c)
    chan_data(c, ch, b"hello from test server\n"); close_with(c, ch, 0)

def sc_ignore_in_kex_strict(c, hk):
    handshake(c, hk, in_kex=bytes([2]) + s_(b"x"))

def sc_short_kexinit(c, hk):  # a 10-byte KEXINIT: the strict-KEX scan must not read past it
    handshake(c, hk, kexinit_override=bytes([20]) + bytes(9))

def sc_sig_algorithm(c, hk):  # a valid ed25519 signature, labelled as another algorithm
    handshake(c, hk, sig_tag=b"rsa-sha2-512")

def sc_stall(c, hk):          # the banner, then nothing at all: a server that stops
    time.sleep(60)

def sc_idle_session(c, hk):   # a session that is quiet for a while once it is running
    handshake(c, hk); ch = serve_session(c)
    time.sleep(4)
    chan_data(c, ch, b"hello from test server\n"); close_with(c, ch, 7)

def sc_banner_flood(c, hk):   # never sends SSH-2.0: the client must give up, not wait forever
    try:
        c.s.sendall(b"junk line\r\n" * 300000)
        time.sleep(30)
    except OSError:
        pass

# ── keepalive: the server goes quiet once the session runs ──────────────────
# rec collects what the server saw: when the keepalive came (seconds since the
# session began), its request, and when mssh closed the connection.

def ka_request(m):
    """(name, want_reply) of a GLOBAL_REQUEST payload."""
    nl = struct.unpack(">I", m[1:5])[0]
    return m[5:5 + nl], m[5 + nl]

def wait_keepalive(c, rec):
    """Read until the keepalive request arrives; other packets from mssh (its EOF, say) are skipped."""
    while True:
        m = c.recv_pkt()
        if m[0] == 80:
            rec["at"] = time.time() - rec["t0"]
            rec["req"] = ka_request(m)
            return

def sc_keepalive_dead(c, hk, rec):    # never answers: mssh sends the keepalive at 150 s, gives up at 180 s
    c.s.settimeout(None)
    handshake(c, hk); serve_session(c)
    rec["t0"] = time.time()
    wait_keepalive(c, rec)
    try:
        while True: c.recv_pkt()          # no reply; wait for mssh to close the connection
    except Exception: pass
    rec["end"] = time.time() - rec["t0"]

def sc_keepalive_reset(c, hk, rec):   # data at 20 s moves the keepalive to 170 s; answering it moves the limit on
    c.s.settimeout(None)
    handshake(c, hk); ch = serve_session(c)
    rec["t0"] = time.time()
    time.sleep(20); chan_data(c, ch, b"tick\n")
    wait_keepalive(c, rec)
    c.send_pkt(bytes([82]))               # REQUEST_FAILURE: what sshd answers an unknown request
    time.sleep(max(0.0, rec["t0"] + 250 - time.time()))   # past 200 s: only the answer keeps the session up
    chan_data(c, ch, b"hello from test server\n"); close_with(c, ch, 7)

# ── the test driver ─────────────────────────────────────────────────────────

class Env:
    def __init__(self):
        self.dir = tempfile.mkdtemp(prefix="mssh-test-")
        self.hk = Ed25519PrivateKey.generate()
        self.ck = Ed25519PrivateKey.generate()
        self.key = os.path.join(self.dir, "id_ed25519")
        self._write_key(self.key, self.ck, 0o600)
        self.kh = os.path.join(self.dir, "known_hosts")
        self.write_kh(self.kh, [self.kh_line()])
    def _write_key(self, path, k, mode):
        open(path, "wb").write(k.private_bytes(serialization.Encoding.PEM,
                               serialization.PrivateFormat.OpenSSH, serialization.NoEncryption()))
        os.chmod(path, mode)
    def kh_line(self, host="127.0.0.1"):
        return "%s ssh-ed25519 %s" % (host, base64.b64encode(hostkey_blob(self.hk)).decode())
    def write_kh(self, path, lines):
        open(path, "w").write("\n".join(lines) + "\n")
    def cleanup(self): shutil.rmtree(self.dir, ignore_errors=True)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

def run_server_scenario(env, scen, port, prefix=b"", banner=True):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port)); ls.listen(1)
    def serve():
        try:
            s, _ = ls.accept(); s.settimeout(60)
            c = Conn(s)
            if banner:                      # the normal version exchange, optionally after junk lines
                s.sendall(prefix + SERVER_VER + b"\r\n")
                line = b""
                while not line.endswith(b"\n"): line += c.read(1)
                c.client_ver = line.rstrip(b"\r\n")
            scen(c, env.hk)
        except (EOFError, OSError, AssertionError, RuntimeError, Exception):
            pass
        finally:
            try: s.close()
            except Exception: pass
            ls.close()
    t = threading.Thread(target=serve, daemon=True); t.start()
    return t

def mssh_cmd(env, port, extra=(), host="root@127.0.0.1", command=("true",), kh=None):
    return [MSSH, "-o", "UserKnownHostsFile=" + (kh or env.kh), "-i", env.key, "-p", str(port),
            *extra, host, *command]

def run(cmd, stdin_data=None, timeout=20, new_session=False):
    try:
        p = subprocess.run(cmd, input=stdin_data, capture_output=True, timeout=timeout,
                           start_new_session=new_session, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as e:
        return "HANG", "", ""

# name -> (server scenario, expected rc, expected text in stdout+stderr, stdin for the prompt)
SERVER_TESTS = [
    ("hello",                           sc_hello,                         7,    "hello from test server", None),
    ("hello, non-strict server",        sc_hello_nonstrict,               0,    "hello from test server", None),
    ("junk lines before the banner",    sc_junk_banner,                   0,    "hello from test server", {"prefix": b"welcome\r\nto nothing\r\n"}),
    ("bad MAC",                         sc_bad_mac,                       1,    "MAC verification FAILED", None),
    ("packet length 0",                 sc_pl_zero,                       1,    "bad encrypted packet length", None),
    ("packet length 2^31-1",            sc_pl_huge,                       1,    "bad encrypted packet length", None),
    ("bad padding length",              sc_bad_padding,                   1,    "bad padding length", None),
    ("string length beyond packet",     sc_truncated_str,                 1,    "buf underflow", None),
    ("disconnect text sanitized",       sc_disconnect_text,               1,    "Server disconnected: ?]0;evil?", None),
    ("auth banner, session runs",       sc_auth_banner_text,              0,    "ok", None),
    ("re-key, same host key",           sc_rekey_ok,                      3,    "post-rekey", None),
    ("re-key, host key changed",        sc_rekey_hostkey_change,          1,    "host key changed during re-key", None),
    ("IGNORE before KEXINIT, strict",   sc_ignore_before_kexinit_strict,  1,    "strict KEX violation", None),
    ("IGNORE before KEXINIT, non-strict", sc_ignore_before_kexinit_nonstrict, 0, "hello from test server", None),
    ("IGNORE during KEX, strict",       sc_ignore_in_kex_strict,          1,    "strict KEX violation", None),
    ("KEXINIT too short to parse",      sc_short_kexinit,                 1,    "KEXINIT too short", None),
    ("signature labelled rsa-sha2-512", sc_sig_algorithm,                 1,    "not ssh-ed25519", None),
    ("never sends the SSH version",     sc_banner_flood,                  1,    "SSH version line", {"banner": False}),
]

def t_unreadable_known_hosts(env):
    """known_hosts exists but cannot be read: this must be an error, not a first-use prompt."""
    d = os.path.join(env.dir, "kh_is_a_directory"); os.makedirs(d, exist_ok=True)
    port = free_port(); t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port, kh=d), stdin_data=b"yes\n", timeout=15)
    return (rc == 1 and "cannot read" in err and "continue connecting" not in err), (rc, err.strip()[-120:])

def t_piped_answer_is_refused(env):
    """Unknown host, answer piped on stdin, no terminal: nobody can have answered."""
    kh = os.path.join(env.dir, "kh_%d" % time.monotonic_ns()); port = free_port()
    t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port, kh=kh), stdin_data=b"yes\n", timeout=15, new_session=True)
    return (rc == 1 and "Host key verification failed" in err and "hello" not in out), (rc, err.strip()[-120:])

def pty_session(env, args, answer, timeout=15, delay=0):
    """Run mssh on a pseudo-terminal, type `answer` at the host-key question.  Returns (status, transcript)."""
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(MSSH, args)
    buf = b""; answered = False; end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.5)
        if fd in r:
            try: data = os.read(fd, 4096)
            except OSError: break
            if not data: break
            buf += data
        if not answered and b"continue connecting" in buf:
            time.sleep(delay)
            os.write(fd, answer); answered = True
    try: os.close(fd)
    except OSError: pass
    _, st = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(st), buf.decode("utf-8", "replace")

def t_typed_yes_on_terminal(env):
    """The question is answered at the keyboard (a terminal), with yes: connects."""
    kh = os.path.join(env.dir, "kh_%d" % time.monotonic_ns()); port = free_port()
    t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, tr = pty_session(env, mssh_cmd(env, port, kh=kh), b"yes\n")
    return (rc == 7 and "hello from test server" in tr), (rc, tr[-160:])

def t_typed_no_on_terminal(env):
    """...and with no: refused, with a message, and the host is not recorded."""
    kh = os.path.join(env.dir, "kh_%d" % time.monotonic_ns()); port = free_port()
    t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, tr = pty_session(env, mssh_cmd(env, port, kh=kh), b"no\n")
    return (rc == 1 and "Host key verification failed" in tr and not os.path.exists(kh)), (rc, tr[-160:])

def t_bracketed_host_refused(env):
    """Brackets are glob characters in a ProxyCommand: a host name with them is refused."""
    rc, out, err = run(mssh_cmd(env, free_port(), extra=("-o", "ProxyCommand=false"), host="root@[a]"), timeout=15)
    return (rc == 1 and "invalid host name" in err), (rc, err.strip()[-120:])

def t_malformed_known_hosts_line(env):
    """A malformed line before the right entry is skipped; the right entry is used."""
    kh = os.path.join(env.dir, "kh_malformed_%d" % time.monotonic_ns())
    env.write_kh(kh, ["127.0.0.1 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5", env.kh_line()])
    port = free_port(); t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port, kh=kh), timeout=15)
    return (rc == 7 and "hello from test server" in out), (rc, err.strip()[-120:])

def t_key_mode_refused(env):
    """A private key readable by others is refused, as OpenSSH does."""
    os.chmod(env.key, 0o644)
    try:
        rc, out, err = run(mssh_cmd(env, free_port()), timeout=15)
    finally:
        os.chmod(env.key, 0o600)
    return (rc == 1 and "unprotected private key" in err), (rc, err.strip()[-120:])

def t_handshake_stall_times_out(env):
    """A server that stops during the handshake is given up on after ConnectTimeout."""
    port = free_port(); t = run_server_scenario(env, sc_stall, port); time.sleep(0.2)
    t0 = time.time()
    rc, out, err = run(mssh_cmd(env, port, extra=("-o", "ConnectTimeout=2")), timeout=20)
    dt = time.time() - t0
    return (rc == 1 and "timed out" in err and dt < 10), (rc, round(dt, 1), err.strip()[-120:])

def t_idle_session_not_timed(env):
    """The limit ends with the handshake: a quiet running session is not cut off."""
    port = free_port(); t = run_server_scenario(env, sc_idle_session, port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port, extra=("-o", "ConnectTimeout=2")), timeout=30)
    return (rc == 7 and "hello from test server" in out), (rc, err.strip()[-120:])

def t_question_not_timed(env):
    """Time spent answering the host key question does not count against the limit."""
    kh = os.path.join(env.dir, "kh_%d" % time.monotonic_ns()); port = free_port()
    t = run_server_scenario(env, sc_hello, port); time.sleep(0.2)
    rc, tr = pty_session(env, mssh_cmd(env, port, extra=("-o", "ConnectTimeout=2"), kh=kh), b"yes\n", delay=3)
    return (rc == 7 and "hello from test server" in tr), (rc, tr[-160:])

def t_connect_stall_times_out(env):
    """A connection that cannot be established (the listener's queue is full) is given up on."""
    ls = socket.socket(); ls.bind(("127.0.0.1", 0)); ls.listen(0); port = ls.getsockname()[1]
    held = []
    for _ in range(3):                      # fill the accept queue; nobody accepts
        c = socket.socket(); c.setblocking(False)
        try: c.connect(("127.0.0.1", port))
        except BlockingIOError: pass
        held.append(c)
    time.sleep(0.3)
    t0 = time.time()
    rc, out, err = run(mssh_cmd(env, port, extra=("-o", "ConnectTimeout=2")), timeout=20)
    dt = time.time() - t0
    for c in held: c.close()
    ls.close()
    return (rc == 1 and "connect to 127.0.0.1" in err and dt < 10), (rc, round(dt, 1), err.strip()[-120:])

def t_bad_connect_timeout(env):
    rc, out, err = run(mssh_cmd(env, free_port(), extra=("-o", "ConnectTimeout=soon")), timeout=15)
    return (rc == 1 and "bad ConnectTimeout" in err), (rc, err.strip()[-120:])

def t_keepalive_then_give_up(env):
    """Silence: a keepalive at 150 s, and the connection given up on at 180 s, as a closed one would be."""
    rec = {}; port = free_port()
    t = run_server_scenario(env, lambda c, hk: sc_keepalive_dead(c, hk, rec), port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port), timeout=240)
    t.join(10)
    good = (rc == 1 and "timed out" in err and "sent nothing for 180 seconds" in err
            and rec.get("req") == (b"keepalive@openssh.com", 1)
            and 145 <= rec.get("at", 0) <= 160 and 170 <= rec.get("end", 0) <= 195)
    return good, (rc, {k: round(v, 1) if isinstance(v, float) else v for k, v in rec.items()},
                  err.strip()[-160:])

def t_keepalive_restarted_by_data(env):
    """Data restarts the count (keepalive at 170 s, not 150 s); answering the keepalive restarts it again."""
    rec = {}; port = free_port()
    t = run_server_scenario(env, lambda c, hk: sc_keepalive_reset(c, hk, rec), port); time.sleep(0.2)
    rc, out, err = run(mssh_cmd(env, port), timeout=300)
    t.join(10)
    good = (rc == 7 and "tick" in out and "hello from test server" in out
            and 165 <= rec.get("at", 0) <= 178 and rec.get("req") == (b"keepalive@openssh.com", 1))
    return good, (rc, {k: round(v, 1) if isinstance(v, float) else v for k, v in rec.items()},
                  err.strip()[-160:])

KEEPALIVE_TESTS = [
    ("silent server: keepalive at 150 s, give up at 180 s", t_keepalive_then_give_up),
    ("data and answers restart the count",                   t_keepalive_restarted_by_data),
]

CLIENT_TESTS = [
    ("known_hosts unreadable is an error",   t_unreadable_known_hosts),
    ("first use: answer piped, refused",     t_piped_answer_is_refused),
    ("first use: typed yes connects",        t_typed_yes_on_terminal),
    ("first use: typed no refused",          t_typed_no_on_terminal),
    ("host name with brackets refused",      t_bracketed_host_refused),
    ("malformed known_hosts line skipped",   t_malformed_known_hosts_line),
    ("private key readable by others refused", t_key_mode_refused),
    ("handshake stall times out",            t_handshake_stall_times_out),
    ("running session is not timed",         t_idle_session_not_timed),
    ("host key question is not timed",       t_question_not_timed),
    ("connect stall times out",              t_connect_stall_times_out),
    ("bad ConnectTimeout refused",           t_bad_connect_timeout),
]

def main():
    if not os.path.exists(MSSH):
        print("no mssh binary at", MSSH, "(build it first)"); return 2
    env = Env(); ok = fail = 0
    print("## hostile server")
    for name, scen, want_rc, want_txt, opts in SERVER_TESTS:
        port = free_port()
        t = run_server_scenario(env, scen, port, **(opts or {}))
        time.sleep(0.2)
        rc, out, err = run(mssh_cmd(env, port), timeout=15)
        good = (rc == want_rc) and (want_txt in out + err)
        ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
        print("  %s %s  (rc=%s%s)" % ("PASS" if good else "FAIL", name, rc,
              "" if good else ", wanted %s and %r; stderr: %s" % (want_rc, want_txt, err.strip()[-160:])))
        t.join(0.1)
    print("## trust decisions on the client")
    for name, fn in CLIENT_TESTS:
        good, detail = fn(env)
        ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
        print("  %s %s  %s" % ("PASS" if good else "FAIL", name, "" if good else detail))
    if "--keepalive" in FLAGS:
        print("## keepalive (slow: waits out the real timers)")
        for name, fn in KEEPALIVE_TESTS:
            good, detail = fn(env)
            ok, fail = (ok + 1, fail) if good else (ok, fail + 1)
            print("  %s %s  %s" % ("PASS" if good else "FAIL", name, "" if good else detail))
    env.cleanup()
    print("\nSUMMARY: %d/%d passed" % (ok, ok + fail))
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
