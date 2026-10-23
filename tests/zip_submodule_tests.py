#!/usr/bin/env python3
"""End-to-end regression test for recursive, self-contained zip_db.sh exports."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)


def write(path: Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(contents, encoding="utf-8")


def init_repo(path: Path) -> None:
    path.mkdir(parents=True)
    run("git", "init", "-q", str(path))
    run("git", "-C", str(path), "config", "user.name", "AOWIS ZIP test")
    run("git", "-C", str(path), "config", "user.email", "zip-tests@example.invalid")


def commit(repo: Path) -> None:
    run("git", "-C", str(repo), "add", "-A")
    run("git", "-C", str(repo), "commit", "-qm", "test fixture")


def add_submodule(repo: Path, source: Path, destination: str) -> None:
    run("git", "-C", str(repo), "-c", "protocol.file.allow=always",
        "submodule", "add", "-q", "--", str(source), destination)


def assert_members(archive: Path, project_name: str) -> None:
    prefix = project_name + "/"
    expected = {
        "src/root.cpp": "root\n",
        "docs/untracked.txt": "new project file\n",
        "external/AOWIS-SERVER-MODEL/src/model.cpp": "modified model\n",
        "external/AOWIS-SERVER-MODEL/src/new.cpp": "new model file\n",
        "external/AOWIS-SERVER-MODEL/.hidden": "hidden file\n",
        "external/AOWIS-SERVER-MODEL/deps/nested/include/nested.h": "nested source\n",
        "external/AOWIS-SERVER-MODEL/deps/nested/fresh.txt": "nested untracked\n",
        "external/EXTRA/extra.txt": "second submodule\n",
    }
    excluded = {
        ".env",
        "external/AOWIS-SERVER-MODEL/.env",
        "external/AOWIS-SERVER-MODEL/secret.key",
        "external/AOWIS-SERVER-MODEL/ignored.o",
        "external/AOWIS-SERVER-MODEL/deps/nested/cert.pem",
    }
    with zipfile.ZipFile(archive) as package:
        names = set(package.namelist())
        for path, contents in expected.items():
            entry = prefix + path
            assert entry in names, f"missing archive entry: {entry}"
            assert package.read(entry).decode("utf-8") == contents, entry
        for path in excluded:
            assert prefix + path not in names, f"sensitive/ignored file included: {path}"
        assert not any(".git" in Path(entry).parts for entry in names), (
            "ZIP must not contain Git internal metadata")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="aowis-zip-test-") as directory:
        root = Path(directory)
        nested = root / "nested-source"
        model = root / "model-source"
        extra = root / "extra-source"
        project = root / "AOWIS-SERVER-DB_CUSTOM"

        init_repo(nested)
        write(nested / "include/nested.h", "nested source\n")
        write(nested / "cert.pem", "do not package\n")
        commit(nested)

        init_repo(model)
        write(model / "src/model.cpp", "original model\n")
        write(model / ".hidden", "hidden file\n")
        write(model / ".gitignore", "*.o\n")
        write(model / ".env", "do not package\n")
        write(model / "secret.key", "do not package\n")
        add_submodule(model, nested, "deps/nested")
        commit(model)

        init_repo(extra)
        write(extra / "extra.txt", "second submodule\n")
        commit(extra)

        init_repo(project)
        write(project / "src/root.cpp", "root\n")
        write(project / ".env", "do not package\n")
        write(project / ".gitignore", "ignored-build/\n")
        (project / "tools").mkdir()
        shutil.copy2(args.script, project / "tools/zip_db.sh")
        add_submodule(project, model, "external/AOWIS-SERVER-MODEL")
        add_submodule(project, extra, "external/EXTRA")
        commit(project)
        run("git", "-C", str(project), "-c", "protocol.file.allow=always",
            "submodule", "update", "--init", "--recursive")

        checkout = project / "external/AOWIS-SERVER-MODEL"
        write(checkout / "src/model.cpp", "modified model\n")
        write(checkout / "src/new.cpp", "new model file\n")
        write(checkout / "ignored.o", "ignored\n")
        write(checkout / "deps/nested/fresh.txt", "nested untracked\n")
        write(project / "docs/untracked.txt", "new project file\n")

        archive = root / "db_custom.zip"
        run("bash", str(project / "tools/zip_db.sh"))
        assert_members(archive, project.name)
        valid_archive = archive.read_bytes()

        # Removing a nested checkout must make the script fail *without*
        # overwriting the previous successfully generated archive.
        shutil.rmtree(checkout / "deps/nested")
        failure = subprocess.run(["bash", str(project / "tools/zip_db.sh")],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert failure.returncode != 0, "uninitialized nested submodule was silently skipped"
        assert "not initialized" in failure.stderr, failure.stderr
        assert archive.read_bytes() == valid_archive, "failed packaging replaced the ZIP"

    print("Recursive submodule ZIP packaging: passed")


if __name__ == "__main__":
    main()
