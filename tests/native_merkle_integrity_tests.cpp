#include "aowis/dvmdb/native_store.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using aowis::dvmdb::Bytes;
using aowis::dvmdb::Hash;
using aowis::dvmdb::Key;
using aowis::dvmdb::ModelRecord;
using aowis::dvmdb::Repository;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Key key(std::uint8_t lastByte) {
    Key result{};
    result.back() = lastByte;
    return result;
}

void appendHash(Bytes& destination, const Hash& value) {
    destination.insert(destination.end(), value.begin(), value.end());
}

void appendU64(Bytes& destination, std::uint64_t value) {
    for (unsigned position = 0; position < 8; ++position) {
        destination.push_back(static_cast<std::uint8_t>(value >> (56U - 8U * position)));
    }
}

struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view label) {
        static std::uint64_t serial = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-merkle-" + std::string(label) + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++serial));
        std::filesystem::create_directories(this->path);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(this->path, ignored);
    }
    std::filesystem::path path;
};

struct BundleBuilder {
    std::map<Hash, Bytes> objects;

    Hash add(const Bytes& payload) {
        const Hash hash = aowis::dvmdb::sha256(payload);
        this->objects.emplace(hash, payload);
        return hash;
    }

    Hash leaf(const Key& uuid, const Hash& value) {
        Bytes content{'L'};
        content.insert(content.end(), uuid.begin(), uuid.end());
        appendHash(content, value);
        return add(content);
    }

    Hash branch(std::uint8_t depth, const Key& prefix,
                const std::vector<std::pair<unsigned, Hash>>& children) {
        Bytes content{'B', depth};
        content.insert(content.end(), prefix.begin(), prefix.end());
        std::array<Hash, 16> slots{};
        for (const std::pair<unsigned, Hash>& entry : children) {
            expect(entry.first < slots.size() && slots[entry.first] == Hash{},
                   "invalid branch fixture slot");
            slots[entry.first] = entry.second;
        }
        for (const Hash& slot : slots) appendHash(content, slot);
        expect(content.size() == 530, "branch fixture length mismatch");
        return add(content);
    }

    Hash revision(const Hash& root, const Hash& references = Hash{},
                  bool model = false, const std::optional<Hash>& parent = std::nullopt) {
        Bytes content{'S'};
        appendHash(content, root);
        appendHash(content, references);
        content.push_back(model ? 1U : 0U);
        content.push_back(parent ? 1U : 0U);
        if (parent) appendHash(content, *parent);
        return add(content);
    }

    void write(const std::filesystem::path& path, const Hash& tip,
               const std::optional<Hash>& base = std::nullopt,
               const std::optional<aowis::dvmdb::BranchId>& branchId = std::nullopt) const {
        Bytes bundle{'A', 'O', 'W', 'S', 'Y', 'N', 'C',
                     static_cast<std::uint8_t>(base ? '4' : '3')};
        appendHash(bundle, tip);
        appendU64(bundle, static_cast<std::uint64_t>(this->objects.size()));
        if (base) appendHash(bundle, *base);
        const aowis::dvmdb::BranchId id = branchId.value_or(aowis::dvmdb::BranchId::generate());
        const std::array<std::uint8_t, 16>& uuid = id.uuid().bytes;
        bundle.insert(bundle.end(), uuid.begin(), uuid.end());
        for (const std::pair<const Hash, Bytes>& entry : this->objects) {
            appendHash(bundle, entry.first);
            appendU64(bundle, static_cast<std::uint64_t>(entry.second.size()));
            bundle.insert(bundle.end(), entry.second.begin(), entry.second.end());
        }
        std::ofstream output(path, std::ios::binary);
        expect(output.good(), "cannot create test bundle");
        output.write(reinterpret_cast<const char*>(bundle.data()),
                     static_cast<std::streamsize>(bundle.size()));
        expect(output.good(), "cannot write test bundle");
    }
};

enum class TreeKind { Raw, Model, Reference };
enum class Defect {
    DuplicateUuid, WrongSlot, WrongCompressedPrefix, NoncanonicalPadding,
    ReversedDepth, NestedWrongSlot, SingleChild, ZeroValue,
    InvalidBranchDepth, TruncatedBranch, InvalidNodeTag
};

Hash addValue(BundleBuilder& builder, TreeKind kind) {
    if (kind == TreeKind::Model) {
        const ModelRecord record{1, {}, Bytes{'m', 'o', 'd', 'e', 'l'}};
        const Bytes encoded = aowis::dvmdb::encodeModelRecord(record);
        Bytes object{'V'};
        object.insert(object.end(), encoded.begin(), encoded.end());
        return builder.add(object);
    }
    if (kind == TreeKind::Reference) {
        Bytes object{'I', 'N', 'R', '1', 0, 0, 0, 1};
        const Key referrer = key(5);
        object.insert(object.end(), referrer.begin(), referrer.end());
        return builder.add(object);
    }
    return builder.add(Bytes{'V', 'd', 'a', 't', 'a'});
}

// Every fixture is a complete cryptographically consistent object graph.
// Only the logical trie invariant named by the case is violated.
Hash makeTree(BundleBuilder& builder, TreeKind kind, Defect defect) {
    if (defect == Defect::ZeroValue) return builder.leaf(key(1), Hash{});
    if (defect == Defect::TruncatedBranch) return builder.add(Bytes{'B', 31});
    if (defect == Defect::InvalidNodeTag) return builder.add(Bytes{'X'});
    const Hash content = addValue(builder, kind);
    const Hash first = builder.leaf(key(1), content);
    const Key blank{};
    if (defect == Defect::SingleChild) {
        return builder.branch(31, blank, {{1, first}});
    }
    const Hash second = builder.leaf(key(2), content);
    switch (defect) {
    case Defect::DuplicateUuid:
        return builder.branch(31, blank, {{1, first}, {2, first}});
    case Defect::WrongSlot:
        return builder.branch(31, blank, {{3, first}, {2, second}});
    case Defect::WrongCompressedPrefix:
        return builder.branch(31, key(0x10), {{1, first}, {2, second}});
    case Defect::NoncanonicalPadding:
        return builder.branch(31, key(1), {{1, first}, {2, second}});
    case Defect::ReversedDepth: {
        const Hash third = builder.leaf(key(0x11), content);
        const Hash inner = builder.branch(30, blank, {{0, first}, {1, third}});
        return builder.branch(31, blank, {{1, inner}, {2, second}});
    }
    case Defect::NestedWrongSlot: {
        const Hash third = builder.leaf(key(0x11), content);
        const Hash inner = builder.branch(31, blank, {{1, first}, {2, second}});
        return builder.branch(30, blank, {{2, inner}, {1, third}});
    }
    case Defect::InvalidBranchDepth:
        return builder.branch(32, blank, {{1, first}, {2, second}});
    case Defect::SingleChild:
    case Defect::ZeroValue:
    case Defect::TruncatedBranch:
    case Defect::InvalidNodeTag:
        break;
    }
    throw std::runtime_error("unhandled malformed tree fixture");
}

void expectRejected(Repository& target, const std::filesystem::path& bundle,
                    const std::string& label, const std::string& expectedError) {
    const std::uint64_t objects = target.objectCount();
    const std::uint64_t pack = target.packSize();
    const Hash head = target.head("stable").id;
    bool failed = false;
    try {
        (void)target.importBranchBundle(bundle, "untrusted");
    } catch (const std::runtime_error& error) {
        failed = true;
        expect(std::string(error.what()).find(expectedError) != std::string::npos,
               "unexpected import rejection for " + label + ": " + error.what());
    }
    expect(failed, "malformed trie imported: " + label);
    expect(target.listBranches() == std::vector<std::string>{"stable"},
           "rejected bundle published branch: " + label);
    expect(target.head("stable").id == head &&
               target.objectCount() == objects && target.packSize() == pack,
           "rejected bundle modified repository: " + label);
}

void testMalformedTrees() {
    TemporaryDirectory temporary("malformed");
    Repository target(temporary.path / "target");
    target.createBranch("stable");
    target.commit("stable", {{key(42), Bytes{'s', 'a', 'f', 'e'}}});

    struct DefectCase {
        Defect defect;
        std::string label;
        std::string error;
    };
    const std::vector<DefectCase> defects{
        {Defect::DuplicateUuid, "duplicate UUID", "wrong slot"},
        {Defect::WrongSlot, "wrong child slot", "wrong slot"},
        {Defect::WrongCompressedPrefix, "wrong compressed prefix", "compressed prefix"},
        {Defect::NoncanonicalPadding, "noncanonical prefix padding", "noncanonical"},
        {Defect::ReversedDepth, "reversed branch depth", "depth ordering"},
        {Defect::NestedWrongSlot, "wrong nested child slot", "wrong slot"},
        {Defect::SingleChild, "one-child branch", "child count"},
        {Defect::ZeroValue, "zero leaf value hash", "zero value hash"},
        {Defect::InvalidBranchDepth, "invalid branch depth", "invalid bundled tree branch"},
        {Defect::TruncatedBranch, "truncated branch", "invalid bundled tree branch"},
        {Defect::InvalidNodeTag, "invalid tree node tag", "invalid bundled tree tag"},
    };
    for (const DefectCase& fixture : defects) {
        for (TreeKind kind : {TreeKind::Raw, TreeKind::Model, TreeKind::Reference}) {
            // Invalid tree in either model index must be caught as well as in
            // the raw data tree. A valid model/ref tree in the other position
            // ensures that validation actually reaches the malformed one.
            BundleBuilder builder;
            const Hash badRoot = makeTree(builder, kind, fixture.defect);
            Hash tip{};
            if (kind == TreeKind::Raw) {
                tip = builder.revision(badRoot);
            } else if (kind == TreeKind::Model) {
                tip = builder.revision(badRoot, Hash{}, true);
            } else {
                const Hash value = addValue(builder, TreeKind::Model);
                const Hash normalRoot = builder.leaf(key(7), value);
                tip = builder.revision(normalRoot, badRoot, true);
            }
            const std::string label = fixture.label +
                (kind == TreeKind::Raw ? " / raw" : kind == TreeKind::Model ?
                 " / model" : " / reference");
            const std::filesystem::path file = temporary.path / "malformed.aowbundle";
            builder.write(file, tip);
            expectRejected(target, file, label, fixture.error);
            std::filesystem::remove(file);
        }
    }
    target.verifyPack();
}

void testValidCompressedAndSharedTrees() {
    TemporaryDirectory temporary("valid");
    Repository target(temporary.path / "target");
    BundleBuilder builder;
    const Hash value = addValue(builder, TreeKind::Raw);
    const Hash one = builder.leaf(key(1), value);
    const Hash two = builder.leaf(key(2), value);
    const Hash three = builder.leaf(key(0x11), value);
    const Hash inner = builder.branch(31, Key{}, {{1, one}, {2, two}});
    const Hash root = builder.branch(30, Key{}, {{0, inner}, {1, three}});
    const Hash first = builder.revision(root);
    const Hash tip = builder.revision(root, Hash{}, false, first);
    const std::filesystem::path bundle = temporary.path / "valid.aowbundle";
    builder.write(bundle, tip);
    target.importBranchBundle(bundle, "valid");
    expect(target.head("valid").id == tip &&
               target.scan("valid").size() == 3 &&
               target.get("valid", key(1)) == Bytes{'d', 'a', 't', 'a'} &&
               target.get("valid", key(2)) == Bytes{'d', 'a', 't', 'a'} &&
               target.get("valid", key(0x11)) == Bytes{'d', 'a', 't', 'a'},
           "valid compressed/shared Merkle history failed verification");
    target.verifyPack();
}

void testValidNonzeroCompressedPrefix() {
    TemporaryDirectory temporary("nonzero-prefix");
    Repository target(temporary.path / "target");
    BundleBuilder builder;
    const Hash value = addValue(builder, TreeKind::Raw);
    Key first{};
    first[0] = 0xab;
    first[1] = 0xcd;
    first[5] = 0xef;
    first[6] = 0x21;
    Key second = first;
    second[6] = 0x31;
    Key prefix = first;
    for (unsigned index = 6; index < prefix.size(); ++index) prefix[index] = 0;
    const Hash left = builder.leaf(first, value);
    const Hash right = builder.leaf(second, value);
    const Hash root = builder.branch(12, prefix, {{2, left}, {3, right}});
    const Hash tip = builder.revision(root);
    const std::filesystem::path bundle = temporary.path / "nonzero.aowbundle";
    builder.write(bundle, tip);
    target.importBranchBundle(bundle, "valid");
    expect(target.head("valid").id == tip && target.scan("valid").size() == 2 &&
               target.get("valid", first) == Bytes{'d', 'a', 't', 'a'} &&
               target.get("valid", second) == Bytes{'d', 'a', 't', 'a'},
           "valid nonzero compressed prefix was not accepted");
}

void testValidMaximumTrieDepth() {
    TemporaryDirectory temporary("maximum-depth");
    Repository target(temporary.path / "target");
    BundleBuilder builder;
    const Hash value = addValue(builder, TreeKind::Raw);
    const Key zero{};
    Hash root = builder.leaf(zero, value);
    for (int depth = 31; depth >= 0; --depth) {
        Key sibling{};
        const unsigned position = static_cast<unsigned>(depth);
        sibling[position / 2U] = static_cast<std::uint8_t>(
            (position % 2U == 0U) ? 0x10 : 0x01);
        const Hash leaf = builder.leaf(sibling, value);
        root = builder.branch(static_cast<std::uint8_t>(depth), zero,
                              {{0, root}, {1, leaf}});
    }
    const Hash tip = builder.revision(root);
    const std::filesystem::path bundle = temporary.path / "deep.aowbundle";
    builder.write(bundle, tip);
    target.importBranchBundle(bundle, "deep");
    expect(target.head("deep").id == tip &&
               target.scan("deep").size() == 33 &&
               target.get("deep", zero) == Bytes{'d', 'a', 't', 'a'},
           "valid maximum-depth compressed Merkle tree was rejected");
}

void testMalformedIncremental() {
    TemporaryDirectory temporary("incremental");
    Repository source(temporary.path / "source");
    Repository target(temporary.path / "target");
    source.createBranch("stable");
    const Hash base = source.commit("stable", {{key(33), Bytes{'b', 'a', 's', 'e'}}}).id;
    const std::filesystem::path baseline = temporary.path / "baseline.aowbundle";
    source.exportBranchBundle("stable", baseline);
    target.importBranchBundle(baseline, "stable");

    BundleBuilder builder;
    const Hash root = makeTree(builder, TreeKind::Raw, Defect::DuplicateUuid);
    const Hash incoming = builder.revision(root, Hash{}, false, base);
    const std::filesystem::path delta = temporary.path / "bad-delta.aowbundle";
    builder.write(delta, incoming, base, source.branchId("stable"));
    const std::uint64_t objects = target.objectCount();
    const std::uint64_t pack = target.packSize();
    bool failed = false;
    try {
        (void)target.importBranchBundle(delta, "stable");
    } catch (const std::runtime_error& error) {
        failed = true;
        expect(std::string(error.what()).find("wrong slot") != std::string::npos,
               "incremental rejected for reason other than invalid trie path");
    }
    expect(failed && target.head("stable").id == base &&
               target.objectCount() == objects && target.packSize() == pack,
           "malformed incremental Merkle history changed existing branch");
    target.verifyPack();
}
} // namespace

int main() {
    try {
        testMalformedTrees();
        testValidCompressedAndSharedTrees();
        testValidNonzeroCompressedPrefix();
        testValidMaximumTrieDepth();
        testMalformedIncremental();
        std::cout << "AOWIS native Merkle integrity tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "AOWIS native Merkle integrity tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
