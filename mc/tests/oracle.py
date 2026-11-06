"""What an archive holds, as the standard library reads it, and the walk that
checks the panel against it, directory by directory."""
import io
import subprocess
import tarfile
import zipfile

from term import DOWN, ENTER, UP, Term


def normal(name):
    """A member's name inside the archive, as mc lists it: the leading "/" and
    "./" dropped, a trailing "/" dropped. Names over 255 bytes are not listed."""
    while name.startswith("/") or name.startswith("./"):
        name = name[1:] if name.startswith("/") else name[2:]
    if name.endswith("/"):
        name = name[:-1]
    return name if 0 < len(name) <= 255 else None


def members_of_tar(path):
    if path.endswith((".zst", ".zstd")):
        data = subprocess.run(["zstd", "-dc", path], capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(data)) as tf:
            return _tar_members(tf)
    with tarfile.open(path, "r:*") as tf:
        return _tar_members(tf)


def _tar_members(tf):
    out = []
    for m in tf:
        name = normal(m.name)
        if name is None:
            continue
        kind = "dir" if m.isdir() else "link" if m.issym() else "file"
        out.append((name, kind, m.size))
    return out


def members_of_zip(path):
    out = []
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            name = normal(info.filename)
            if name is None:
                continue
            kind = "dir" if info.filename.endswith("/") else "file"
            out.append((name, kind, info.file_size))
    return out


def file_bytes(path):
    """The content of each file of an archive: name to bytes."""
    out = {}
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                name = normal(info.filename)
                if name is not None and not info.filename.endswith("/"):
                    out[name] = z.read(info)
        return out
    if path.endswith((".zst", ".zstd")):
        data = subprocess.run(["zstd", "-dc", path], capture_output=True, check=True).stdout
        tf = tarfile.open(fileobj=io.BytesIO(data))
    else:
        tf = tarfile.open(path, "r:*")
    with tf:
        for m in tf:
            name = normal(m.name)
            if name is not None and m.isfile():
                out[name] = tf.extractfile(m).read()
    return out


def children(members, prefix):
    """The entries directly inside directory PREFIX ("" or "a/b/"): name to
    (kind, size). A name with an empty, "." or ".." component is not listed."""
    out = {}
    for name, kind, size in members:
        if not name.startswith(prefix):
            continue
        rest = name[len(prefix):]
        if not rest:
            continue
        comp, slash, _ = rest.partition("/")
        if comp in ("", ".", ".."):
            continue
        if slash or kind == "dir":
            out.setdefault(comp, ("dir", 0))
        else:
            out.setdefault(comp, (kind, size))
    return out


def shown(kind, size):
    """What the panel shows for an entry: its mark, and its size."""
    if kind == "dir":
        return "/", "0"
    if kind == "link":
        return "@", "0"
    return " ", str(size)


def same(shown_name, want):
    """mc cuts a long name in the middle with '~', so compare its ends."""
    if len(want) <= 26:
        return shown_name == want
    return shown_name[:12] == want[:12] and shown_name[-12:] == want[-12:]


def walk(tc, t, arc, members, prefix="", visited=None):
    """Check the listing of directory PREFIX, then every directory under it.
    TC is the test case; T is the terminal, inside the archive."""
    visited = set() if visited is None else visited
    utar = "/uzip://" if arc.endswith(".zip") else "/utar://"
    title = arc + utar + prefix.rstrip("/")
    tc.assertIn(title[:30], t.line(0), "title for " + prefix)

    want = children(members, prefix)
    rows = [(n, s) for n, s in t.entries() if not n.startswith("/..")]
    tc.assertEqual(len(rows), len(want), "entries in %r: %r" % (prefix, rows))
    for shown_name, size in rows:
        name = next((k for k in want if same(shown_name[1:], k) or same(shown_name, k)), None)
        tc.assertIsNotNone(name, "unexpected entry %r in %r" % (shown_name, prefix))
        mark, want_size = shown(*want[name])
        tc.assertEqual(shown_name[0], mark, "mark of %s%s" % (prefix, name))
        tc.assertEqual(size, want_size, "size of %s%s" % (prefix, name))

    for i, (shown_name, _) in enumerate(t.entries()):
        if not shown_name.startswith("/") or shown_name.startswith("/.."):
            continue
        name = next((k for k in want if want[k][0] == "dir" and same(shown_name[1:], k)), None)
        if name is None or prefix + name in visited:
            continue
        visited.add(prefix + name)
        t.goto(i)
        t.send(ENTER, 0.9)
        walk(tc, t, arc, members, prefix + name + "/", visited)
        t.goto(0)
        t.send(ENTER, 0.9)
        tc.assertEqual(t.cursor_name(), shown_name, "cursor after .. from " + name)
    return visited


def enter_archive(tc, t, name):
    """Enter the archive called NAME, in the panel's directory."""
    i = t.find(name)
    tc.assertIsNotNone(i, "archive %s is not listed" % name)
    t.goto(i)
    t.send(ENTER, 2.0)
    tc.assertTrue(in_archive(t), "did not enter %s: %r" % (name, t.line(0)[:60]))


def in_archive(t):
    return "utar://" in t.line(0) or "uzip://" in t.line(0)
