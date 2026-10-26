#!/usr/bin/env python3
"""Exercise canonical AOWIS DVMDB patch names, archive roots, and no legacy entrypoints."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def run(*arguments: str, success: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(arguments, text=True, capture_output=True, check=False)
    if success and result.returncode != 0:
        raise AssertionError(f"failed command {arguments}: {result.stdout}\n{result.stderr}")
    return result


def patch_archive(path: Path, files: dict[str, str]) -> None:
    with zipfile.ZipFile(path, "w") as archive:
        for name, content in files.items():
            archive.writestr(name, content)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply-script", type=Path, required=True)
    parser.add_argument("--zip-script", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-naming-") as temporary:
        directory = Path(temporary)
        project = directory / "AOWIS-SERVER-DVMDB"
        scripts = project / "tools"
        scripts.mkdir(parents=True)
        shutil.copy2(arguments.apply_script, scripts / "apply_patch.sh")
        shutil.copy2(arguments.zip_script, scripts / "zip_dvmdb.sh")
        (project / ".gitignore").write_text("/.aowis-patch-delete.txt\n", encoding="utf-8")
        (project / "obsolete.txt").write_text("old\n", encoding="utf-8")
        (project / "legacy_tree").mkdir()
        (project / "legacy_tree" / "obsolete.txt").write_text("old\n", encoding="utf-8")
        # Even mistakenly tracked bytecode must not leak into source snapshots.
        python_cache = project / "tools" / "__pycache__"
        python_cache.mkdir()
        (python_cache / "worker.cpython-314.pyc").write_bytes(b"\0\x01test-bytecode")
        run("git", "init", "-q", str(project))
        run("git", "-C", str(project), "add", "-A")

        # One entrypoint; no compatibility wrapper or old public API paths.
        assert not (scripts / "zip_db.sh").exists()
        assert not (arguments.source_root / "tools" / "zip_db.sh").exists()
        assert not (arguments.source_root / "include" / "aowis" / "native").exists()
        assert not (arguments.source_root / "include" / "aowis" / "dvmdb" / "db_http_routes.h").exists()
        assert not (arguments.source_root / "src" / "db_http_routes.cpp").exists()
        assert (arguments.source_root / "include" / "aowis" / "dvmdb" / "dvmdb_http_routes.h").is_file()
        assert (arguments.source_root / "src" / "dvmdb_http_routes.cpp").is_file()
        run("bash", str(scripts / "zip_dvmdb.sh"))
        output = directory / "dvmdb.zip"
        assert output.is_file()
        with zipfile.ZipFile(output) as archive:
            assert all(item.startswith("AOWIS-SERVER-DVMDB/")
                       for item in archive.namelist()), "incorrect ZIP root"
        assert output.is_file()
        with zipfile.ZipFile(output) as archive:
            assert not any("__pycache__" in name or name.endswith(".pyc")
                           for name in archive.namelist()), "source archive contains Python bytecode"

        prefix = "AOWIS-SERVER-DVMDB/"
        old_name = directory / ("aowis-" + "db-custom-rejected.zip")
        patch_archive(old_name, {prefix + "wrong.txt": "should not be written"})
        rejected = run("bash", str(scripts / "apply_patch.sh"), str(old_name), success=False)
        assert rejected.returncode != 0 and "expected aowis-dvmdb" in rejected.stderr
        assert not (project / "wrong.txt").exists(), "old patch naming bypassed validation"

        wrong_root = directory / "aowis-dvmdb-wrong-root.zip"
        patch_archive(wrong_root, {"WRONG-PROJECT/wrong.txt": "incorrect root"})
        rejected = run("bash", str(scripts / "apply_patch.sh"), str(wrong_root), success=False)
        assert rejected.returncode != 0 and "Invalid patch path" in rejected.stderr
        assert not (project / "wrong.txt").exists()

        valid = directory / "aowis-dvmdb-valid.zip"
        patch_archive(valid, {
            prefix + "new_file.txt": "new\n",
            prefix + ".aowis-patch-delete.txt": "obsolete.txt\nlegacy_tree/obsolete.txt\n",
        })
        run("bash", str(scripts / "apply_patch.sh"), str(valid))
        assert (project / "new_file.txt").read_text() == "new\n"
        assert not (project / "obsolete.txt").exists(), "obsolete renamed file was retained"
        assert not (project / "legacy_tree").exists(), "obsolete directory was retained"
        assert not (project / ".aowis-patch-delete.txt").exists(), "deletion manifest was retained"
        with zipfile.ZipFile(output) as archive:
            names = archive.namelist()
            assert prefix + "new_file.txt" in names
            assert prefix + "obsolete.txt" not in names
            assert prefix + ".aowis-patch-delete.txt" not in names

    print("DVMDB-only naming, patch application and source archive: passed")


if __name__ == "__main__":
    main()
