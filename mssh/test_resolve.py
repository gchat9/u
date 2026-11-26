#!/usr/bin/env python3
# Tests of resolve.c (the resolver of mssh, with no libc resolver) on its own:
#     python3 mssh/test_resolve.py
# The code is compiled with the host's cc (with ASan and UBSan, if it has them) into a small program, which is
# run against a fake nameserver on the loopback that answers, or does not, in ways chosen by the name asked for.
# Needs: cc, python3.  Nothing outside the machine is asked.
import os, random, socket, struct, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(HERE, "..", "_lib", "sodium_lite")
T = tempfile.mkdtemp(prefix="resolve-test-")

DRIVER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resolve.h"
static int hex(const char *s, uint8_t *o, size_t max) { size_t n = strlen(s) / 2; if (n > max) n = max; for (size_t i = 0; i < n; i++) { unsigned v; sscanf(s + 2 * i, "%2x", &v); o[i] = (uint8_t)v; } return (int)n; }
static void show(int n, uint8_t (*a)[4]) {
    if (n < 0) { printf("ERR %d %s\n", n, resolve_strerror(n)); return; }
    printf("OK"); for (int i = 0; i < n; i++) printf(" %d.%d.%d.%d", a[i][0], a[i][1], a[i][2], a[i][3]); printf("\n");
}
int main(int argc, char **argv) {
    uint8_t a[64][4];
    if (!strcmp(argv[1], "resolve")) {       /* HOSTS RESOLV PORT BUDGET MAX NAME */
        show(resolve4_with(argv[7], a, atoi(argv[6]), atoi(argv[5]), argv[2], argv[3], atoi(argv[4])), a);
    } else if (!strcmp(argv[1], "query")) {  /* NAME ID */
        uint8_t q[DNS_QUERY_MAX]; size_t n = dns_query(q, argv[2], (uint16_t)atoi(argv[3]));
        if (!n) { printf("BAD\n"); return 0; } for (size_t i = 0; i < n; i++) printf("%02x", q[i]); printf("\n");
    } else if (!strcmp(argv[1], "answer")) { /* QUERYHEX RESPONSEHEX MAX */
        uint8_t q[512], m[4096]; int ql = hex(argv[2], q, sizeof q), ml = hex(argv[3], m, sizeof m);
        int n = dns_answer(m, (size_t)ml, q, (size_t)ql, a, atoi(argv[4]));
        if (n == RES_IGNORE) printf("IGNORE\n"); else show(n, a);
    } else if (!strcmp(argv[1], "fuzz")) {   /* SEED ITERATIONS QUERYHEX RESPONSEHEX */
        uint8_t q[512], m0[4096], m[4096]; int ql = hex(argv[4], q, sizeof q), ml = hex(argv[5], m0, sizeof m0);
        srand((unsigned)atoi(argv[2])); long it = atol(argv[3]), ok = 0;
        for (long i = 0; i < it; i++) {
            int len = ml; memcpy(m, m0, (size_t)ml);
            int k = 1 + rand() % 6; while (k--) {
                switch (rand() % 4) {
                case 0: m[rand() % len] = (uint8_t)rand(); break;
                case 1: m[rand() % len] ^= (uint8_t)(1 << (rand() % 8)); break;
                case 2: len = rand() % (len + 1); if (!len) len = 1; break;
                default: { int p = rand() % len; m[p] = (uint8_t)(rand() % 2 ? 0xC0 : rand() % 70); } }
            }
            int n = dns_answer(m, (size_t)len, q, (size_t)ql, a, 1 + rand() % 8);
            if (n >= 0 || n == RES_NOTFOUND || n == RES_SERVFAIL || n == RES_IGNORE) ok++; else { printf("BAD RETURN %d\n", n); return 1; }
        }
        printf("FUZZ %ld ok\n", ok);
    }
    return 0;
}
'''

def build():
    src = os.path.join(T, "driver.c"); open(src, "w").write(DRIVER)
    files = [src, os.path.join(HERE, "resolve.c"), os.path.join(LIB, "sodium_lite.c"), os.path.join(LIB, "randombytes.c")]
    base = ["cc", "-g", "-O1", "-Wall", "-Wextra", "-Wno-unused-result", "-I" + HERE, "-I" + LIB]
    out = os.path.join(T, "driver")
    for san in (["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []):
        r = subprocess.run(base + san + files + ["-o", out], capture_output=True, text=True)
        if r.returncode == 0: return out, bool(san)
    sys.exit("cannot build the test program:\n" + r.stderr)

# ── DNS messages (RFC 1035), by hand ─────────────────────────────────────────
def wire(name):
    return b"".join(bytes([len(l)]) + l.encode() for l in name.rstrip(".").split(".")) + b"\0"
def rr(name_or_ptr, typ, rdata, cls=1, ttl=60, rdlen=None):
    n = name_or_ptr if isinstance(name_or_ptr, bytes) else wire(name_or_ptr)
    return n + struct.pack(">HHIH", typ, cls, ttl, len(rdata) if rdlen is None else rdlen) + rdata
A = lambda ip: bytes(int(x) for x in ip.split("."))
PTR = b"\xc0\x0c"                              # a pointer to the name of the question (offset 12)
def reply(q, answers=(), rcode=0, tc=False, aa=True, id=None, qname=None, qr=True, opcode=0, ancount=None, extra=b""):
    qid = q[:2] if id is None else struct.pack(">H", id)
    flags = (0x8000 if qr else 0) | (opcode << 11) | (0x0400 if aa else 0) | (0x0200 if tc else 0) | 0x0100 | 0x0080 | rcode
    question = q[12:] if qname is None else wire(qname) + q[-4:]
    body = b"".join(answers) + extra
    return qid + struct.pack(">HHHHH", flags, 1, len(answers) if ancount is None else ancount, 0, 0) + question + body
def qname_of(q):
    i, labels = 12, []
    while q[i]: labels.append(q[i + 1:i + 1 + q[i]].decode()); i += 1 + q[i]
    return ".".join(labels)

class Server(threading.Thread):
    """A nameserver on the loopback: what it does depends on the name that is asked for."""
    def __init__(self, host="127.0.0.1", port=0):
        super().__init__(daemon=True)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self.sock.bind((host, port))
        self.port = self.sock.getsockname()[1]; self.seen = []; self.count = {}; self.host = host
    def run(self):
        while True:
            try: q, addr = self.sock.recvfrom(2048)
            except OSError: return
            name = qname_of(q).lower(); self.seen.append(name); self.count[name] = self.count.get(name, 0) + 1
            for r in self.respond(name, q, addr): self.sock.sendto(r, addr)
    def respond(self, name, q, addr):
        n = self.count[name]
        if name == "one.test": return [reply(q, [rr(PTR, 1, A("192.0.2.1"))])]
        if name == "three.test": return [reply(q, [rr(PTR, 1, A("192.0.2.%d" % i)) for i in (1, 2, 3)])]
        if name == "many.test": return [reply(q, [rr(PTR, 1, A("192.0.2.%d" % i)) for i in range(1, 13)])]
        if name == "cname.test": return [reply(q, [rr(PTR, 5, wire("real.cname.test")), rr(b"\xc0" + bytes([12 + 1 + 5 + 1 + 4 + 0]) if False else wire("real.cname.test"), 1, A("192.0.2.9"))])]
        if name == "ptrchain.test":     # the owner of the A record is a pointer to a pointer
            return [reply(q, [rr(PTR, 5, wire("t.test")), rr(b"\xc0\x1e", 1, A("192.0.2.7"))], extra=b"")]
        if name == "mixedcase.test": return [reply(q, [rr(PTR, 1, A("192.0.2.5"))], qname="MixedCase.TEST")]
        if name == "nx.test": return [reply(q, rcode=3)]
        if name == "servfail.test": return [reply(q, rcode=2)]
        if name == "refused.test": return [reply(q, rcode=5)]
        if name == "nodata.test": return [reply(q)]
        if name == "aaaa.test": return [reply(q, [rr(PTR, 28, bytes(15) + b"\1")])]
        if name == "txt.test": return [reply(q, [rr(PTR, 16, b"\x03abc"), rr(PTR, 1, A("192.0.2.4"))])]
        if name == "class.test": return [reply(q, [rr(PTR, 1, A("192.0.2.1"), cls=3), rr(PTR, 1, A("192.0.2.2"))])]
        if name == "rdlen3.test": return [reply(q, [rr(PTR, 1, b"\1\2\3"), rr(PTR, 1, A("192.0.2.2"))])]
        if name == "trunc.test": return [reply(q, tc=True)]
        if name == "truncpartial.test": return [reply(q, [rr(PTR, 1, A("192.0.2.3"))], tc=True)]
        if name == "wrongid.test":
            return [reply(q, [rr(PTR, 1, A("6.6.6.6"))], id=(q[0] << 8 | q[1]) ^ 1), reply(q, [rr(PTR, 1, A("192.0.2.1"))])]
        if name == "wrongq.test":
            return [reply(q, [rr(PTR, 1, A("6.6.6.6"))], qname="other.test"), reply(q, [rr(PTR, 1, A("192.0.2.1"))])]
        if name == "notresponse.test":
            return [reply(q, [rr(PTR, 1, A("6.6.6.6"))], qr=False), reply(q, [rr(PTR, 1, A("192.0.2.1"))])]
        if name == "forged.test":      # a packet that is right, from another port, comes first
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((self.host, 0))
            s.sendto(reply(q, [rr(PTR, 1, A("6.6.6.6"))]), addr); s.close(); time.sleep(0.05)
            return [reply(q, [rr(PTR, 1, A("192.0.2.1"))])]
        if name == "late.test": return [reply(q, [rr(PTR, 1, A("192.0.2.1"))])] if n >= 3 else []
        if name == "silent.test": return []
        if name == "badlen.test": return [reply(q, [rr(PTR, 1, A("1.2.3.4"), rdlen=200)])]
        if name == "shortrr.test": return [reply(q, [rr(PTR, 1, A("1.2.3.4"))[:-3]])]
        if name == "countlie.test": return [reply(q, [rr(PTR, 1, A("1.2.3.4"))], ancount=500)]
        if name == "loop.test": return [reply(q, [rr(b"\xc0" + bytes([12 + len(wire("loop.test")) + 4]), 1, A("192.0.2.8"))])]
        if name == "opcode.test": return [reply(q, [rr(PTR, 1, A("1.2.3.4"))], opcode=4)]
        if name == "badlabel.test": return [reply(q, [b"\x80\x00" + struct.pack(">HHIH", 1, 1, 0, 4) + A("1.2.3.4")])]
        if name == "garbage.test": return [q[:2] + os.urandom(40)]
        return [reply(q, rcode=3)]

def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p

ok = fail = 0
def check(name, cond, info=""):
    global ok, fail
    if cond: ok += 1
    else: fail += 1
    print("  %s %s %s" % ("PASS" if cond else "FAIL", name, "" if cond else info))
def write(name, text):
    p = os.path.join(T, name); open(p, "wb").write(text if isinstance(text, bytes) else text.encode()); return p

def main():
    drv, sanitized = build()
    print("(test program built %s)" % ("with ASan and UBSan" if sanitized else "without sanitizers"))
    srv = Server(); srv.start()
    hosts0 = write("hosts0", "")
    def conf(*ns): return write("resolv.conf", "".join("nameserver %s\n" % n for n in ns))
    def res(name, hosts=None, resolv=None, port=None, budget=0, mx=8):
        t0 = time.time()
        r = subprocess.run([drv, "resolve", hosts or hosts0, resolv or conf("127.0.0.1"), str(port or srv.port), str(budget), str(mx), name], capture_output=True, text=True, timeout=30)
        return (r.stdout.strip() or "CRASH " + r.stderr[-300:]), time.time() - t0
    R = lambda *a, **k: res(*a, **k)[0]

    print("-- a name that is four numbers is itself, and nothing is asked")
    before = len(srv.seen)
    for n, want in (("192.0.2.1", "OK 192.0.2.1"), ("0.0.0.0", "OK 0.0.0.0"), ("255.255.255.255", "OK 255.255.255.255"), ("10.0.0.10", "OK 10.0.0.10")):
        check("%s" % n, R(n) == want, R(n))
    check("no query was made for them", len(srv.seen) == before, str(srv.seen[before:]))
    print("-- what is not a number is asked for (so the servers see it)")
    for n in ("256.1.1.1", "01.2.3.4", "1.2.3", "1.2.3.4.5", "1.2.3.", "1..2.3", "a.b.c.d", "1.2.3.4x", "0x7f.0.0.1"):
        b = len(srv.seen); r = R(n)
        check("%-12s is looked up (%s)" % (n, r), len(srv.seen) > b or r.startswith("ERR -1"), r)
    print("-- IPv6 and names that are not")
    check("::1 is IPv6", R("::1").startswith("ERR -2"), R("::1"))
    check("fe80::1 is IPv6", R("fe80::1").startswith("ERR -2"))
    b = len(srv.seen)
    for n, why in (("a..b", "empty label"), (".a", "leading dot"), (".", "only a dot"), ("a" * 64 + ".x", "label of 64"), (".".join(["a" * 63] * 4 + ["b"]), "name over 255")):
        check("%s: %s" % (n[:20], why), R(n).startswith("ERR -1"), R(n))
    check("a label of 63 is a name", R("a" * 63 + ".test").startswith("ERR -3"), R("a" * 63 + ".test"))
    check("a name of 255 bytes on the wire is a name", R(".".join(["a" * 63] * 3 + ["b" * 61])).startswith("ERR -3"), R(".".join(["a" * 63] * 3 + ["b" * 61])))
    print("-- /etc/hosts")
    h = write("hosts", "# a comment\n127.0.0.1\tlocalhost  Localhost.localdomain\n::1 ip6-localhost\n192.0.2.10 web.test www.test # note\n192.0.2.11 WEB.test\n 192.0.2.12   spaced.test\r\n192.0.2.13 notnamed\n10.0.0.1 a.test\n10.0.0.2 a.test\n" + "# " + "x" * 3000 + " 192.0.2.99 long.test\n192.0.2.14 last.test")
    for n, want in (("localhost", "OK 127.0.0.1"), ("LOCALHOST", "OK 127.0.0.1"), ("localhost.localdomain", "OK 127.0.0.1"), ("www.test", "OK 192.0.2.10"),
                    ("web.test", "OK 192.0.2.10 192.0.2.11"), ("spaced.test", "OK 192.0.2.12"), ("a.test", "OK 10.0.0.1 10.0.0.2"), ("last.test", "OK 192.0.2.14")):
        check("hosts: %s" % n, R(n, hosts=h) == want, R(n, hosts=h))
    check("hosts: an IPv6 line is not used", R("ip6-localhost", hosts=h).startswith("ERR -3"), R("ip6-localhost", hosts=h))
    check("hosts: a comment is not a name", R("note", hosts=h).startswith("ERR -3"))
    check("hosts: the address of a line is not a name", R("192.0.2.99", hosts=h) == "OK 192.0.2.99")
    check("hosts: a line that is too long is skipped", R("long.test", hosts=h).startswith("ERR -3"))
    check("hosts: no file is not an error", R("one.test", hosts=os.path.join(T, "none")) == "OK 192.0.2.1")
    check("hosts: wins over the nameserver", R("one.test", hosts=write("h2", "198.51.100.1 one.test\n")) == "OK 198.51.100.1")
    check("hosts: at most max", R("web.test", hosts=h, mx=1) == "OK 192.0.2.10", R("web.test", hosts=h, mx=1))
    print("-- /etc/resolv.conf (three nameservers, on 127.0.0.1, .2 and .3: which one is asked)")
    s2, s3 = Server("127.0.0.2", srv.port), Server("127.0.0.3", srv.port); s2.start(); s3.start()
    def who(text):
        for s in (srv, s2, s3): s.seen.clear()
        R("one.test", resolv=write("resolv.conf", text)); return [i for i, s in enumerate((srv, s2, s3), 1) if s.seen]
    check("the first nameserver", who("nameserver 127.0.0.2\nnameserver 127.0.0.3\n") == [2], str(who("nameserver 127.0.0.2\nnameserver 127.0.0.3\n")))
    check("an IPv6 one is passed over", who("nameserver ::1\nnameserver fe80::1\nnameserver 127.0.0.3\nnameserver 127.0.0.2\n") == [3])
    check("comments, tabs, other lines", who("# nameserver 127.0.0.2\n; x\nsearch example.org\noptions timeout:1\nnameserver\t127.0.0.3  \n") == [3])
    check("no nameserver line: 127.0.0.1", who("search example.org\n") == [1])
    check("no file: 127.0.0.1", (lambda: (srv.seen.clear(), subprocess.run([drv, "resolve", hosts0, os.path.join(T, "none"), str(srv.port), "0", "8", "one.test"], capture_output=True), srv.seen == ["one.test"])[2])())
    check("'nameserver' alone is nothing", who("nameserver\nnameserver 127.0.0.3\n") == [3])
    check("a bad address is passed over", who("nameserver 127.0.0.300\nnameserver 127.0.0.2\n") == [2])
    check("a bad address with none after it: 127.0.0.1, unchanged", who("nameserver 10.1.2.x\n") == [1], str(who("nameserver 10.1.2.x\n")))
    print("-- what the nameserver says")
    for name, want in (("one.test", "OK 192.0.2.1"), ("three.test", "OK 192.0.2.1 192.0.2.2 192.0.2.3"), ("many.test", "OK " + " ".join("192.0.2.%d" % i for i in range(1, 9))),
                       ("cname.test", "OK 192.0.2.9"), ("mixedcase.test", "OK 192.0.2.5"), ("txt.test", "OK 192.0.2.4"), ("class.test", "OK 192.0.2.2"),
                       ("rdlen3.test", "OK 192.0.2.2"), ("truncpartial.test", "OK 192.0.2.3"), ("loop.test", "OK 192.0.2.8"),
                       ("nx.test", "ERR -3 no such host"), ("nodata.test", "ERR -3 no such host"), ("aaaa.test", "ERR -3 no such host"),
                       ("servfail.test", "ERR -5 the nameserver's answer is an error"), ("refused.test", "ERR -5 the nameserver's answer is an error"),
                       ("trunc.test", "ERR -5 the nameserver's answer is an error"), ("badlen.test", "ERR -5 the nameserver's answer is an error"),
                       ("shortrr.test", "ERR -5 the nameserver's answer is an error"), ("countlie.test", "ERR -5 the nameserver's answer is an error"),
                       ("opcode.test", "ERR -5 the nameserver's answer is an error"), ("badlabel.test", "ERR -5 the nameserver's answer is an error")):
        check("%-18s -> %s" % (name, want[:34]), R(name) == want, R(name))
    check("max 3 of 12 addresses", R("many.test", mx=3) == "OK 192.0.2.1 192.0.2.2 192.0.2.3", R("many.test", mx=3))
    print("-- packets that are not the answer are not taken (and the right one after them is)")
    for name in ("wrongid.test", "wrongq.test", "notresponse.test", "forged.test"):
        check("%-18s -> the right address" % name, R(name) == "OK 192.0.2.1", R(name))
    print("-- time")
    r, dt = res("silent.test", budget=600)
    check("silence: a timeout after about the budget (%.2fs)" % dt, r.startswith("ERR -4") and 0.4 < dt < 1.5, "%s %.2f" % (r, dt))
    srv.count.clear(); r, dt = res("late.test", budget=3000)
    check("two queries are lost: the third is answered (%.2fs, %d asked)" % (dt, srv.count.get("late.test", 0)), r == "OK 192.0.2.1" and srv.count["late.test"] == 3, "%s %s" % (r, srv.count))
    r, dt = res("silent.test", budget=0)
    check("no budget: three times two seconds (%.1fs)" % dt, r.startswith("ERR -4") and 5.5 < dt < 8, "%s %.1f" % (r, dt))
    r, dt = res("garbage.test", budget=600)
    check("garbage that has our ID: not understood or not ours (%s)" % r, r.startswith("ERR -5") or r.startswith("ERR -4"), r)
    r, dt = res("one.test", port=free_port())
    check("nobody listens: the reason, at once (%.2fs)" % dt, r.startswith("ERR -6") and "refused" in r.lower() and dt < 1, "%s %.2f" % (r, dt))
    print("-- the packets")
    q = subprocess.run([drv, "query", "www.Example.org", "4660"], capture_output=True, text=True).stdout.strip()
    want = (struct.pack(">HHHHHH", 0x1234, 0x0100, 1, 0, 0, 0) + wire("www.Example.org") + struct.pack(">HH", 1, 1)).hex()
    check("the query for www.Example.org", q == want, q + "\n" + want)
    check("a dot at the end is the same name", subprocess.run([drv, "query", "a.b.", "1"], capture_output=True, text=True).stdout == subprocess.run([drv, "query", "a.b", "1"], capture_output=True, text=True).stdout)
    qq = bytes.fromhex(q)
    for name, msg, want in (("an answer", reply(qq, [rr(PTR, 1, A("1.2.3.4"))]), "OK 1.2.3.4"), ("the ID is not ours", reply(qq, id=1), "IGNORE"),
                            ("a short packet", b"\x12\x34", "IGNORE"), ("not a response", reply(qq, qr=False), "IGNORE"),
                            ("the question is cut", reply(qq)[:20], "IGNORE")):
        r = subprocess.run([drv, "answer", q, msg.hex(), "8"], capture_output=True, text=True).stdout.strip()
        check("dns_answer: %s" % name, r == want, r)
    print("-- bytes that are not a packet (ASan and UBSan: no read outside, no crash)")
    good = reply(qq, [rr(PTR, 5, wire("x.y")), rr(PTR, 1, A("1.2.3.4")), rr(PTR, 1, A("5.6.7.8"))])
    r = subprocess.run([drv, "fuzz", "1", "300000", q, good.hex()], capture_output=True, text=True, timeout=300)
    check("300000 changed answers: %s" % (r.stdout.strip() or r.stderr[-200:]), r.returncode == 0 and r.stdout.startswith("FUZZ 300000"), r.stderr[-300:])
    print("\n%d passed, %d failed" % (ok, fail))
    sys.exit(1 if fail else 0)

if __name__ == "__main__":
    main()
