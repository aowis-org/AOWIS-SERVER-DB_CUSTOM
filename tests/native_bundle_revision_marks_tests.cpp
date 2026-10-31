#include "aowis/dvmdb/native_store.h"
#include "project_repository.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>

namespace {
using aowis::dvmdb::RevisionId;
using aowis::dvmdb::test::ProjectRepository;

using aowis::dvmdb::RevisionBundleImport;
using aowis::dvmdb::BundleImportStatus;
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Change;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::MergeResult;
using aowis::dvmdb::MergeStatus;
using aowis::dvmdb::ModelChange;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;
using aowis::dvmdb::VerificationReport;

void check(bool condition, std::string_view error) {
    if (!condition) throw std::runtime_error(std::string(error));
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const std::filesystem::path base =
            std::filesystem::temp_directory_path() / "aowis-dvmdb-revision-marks-XXXXXX";
        std::string pattern = base.string();
        check(::mkdtemp(pattern.data()) != nullptr, "create unique test directory");
        this->directory_ = pattern;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(this->directory_, ignored);
    }
    const std::filesystem::path& path() const { return this->directory_; }
private:
    std::filesystem::path directory_;
};

Key uuid(unsigned sequence) {
    Key result{};
    result[12] = static_cast<std::uint8_t>(sequence >> 24U);
    result[13] = static_cast<std::uint8_t>(sequence >> 16U);
    result[14] = static_cast<std::uint8_t>(sequence >> 8U);
    result[15] = static_cast<std::uint8_t>(sequence);
    return result;
}

void commit(ProjectRepository& repository, std::string_view branch,
            unsigned sequence, bool modelIndexed) {
    const Bytes payload{static_cast<std::uint8_t>(sequence & 255U),
                        static_cast<std::uint8_t>((sequence >> 8U) & 255U)};
    if (modelIndexed) {
        const ModelRecord record{1, {}, payload};
        (void)repository.commitModel(branch, {ModelChange{uuid(sequence), record}});
    } else {
        (void)repository.commit(branch, {Change{uuid(sequence), payload}});
    }
}

void verifySameHistory(ProjectRepository& source, ProjectRepository& imported) {
    const VerificationReport reference = source.verifyRepository();
    const VerificationReport actual = imported.verifyRepository();
    check(actual.revisions == reference.revisions &&
          actual.reachableObjects == reference.reachableObjects &&
          actual.treeNodes == reference.treeNodes &&
          actual.modelSnapshots == reference.modelSnapshots,
          "imported revision graph differs from verified source");
}

void expectRejected(ProjectRepository& repository, const std::filesystem::path& path,
                    std::string_view branch, std::string_view reason) {
    const RevisionId originalHead = repository.head(branch);
    const std::uint64_t previousObjects = repository.objectCount();
    try {
        (void)repository.importBundle(path, branch);
    } catch (const std::runtime_error& error) {
        check(std::string_view(error.what()).find(reason) != std::string_view::npos,
              "bundle rejected for an unexpected reason");
        check(repository.head(branch) == originalHead &&
              repository.objectCount() == previousObjects,
              "rejected import mutated destination state");
        return;
    }
    throw std::runtime_error("invalid bundle import was accepted");
}

void testRevisionMarks(bool modelIndexed) {
    TemporaryDirectory temporary;
    ProjectRepository source(temporary.path() / "source");
    ProjectRepository destination(temporary.path() / "destination");
    source.createBranch("main");
    const unsigned commonCommits = modelIndexed ? 20U : 190U;
    const unsigned divergentCommits = modelIndexed ? 18U : 70U;
    for (unsigned index = 1; index <= commonCommits; ++index) {
        commit(source, "main", index, modelIndexed);
    }
    const RevisionId ancestor = source.head("main");
    const std::filesystem::path full = temporary.path() / "base.aowbundle";
    source.exportBundle("main", full);
    check(destination.importBundle(full, "main").status ==
          BundleImportStatus::Created, "failed to import base graph");
    verifySameHistory(source, destination);

    source.createBranch("side", "main");
    for (unsigned index = 0; index < divergentCommits; ++index) {
        commit(source, "main", 10000U + index, modelIndexed);
        commit(source, "side", 20000U + index, modelIndexed);
    }
    const RevisionId sideTip = source.head("side");
    const MergeResult merge = source.merge("main", "side");
    check(merge.status == MergeStatus::Merged && merge.conflicts.empty() &&
          source.revision(*merge.resultHead).parents.size() == 2 &&
          source.revision(*merge.resultHead).parents[1] == sideTip,
          "fixture did not create a two-parent merge DAG");
    const RevisionId incoming = source.head("main");
    const std::filesystem::path delta = temporary.path() / "merge.aowbundle";
    source.exportIncrementalBundle("main", ancestor, delta);

    // A sender may name the recipient's real HEAD as base and still send a
    // tip that does not descend from it, moving the branch sideways. Here
    // the recipient's HEAD is a local revision the sender never had; the
    // ordinal-based revision lookup must still prove the base is not an
    // ancestor of the incoming tip.
    {
        ProjectRepository forked(temporary.path() / "forked");
        check(forked.importBundle(full, "main").status == BundleImportStatus::Created,
              "failed to seed forked recipient");
        commit(forked, "main", 999999U, modelIndexed);
        const RevisionId unrelated = forked.head("main");
        const aowis::dvmdb::Hash unrelatedHash = forked.revision(unrelated).recordHash;
        const std::filesystem::path invalid = temporary.path() / "wrong-base.aowbundle";
        std::filesystem::copy_file(delta, invalid);
        {
            std::fstream stream(invalid, std::ios::binary | std::ios::in | std::ios::out);
            check(stream.good(), "open forged incremental bundle");
            stream.seekp(96); // base revision UUID, then its record digest
            stream.write(reinterpret_cast<const char*>(unrelated.uuid().bytes.data()),
                         static_cast<std::streamsize>(unrelated.uuid().bytes.size()));
            stream.write(reinterpret_cast<const char*>(unrelatedHash.data()),
                         static_cast<std::streamsize>(unrelatedHash.size()));
            check(stream.good(), "write forged incremental base");
        }
        check(aowis::dvmdb::test::bundleLayout(invalid).headerBytes == 144,
              "forged fixture is not a provenance-free delta");
        expectRejected(forked, invalid, "main", "not an ancestor");
    }

    const RevisionBundleImport updated = destination.importBundle(delta, "main");
    check(updated.status == BundleImportStatus::FastForward && updated.head == incoming,
          "incremental import with second-parent ancestry did not fast-forward");
    check(destination.importBundle(delta, "main").status ==
          BundleImportStatus::UpToDate, "incremental retry is not idempotent");
    check(destination.revision(sideTip).parents.size() == 1,
          "second-parent history was not imported");
    check(destination.head("main") == incoming &&
          destination.verifyRepository().revisions >=
              source.verifyRepository().revisions,
          "import lost reachable imported merge ancestors");

    const std::filesystem::path complete = temporary.path() / "complete.aowbundle";
    source.exportBundle("main", complete);
    ProjectRepository independent(temporary.path() / "independent");
    check(independent.importBundle(complete, "main").status ==
          BundleImportStatus::Created, "fresh full merge DAG import failed");
    verifySameHistory(source, independent);
}

} // namespace

int main() {
    try {
        testRevisionMarks(false);
        testRevisionMarks(true);
        std::cout << "AOWIS native bundle revision-mark tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native bundle revision-mark tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
