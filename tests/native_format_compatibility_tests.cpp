#include "aowis/native/native_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::Repository;

constexpr std::string_view baseHash = "b8d2bafee89295d4bb183f90b7aad1e2e4a8a76b39229827f79c71405935f468";
constexpr std::string_view featureHash = "4f0f347555c80634f2578dae167d80b841759511a3b891ab0045a479a8f15bb2";
constexpr std::string_view middleHash = "b13743ce2d342a1c4e8c4490ba4ac60cbcbf1e7d20436a561fac67983e217e9a";
constexpr std::string_view tipHash = "7bf2c352bb7308517a3785101b1634b206a954c013549070eb4760ea52f452d1";

void expect(bool success, const std::string& message) {
    if (!success) throw std::runtime_error(message);
}

Key key(std::uint8_t first) {
    Key result{};
    result[0] = first;
    return result;
}

Bytes bytes(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

void checkValue(Repository& repository, const std::string& branch, std::uint8_t number,
                const std::optional<Bytes>& expected) {
    expect(repository.get(branch, key(number)) == expected,
           "frozen model value mismatch on " + branch + " key " + std::to_string(number));
}

void checkMain(Repository& repository) {
    expect(aowis::native::toHex(repository.head("main").id) == tipHash,
           "frozen main revision identity changed");
    checkValue(repository, "main", 0x10, bytes("alpha-v2"));
    checkValue(repository, "main", 0x20, std::nullopt);
    checkValue(repository, "main", 0x30, bytes("gamma-v2"));
    const std::vector<aowis::native::Revision> history = repository.history("main");
    expect(history.size() == 4, "frozen main ancestry length changed");
    expect(aowis::native::toHex(history[1].id) == middleHash, "middle revision changed");
    expect(aowis::native::toHex(history[2].id) == baseHash, "base revision changed");
    expect(repository.getAtRevision(aowis::native::fromHex(baseHash), key(0x10)) == bytes("alpha-v1"),
           "frozen historical read failed");
    expect(repository.getAtRevision(aowis::native::fromHex(baseHash), key(0x20)) == bytes("beta-v1"),
           "frozen historical value changed");
}

void checkFeature(Repository& repository) {
    expect(aowis::native::toHex(repository.head("feature").id) == featureHash,
           "frozen feature revision identity changed");
    checkValue(repository, "feature", 0x10, bytes("alpha-v1"));
    checkValue(repository, "feature", 0x20, bytes("beta-feature"));
    checkValue(repository, "feature", 0x40, bytes("feature-new"));
    const std::vector<aowis::native::Revision> history = repository.history("feature");
    expect(history.size() == 3, "frozen feature ancestry length changed");
    expect(aowis::native::toHex(history[1].id) == baseHash, "feature ancestor changed");
}

struct TemporaryRoot {
    TemporaryRoot() {
        const std::filesystem::path base = std::filesystem::temp_directory_path() / "aowis-format-compat-XXXXXXXX";
        std::string pattern = base.string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const char* created = ::mkdtemp(buffer.data());
        expect(created != nullptr, "could not create disposable compatibility-test directory");
        this->path = created;
    }
    ~TemporaryRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

void checkFrozenFile(const std::filesystem::path& file, std::string_view digest) {
    std::ifstream input(file, std::ios::binary);
    expect(input.good(), "missing compatibility fixture: " + file.string());
    const Bytes contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(!input.bad() && aowis::native::toHex(aowis::native::sha256(contents)) == digest,
           "compatibility fixture changed without an explicit compatibility decision: " + file.string());
}

void verifyFixtureChecksums(const std::filesystem::path& fixture) {
    checkFrozenFile(fixture / "repository/objects.pack", "95e1bd4d189a8065aab9f339c8ca6a6132c58ae487df2b2e8a1af69ed5995e02");
    checkFrozenFile(fixture / "repository/refs/heads/main", "1716252d07ae905394050e7358c09b58d2a62a4bf408534ac69a73349afbb110");
    checkFrozenFile(fixture / "repository/refs/heads/feature", "3d1501338e4e3da3bb475d4bb095f8c8fc01413d0eabe371a7bfb01988c696f0");
    checkFrozenFile(fixture / "base.aowbundle", "40773284242a8dc0a041534ea14d3efcdbada949ab8e73dd27821fb96594925d");
    checkFrozenFile(fixture / "delta.aowbundle", "662a1df1dc373999df5062bd2c675684b4a5b907429941ca049e1eddf4d2ddac");
    checkFrozenFile(fixture / "feature.aowbundle", "f00c5596c81d72672b18e6030d44a7dedfb87c1c9d15b5805f4e9667a44655b8");
    checkFrozenFile(fixture / "tip.aowbundle", "f1dfc5126a6f9c75ae97cc5495adc97efe2d72a901bd9a8dad67442a3b61ee35");
}

void exercise(const std::filesystem::path& fixture) {
    verifyFixtureChecksums(fixture);
    TemporaryRoot temporary;
    const std::filesystem::path oldRepository = temporary.path / "old-pack";
    std::filesystem::copy(fixture / "repository", oldRepository,
                          std::filesystem::copy_options::recursive);
    expect(!std::filesystem::exists(oldRepository / "objects.idx"),
           "frozen fixture must not depend on a saved derived index");
    {
        Repository repository(oldRepository);
        repository.verifyRepository();
        checkMain(repository);
        checkFeature(repository);
        const std::vector<std::string> branches = repository.listBranches();
        expect(branches == std::vector<std::string>({"feature", "main"}),
               "frozen branch set changed");
    }
    expect(std::filesystem::exists(oldRepository / "objects.idx"),
           "older pack index was not rebuilt");
    {
        Repository repository(oldRepository);
        repository.verifyRepository();
        checkMain(repository);
        checkFeature(repository);
        repository.commit("main", {{key(0x50), bytes("new-reader-wrote-this")}});
        repository.verifyRepository();
    }
    {
        Repository imported(temporary.path / "bundles");
        imported.importBranchBundle(fixture / "base.aowbundle", "main");
        expect(aowis::native::toHex(imported.head("main").id) == baseHash,
               "historical full bundle imported at wrong revision");
        checkValue(imported, "main", 0x10, bytes("alpha-v1"));
        imported.importBranchBundle(fixture / "delta.aowbundle", "main");
        imported.importBranchBundle(fixture / "delta.aowbundle", "main");
        checkMain(imported);
        imported.importBranchBundle(fixture / "feature.aowbundle", "feature");
        checkFeature(imported);
        imported.verifyRepository();
        imported.exportBranchBundle("main", temporary.path / "current-export.aowbundle");
    }
    {
        Repository roundtrip(temporary.path / "roundtrip");
        roundtrip.importBranchBundle(temporary.path / "current-export.aowbundle", "main");
        checkMain(roundtrip);
        roundtrip.importBranchBundle(fixture / "tip.aowbundle", "alternate");
        expect(roundtrip.head("alternate").id == roundtrip.head("main").id,
               "old full bundle differs from current export");
        roundtrip.verifyRepository();
    }
    // A damaged frozen transfer must fail without publishing a branch.
    const std::filesystem::path damaged = temporary.path / "damaged.aowbundle";
    std::filesystem::copy_file(fixture / "base.aowbundle", damaged);
    {
        std::fstream stream(damaged, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(60);
        stream.put('\xff');
        expect(stream.good(), "could not damage private bundle copy");
    }
    {
        Repository destination(temporary.path / "rejected");
        bool rejected = false;
        try {
            destination.importBranchBundle(damaged, "main");
        } catch (const std::exception&) {
            rejected = true;
        }
        expect(rejected, "damaged frozen bundle was accepted");
        expect(destination.listBranches().empty(), "corrupt bundle published a branch");
        destination.verifyRepository();
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "usage: aowis-native-format-compatibility-tests FIXTURE_ROOT");
        exercise(std::filesystem::absolute(argv[1]));
        std::cout << "Frozen pack and portable bundle compatibility passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Frozen format compatibility FAILED: " << error.what() << '\n';
        return 1;
    }
}
