"""The archives and files the tests run on, built from Python. Zstandard needs
the zstd tool; everything else is in the standard library."""
import gzip
import io
import lzma
import os
import shutil
import subprocess
import tarfile
import zipfile

MTIME = 1700000000

# Each form of a tar archive: its name's ending, and the compression it has.
TAR_FORMS = {
    "tar": (".tar", None),
    "tgz": (".tgz", "gz"),
    "tar.gz": (".tar.gz", "gz"),
    "txz": (".txz", "xz"),
    "tar.xz": (".tar.xz", "xz"),
    "tar.zst": (".tar.zst", "zst"),
    "tar.zstd": (".tar.zstd", "zst"),
}


def have(tool):
    return shutil.which(tool) is not None


def put(path, data=b""):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def zip_archive(path, files, dirs=(), dir_entries=True):
    """A zip of FILES, a dict of name to bytes; DIRS are written as directory
    entries when DIR_ENTRIES, as zip -r does, and left out otherwise."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        if dir_entries:
            for d in dirs:
                z.writestr(d.rstrip("/") + "/", b"")
        for name, data in files.items():
            z.writestr(name, data)
    return path


def tar_bytes(members, fmt="gnu"):
    """A tar stream. MEMBERS: (name, kind, data, target), kind 'file', 'dir',
    'link' (symbolic) or 'hard'."""
    buf = io.BytesIO()
    fmt_const = {"gnu": tarfile.GNU_FORMAT, "pax": tarfile.PAX_FORMAT,
                 "ustar": tarfile.USTAR_FORMAT}[fmt]
    with tarfile.open(fileobj=buf, mode="w", format=fmt_const) as tf:
        for name, kind, data, target in members:
            ti = tarfile.TarInfo(name)
            ti.mtime = MTIME
            if kind == "dir":
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                tf.addfile(ti)
            elif kind == "link":
                ti.type = tarfile.SYMTYPE
                ti.linkname = target
                tf.addfile(ti)
            elif kind == "hard":
                ti.type = tarfile.LNKTYPE
                ti.linkname = target
                tf.addfile(ti)
            else:
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def compress(data, how):
    if how is None:
        return data
    if how == "gz":
        return gzip.compress(data, mtime=0)
    if how == "xz":
        return lzma.compress(data, format=lzma.FORMAT_XZ)
    if how == "zst":
        return subprocess.run(["zstd", "-q", "-c"], input=data, capture_output=True,
                              check=True).stdout
    raise ValueError(how)


def tar_archive(dirpath, base, members, form, fmt="gnu"):
    """Write BASE in FORM (a key of TAR_FORMS) into DIRPATH; returns its path."""
    ending, how = TAR_FORMS[form]
    return put(os.path.join(dirpath, base + ending), compress(tar_bytes(members, fmt), how))


# The members of the basic fixture: what a directory listing has to show.
BASIC = [
    ("./", "dir", b"", None),
    ("top.txt", "file", b"top file\n", None),
    ("docs/", "dir", b"", None),
    ("docs/readme.md", "file", b"# readme\n", None),
    ("docs/deep/", "dir", b"", None),
    ("docs/deep/nested.txt", "file", b"nested\n", None),
    ("empty/", "dir", b"", None),
    ("link", "link", b"", "docs/readme.md"),
    ("big.bin", "file", bytes(range(256)) * 256, None),
]

# The adversarial fixture: long names (GNU long-name records, or pax), names with
# spaces and unicode, an absolute name, a name with too many characters to keep,
# a link whose target is long, a hard link, an empty file, and a link in a
# directory. Oracle-checked: see oracle.py.
LONG_DIR = "L" * 150
TOO_LONG = "V" * 260
LONG_TARGET = "t" * 140
ADVERSARIAL = [
    ("./", "dir", b"", None),
    ("adir/", "dir", b"", None),
    ("adir/inner.txt", "file", b"inner\n", None),
    ("adir/deeper/", "dir", b"", None),
    ("adir/deeper/x y.txt", "file", b"space\n", None),
    ("adir/rel-link", "link", b"", "inner.txt"),
    ("a b/c d.txt", "file", b"spaces in names\n", None),
    ("u\u00f1i/\u00fcn\u00efcode.txt", "file", b"utf8\n", None),
    (LONG_DIR + "/file-in-long-dir.txt", "file", b"long dir\n", None),
    (TOO_LONG + "/dropped.txt", "file", b"way too long\n", None),
    ("/abs/absolute-name.txt", "file", b"leading slash\n", None),
    ("empty.txt", "file", b"", None),
    ("top.txt", "file", b"top\n", None),
    ("lnk", "link", b"", LONG_TARGET),
    ("hard.txt", "hard", b"", "top.txt"),
]

# A zip with the stored directories of zip -r, and one without any.
ZIP_NOTES = {
    "notes.txt": b"Hello from notes\nsecond line\n",
    "todo.txt": b"TODO: write more\n",
    "readme.txt": b"Read me first\n",
    "src/log.txt": b"log entry 1\n",
    "src/main.c": b"int main(void){return 0;}\n",
}
ZIP_IMPLICIT = {
    "a.txt": b"alpha\n",
    "b b.txt": b"beta beta\n",
    "d1/d2/deep.txt": b"deep\n",
    "d1/x.log": b"x\n",
    "d1/y.log": b"y\n",
    "zz.md": b"top\n",
}
ZIP_DEEP = {
    "a/b/c/d/leaf.txt": b"leaf\n",
    "a/b/c/side.txt": b"side\n",
    "a/b/other/x.txt": b"x\n",
    "a/top.txt": b"t\n",
    "zeta/z.txt": b"z\n",
    "alpha.txt": b"a\n",
}
ZIP_DOTTED = {          # members named with "./", as some tools write them
    "./readme.txt": b"x\n",
    "./bin/tool": b"y\n",
    "deep/a/b/c/leaf.txt": b"deep\n",
    "many/file000.txt": b"n\n",
}


def zip_big(path, count=120):
    return zip_archive(path, {"item%03d.dat" % i: b"x" * (i % 40) for i in range(count)},
                       dir_entries=False)


def tree_files(root, names):
    """Make empty files NAMES (paths relative to ROOT)."""
    for n in names:
        put(os.path.join(root, n))
    return root
