#!/usr/bin/env python3
"""Exercise DVMDB patch naming, archive roots and legacy ZIP entrypoint."""

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
    parser.add_argument("--legacy-zip-script", type=Path, required=True)
    arguments = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-naming-") as temporary:
        directory = Path(temporary)
        project = directory / "AOWIS-SERVER-DVMDB"
        scripts = project / "tools"
        scripts.mkdir(parents=True)
        shutil.copy2(arguments.apply_script, scripts / "apply_patch.sh")
        shutil.copy2(arguments.zip_script, scripts / "zip_dvmdb.sh")
        shutil.copy2(arguments.legacy_zip_script, scripts / "zip_db.sh")
        (project / ".gitignore").write_text("/.aowis-patch-delete.txt\n", encoding="utf-8")
        (project / "obsolete.txt").write_text("old\n", encoding="utf-8")
        run("git", "init", "-q", str(project))
        run("git", "-C", str(project), "add", "-A")

        # Both supported CLI entrypoints create the new default archive name.
        run("bash", str(scripts / "zip_db.sh"))
        output = directory / "dvmdb.zip"
        assert output.is_file(), "legacy ZIP entrypoint did not create dvmdb.zip"
        with zipfile.ZipFile(output) as archive:
            assert all(item.startswith("AOWIS-SERVER-DVMDB/")
                       for item in archive.namelist()), "incorrect ZIP root"
        run("bash", str(scripts / "zip_dvmdb.sh"))
        assert output.is_file()

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
            prefix + ".aowis-patch-delete.txt": "obsolete.txt\n",
        })
        run("bash", str(scripts / "apply_patch.sh"), str(valid))
        assert (project / "new_file.txt").read_text() == "new\n"
        assert not (project / "obsolete.txt").exists(), "obsolete renamed file was retained"
        assert not (project / ".aowis-patch-delete.txt").exists(), "deletion manifest was retained"
        with zipfile.ZipFile(output) as archive:
            names = archive.namelist()
            assert prefix + "new_file.txt" in names
            assert prefix + "obsolete.txt" not in names
            assert prefix + ".aowis-patch-delete.txt" not in names

    print("DVMDB naming, patch application and source archive: passed")


if __name__ == "__main__":
    main()
