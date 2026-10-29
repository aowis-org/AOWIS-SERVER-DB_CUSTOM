#!/usr/bin/env python3
"""Offline, verified, all-project native AOWIS storage-root backup/restore.

The source must be quiescent. Lock both the catalogue and every repository's
lifetime lock so direct native-library users cannot be silently copied live.
Only a fully verified staging tree is atomically published at its final path.
Linux only (flock, renameat2/RENAME_NOREPLACE and directory fsync).
"""

import argparse
import contextlib
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


PROJECT_ID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z")
UUID7 = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z")
HEAD = re.compile(rb"(?!0{64}\n)[0-9a-f]{64}\n\Z")


class BackupError(Exception):
    pass


def valid_branch_label(label):
    # Match the native UUID-keyed branch label contract (UTF-8 byte limit,
    # internal spaces, no controls or URL/path delimiters).
    if not 1 <= len(label) <= 96 or label in (b".", b"..") or label.startswith(b" ") or label.endswith(b" "):
        return False
    try:
        text = label.decode("utf-8")
    except UnicodeDecodeError:
        return False
    return all(ord(character) >= 0x20 and ord(character) != 0x7f
               and character not in "/\\%?#" for character in text)


def require(condition, message):
    if not condition:
        raise BackupError(message)


def safe_directory(path):
    require(path.is_dir() and not path.is_symlink(), f"unsafe or absent directory: {path}")


def projects_in(root):
    safe_directory(root)
    safe_directory(root / "projects")
    result = []
    for project in sorted((root / "projects").iterdir()):
        if project.name.startswith(".creating-"):
            require(project.is_dir() and not project.is_symlink(), f"unsafe staging project: {project}")
            continue
        require(PROJECT_ID.fullmatch(project.name) is not None, f"invalid project entry: {project}")
        safe_directory(project)
        safe_directory(project / "repository")
        meta = project / "project.meta"
        require(meta.is_file() and not meta.is_symlink(), f"invalid project manifest: {meta}")
        content = meta.read_bytes().splitlines()
        require(len(content) == 4 and content[0] == b"AOWIS-PROJECT-1"
                and content[1] == project.name.encode("ascii") and content[2] == b"pack",
                f"invalid project identity: {project}")
        result.append(project)
    return result


def repository_identity(root):
    # Legacy catalogues have no ID until a ProjectService first opens them.
    # A present identity must be valid; never replace malformed metadata.
    path = root / "repository.id"
    if not path.exists() and not path.is_symlink():
        return None
    require(path.is_file() and not path.is_symlink(), f"unsafe repository identity: {path}")
    payload = path.read_bytes()
    parts = payload.splitlines(keepends=True)
    require(len(parts) == 2 and parts[0] == b"AOWIS-REPOSITORY-1\n"
            and parts[1].endswith(b"\n") and
            UUID7.fullmatch(parts[1][:-1].decode("ascii", errors="replace")) is not None,
            f"invalid DVMDB repository identity: {path}")
    return parts[1][:-1]


def tracking_metadata(root):
    """Validate optional VC-03 tracking schema; preserve its exact bytes."""
    path = root / "repository.tracking"
    if not path.exists() and not path.is_symlink():
        return None
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= 8 * 1024 * 1024,
            f"unsafe or oversized repository tracking metadata: {path}")
    payload = path.read_bytes()
    require(payload.endswith(b"\n"), f"truncated repository tracking metadata: {path}")
    lines = payload.decode("ascii", errors="replace").splitlines()
    require(len(lines) >= 2 and lines[0] == "AOWIS-TRACKING-1" and
            lines[1] in ("role\tunconfigured", "role\tprivate", "role\tshared"),
            f"invalid repository tracking header: {path}")
    local_id = repository_identity(root)
    remote_ids = set()
    remote_endpoints = set()
    tracking_keys = set()
    for line in lines[2:]:
        fields = line.split("\t")
        if len(fields) == 3 and fields[0] == "remote":
            remote_id, encoded = fields[1:]
            require(UUID7.fullmatch(remote_id) is not None and
                    remote_id != (local_id or b"").decode("ascii") and
                    remote_id not in remote_ids and len(remote_ids) < 64 and
                    len(encoded) % 2 == 0 and len(encoded) <= 4096 and
                    re.fullmatch("[0-9a-f]*", encoded) is not None,
                    f"invalid remote registry entry: {path}")
            endpoint = bytes.fromhex(encoded)
            require(1 <= len(endpoint) <= 2048 and
                    endpoint.startswith((b"http://", b"https://", b"file:///")) and
                    all(0x20 < ch < 0x7f and ch not in (ord('@'), ord('#'), ord('\\'))
                        for ch in endpoint) and
                    endpoint not in (b"http://", b"https://", b"file:///") and
                    (endpoint.startswith(b"file:///") or
                     endpoint.split(b"://", 1)[1][:1] not in (b"/", b":", b"?")),
                    f"invalid remote endpoint: {path}")
            require(endpoint not in remote_endpoints,
                    f"remote endpoint registered under multiple UUIDs: {path}")
            remote_ids.add(remote_id)
            remote_endpoints.add(endpoint)
        elif len(fields) == 5 and fields[0] == "ref":
            remote_id, project_id, branch_id, head = fields[1:]
            key = remote_id, project_id, branch_id
            require(UUID7.fullmatch(remote_id) is not None and
                    UUID7.fullmatch(project_id) is not None and
                    UUID7.fullmatch(branch_id) is not None and
                    (head == "-" or UUID7.fullmatch(head) is not None) and
                    key not in tracking_keys and len(tracking_keys) < 10000,
                    f"invalid remote tracking ref: {path}")
            tracking_keys.add(key)
        else:
            raise BackupError(f"unknown repository tracking record: {path}")
    require(all(key[0] in remote_ids for key in tracking_keys),
            f"unregistered remote in repository tracking: {path}")
    return payload


def working_copy_records(root):
    """Validate and compare the exact bytes of private working-copy records."""
    directory = root / "working-copies"
    if not directory.exists() and not directory.is_symlink():
        return {}
    safe_directory(directory)
    records = {}
    for path in sorted(directory.iterdir()):
        if path.name.startswith(".creating-"):
            require(UUID7.fullmatch(path.name[len(".creating-"):]) is not None
                    and path.is_file() and not path.is_symlink(),
                    f"unsafe working-copy staging record: {path}")
            continue
        require(UUID7.fullmatch(path.name) is not None
                and path.is_file() and not path.is_symlink()
                and path.stat().st_size <= 512,
                f"invalid working-copy record: {path}")
        body = path.read_bytes()
        lines = body.splitlines(keepends=True)
        require(len(lines) == 9 and lines[0] == b"AOWIS-WORKING-COPY-1\n" and
                all(line.endswith(b"\n") for line in lines),
                f"invalid working-copy record format: {path}")
        fields = [b"project", b"branch", b"base", b"session", b"generation",
                  b"dirty", b"validation", b"changes"]
        values = []
        for line, field in zip(lines[1:], fields):
            parts = line[:-1].split(b"\t")
            require(len(parts) == 2 and parts[0] == field,
                    f"invalid working-copy field: {path}")
            values.append(parts[1])
        uuid = lambda value: UUID7.fullmatch(value.decode("ascii", errors="replace")) is not None
        require(uuid(values[0]) and uuid(values[1]) and
                (values[2] == b"-" or uuid(values[2])) and uuid(values[3]) and
                values[4:] == [b"0", b"0", b"unvalidated", b"0"],
                f"invalid initial working-copy state: {path}")
        records[path.name] = body
    return records


def branch_heads(root):
    heads = {}
    for project in projects_in(root):
        branch_dir = project / "repository" / "refs" / "heads"
        safe_directory(branch_dir)
        values = {}
        for entry in branch_dir.iterdir():
            # Only canonical, unpublished UUIDv7 staging files may be skipped.
            # All other entries (including dotfiles) are corruption, not branches.
            if entry.name.startswith(".stage-"):
                require(UUID7.fullmatch(entry.name[7:]) is not None
                        and entry.is_file() and not entry.is_symlink(),
                        f"invalid branch staging file: {entry}")
                continue
            require(UUID7.fullmatch(entry.name) is not None
                    and entry.is_file() and not entry.is_symlink(),
                    f"invalid UUIDv7 branch filename: {entry}")
            content = entry.read_bytes()
            parts = content.splitlines(keepends=True)
            require(parts and parts[0] in (b"AOWIS-BRANCH-1\n", b"AOWIS-BRANCH-2\n",
                                               b"AOWIS-BRANCH-3\n", b"AOWIS-BRANCH-4\n",
                                               b"AOWIS-BRANCH-5\n"),
                    f"invalid UUIDv7 branch record marker: {entry}")
            marker = parts[0]
            require(len(parts) == (3 if marker == b"AOWIS-BRANCH-1\n" else
                                   5 if marker == b"AOWIS-BRANCH-3\n" else
                                   6 if marker == b"AOWIS-BRANCH-5\n" else 4)
                    and parts[1].endswith(b"\n")
                    and valid_branch_label(parts[1][:-1]),
                    f"invalid UUIDv7 branch record name or length: {entry}")
            if marker == b"AOWIS-BRANCH-4\n":
                require(parts[2:] == [b"-\n", b"-\n"],
                        f"invalid unborn UUIDv7 branch record: {entry}")
            elif marker != b"AOWIS-BRANCH-1\n":
                require(parts[2].endswith(b"\n") and
                        UUID7.fullmatch(parts[2][:-1].decode("ascii", errors="replace")) is not None,
                        f"invalid UUIDv7 branch HEAD identity: {entry}")
            if marker in (b"AOWIS-BRANCH-3\n", b"AOWIS-BRANCH-5\n"):
                require(parts[3].endswith(b"\n") and
                        UUID7.fullmatch(parts[3][:-1].decode("ascii", errors="replace")) is not None
                        and parts[3][:-1].decode("ascii") != entry.name,
                        f"invalid UUIDv7 branch creation origin: {entry}")
            if marker == b"AOWIS-BRANCH-5\n":
                require(parts[4].endswith(b"\n") and
                        UUID7.fullmatch(parts[4][:-1].decode("ascii", errors="replace")) is not None,
                        f"invalid immutable UUIDv7 branch fork revision: {entry}")
            require(marker == b"AOWIS-BRANCH-4\n" or HEAD.fullmatch(parts[-1]) is not None,
                    f"invalid UUIDv7 branch HEAD digest: {entry}")
            values[entry.name] = content
        require(values, f"project has no published branches: {project.name}")
        heads[project.name] = (project.joinpath("project.meta").read_bytes(), values)
    return heads


def lock_file(path, stack):
    fd = os.open(path, os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW)
    stack.callback(os.close, fd)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as error:
        raise BackupError(f"source is active (stop every server/library user): {path}") from error


def acquire_source_locks(root, stack):
    # A ProjectService holds the catalogue lock during its entire lifetime.
    lock_file(root / "catalogue.lock", stack)
    repository_identity(root)
    tracking_metadata(root)
    working_copy_records(root)
    for project in projects_in(root):
        # Every Repository holds maintenance.lock shared throughout its lifetime.
        lock_file(project / "repository" / "maintenance.lock", stack)
    return branch_heads(root)


def reject_links_and_special(root):
    for current, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(current) / name
            mode = path.lstat().st_mode
            require(stat.S_ISDIR(mode) or stat.S_ISREG(mode),
                    f"symlink or special filesystem entry in source: {path}")


def ensure_distinct(source, dest):
    source_real = source.resolve(strict=True)
    dest_real = dest.parent.resolve(strict=True) / dest.name
    require(source_real != dest_real and source_real not in dest_real.parents
            and dest_real not in source_real.parents,
            "destination and source must be separate directory trees")
    require(not dest.exists() and not dest.is_symlink(), f"destination already exists: {dest}")


def sync_tree(root):
    # fsync every copied file and directory before publishing a durable backup.
    for current, dirs, files in os.walk(root, topdown=False):
        for filename in files:
            fd = os.open(Path(current) / filename, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        for directory in dirs:
            sync_dir(Path(current) / directory)
    sync_dir(root)


def sync_dir(directory):
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def publish_no_replace(source, target):
    libc = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(libc, "renameat2", None)
    require(renameat2 is not None, "atomic no-overwrite directory rename unavailable")
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    result = renameat2(-100, os.fsencode(source), -100, os.fsencode(target), 1)
    if result != 0:
        code = ctypes.get_errno()
        raise OSError(code, f"atomic backup publication failed: {os.strerror(code)}", str(target))
    sync_dir(target.parent)


def verify(root, verify_binary):
    # Native verification may rebuild derived indexes; run it only on staging.
    repository_identity(root)
    tracking_metadata(root)
    working_copy_records(root)
    expected = branch_heads(root)
    for project in projects_in(root):
        result = subprocess.run([str(verify_binary), "--full", str(project / "repository")],
                                text=True, capture_output=True, check=False)
        require(result.returncode == 0,
                f"full verification failed for {project.name}: {result.stderr.strip()}")
    require(branch_heads(root) == expected, "verification unexpectedly changed published heads")
    return expected


def copy_and_verify(source, destination, verify_binary):
    safe_directory(destination.parent)
    ensure_distinct(source, destination)
    with contextlib.ExitStack() as locks:
        expected = acquire_source_locks(source, locks)
        source_id = repository_identity(source)
        source_tracking = tracking_metadata(source)
        source_drafts = working_copy_records(source)
        reject_links_and_special(source)
        scratch = Path(tempfile.mkdtemp(prefix=".aowis-offline-copy-", dir=destination.parent))
        try:
            # Refuse links/special entries and do not follow symlinks on traversal.
            shutil.copytree(source, scratch, dirs_exist_ok=True, symlinks=True)
            require(repository_identity(scratch) == source_id, "copied repository identity changed")
            require(tracking_metadata(scratch) == source_tracking, "copied remote tracking metadata changed")
            require(working_copy_records(scratch) == source_drafts, "copied working-copy records changed")
            require(branch_heads(scratch) == expected, "copied catalogue or branch heads changed")
            verify(scratch, verify_binary)
            require(tracking_metadata(scratch) == source_tracking,
                    "verified remote tracking metadata changed")
            require(working_copy_records(scratch) == source_drafts,
                    "verified working-copy records changed")
            require(branch_heads(scratch) == expected, "verified copy differs from source heads")
            sync_tree(scratch)
            publish_no_replace(scratch, destination)
        finally:
            if scratch.exists():
                shutil.rmtree(scratch)
    print(f"Verified {len(expected)} projects; published offline snapshot: {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("backup", "restore", "verify"))
    parser.add_argument("source", type=Path, help="offline storage root / snapshot root")
    parser.add_argument("destination", type=Path, nargs="?", help="new, absent destination root")
    parser.add_argument("--verify-binary", type=Path, required=True,
                        help="path to freshly built aowis-dvmdb-verify")
    args = parser.parse_args()
    try:
        source = args.source.absolute()
        verify_binary = args.verify_binary.absolute()
        require(verify_binary.is_file(), f"native verifier does not exist: {verify_binary}")
        if args.command == "verify":
            require(args.destination is None, "verify takes only a source root")
            with contextlib.ExitStack() as locks:
                expected = acquire_source_locks(source, locks)
                reject_links_and_special(source)
                scratch = Path(tempfile.mkdtemp(prefix=".aowis-offline-verify-", dir=source.parent))
                try:
                    shutil.copytree(source, scratch, dirs_exist_ok=True, symlinks=True)
                    require(branch_heads(scratch) == expected, "copied branch heads changed")
                    require(tracking_metadata(scratch) == tracking_metadata(source),
                            "copied remote tracking metadata changed")
                    require(working_copy_records(scratch) == working_copy_records(source),
                            "copied working-copy records changed")
                    require(verify(scratch, verify_binary) == expected,
                            "verification changed published branch heads")
                finally:
                    shutil.rmtree(scratch)
            print(f"Verified {len(expected)} projects in offline root: {source}")
        else:
            require(args.destination is not None, f"{args.command} requires a destination")
            copy_and_verify(source, args.destination.absolute(), verify_binary)
    except (BackupError, OSError, subprocess.SubprocessError) as error:
        print(f"Offline {args.command} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
