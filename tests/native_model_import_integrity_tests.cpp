#include "aowis/native/native_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using aowis::native::BundleImportStatus;
using aowis::native::Bytes;
using aowis::native::Hash;
using aowis::native::Key;
using aowis::native::ModelRecord;
using aowis::native::Repository;

void expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Key key(std::uint8_t value) {
    Key result{};
    result.back() = value;
    return result;
}

void appendHash(Bytes& destination, const Hash& value) {
    destination.insert(destination.end(), value.begin(), value.end());
}

void appendU64(Bytes& destination, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        destination.push_back(static_cast<std::uint8_t>(value >> (56U - 8U * index)));
    }
}

struct TemporaryDirectory {
    explicit TemporaryDirectory(std::string_view label) {
        static std::uint64_t sequence = 0;
        this->path = std::filesystem::temp_directory_path() /
            ("aowis-model-import-" + std::string(label) + "-" +
             std::to_string(::getpid()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++sequence));
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

    Hash add(const Bytes& bytes) {
        const Hash hash = aowis::native::sha256(bytes);
        this->objects.emplace(hash, bytes);
        return hash;
    }

    Hash modelValue(const std::vector<Key>& references, bool noncanonical = false) {
        Bytes envelope = aowis::native::encodeModelRecord(
            ModelRecord{1, references, Bytes{'m', 'o', 'd', 'e', 'l'}});
        if (noncanonical) {
            expect(references.size() == 2, "noncanonical fixture requires two references");
            // Deliberately reverse the already canonical reference sequence.
            std::swap_ranges(envelope.begin() + 11, envelope.begin() + 27,
                             envelope.begin() + 27);
        }
        Bytes bytes{'V'};
        bytes.insert(bytes.end(), envelope.begin(), envelope.end());
        return add(bytes);
    }

    Hash duplicateReferenceValue(const Key& reference) {
        Bytes envelope = aowis::native::encodeModelRecord(
            ModelRecord{1, {reference, key(3)}, Bytes{'m'}});
        // Maintain a syntactically valid canonical envelope length, but make
        // its two incoming dependency UUIDs identical.
        std::copy_n(envelope.begin() + 11, 16, envelope.begin() + 27);
        Bytes bytes{'V'};
        bytes.insert(bytes.end(), envelope.begin(), envelope.end());
        return add(bytes);
    }

    Hash incomingValue(const std::vector<Key>& sources) {
        Bytes bytes{'I', 'N', 'R', '1', 0, 0, 0,
                    static_cast<std::uint8_t>(sources.size())};
        for (const Key& source : sources) {
            bytes.insert(bytes.end(), source.begin(), source.end());
        }
        return add(bytes);
    }

    Hash leaf(const Key& uuid, const Hash& content) {
        Bytes encoded{'L'};
        encoded.insert(encoded.end(), uuid.begin(), uuid.end());
        appendHash(encoded, content);
        return add(encoded);
    }

    static unsigned nibble(const Key& uuid, unsigned depth) {
        return depth % 2U == 0U ? static_cast<unsigned>(uuid[depth / 2U] >> 4U) :
                                   static_cast<unsigned>(uuid[depth / 2U] & 15U);
    }

    Hash tree(const std::map<Key, Hash>& values) {
        if (values.empty()) return Hash{};
        std::vector<std::pair<Key, Hash>> ordered(values.begin(), values.end());
        return treePart(ordered, 0, ordered.size());
    }

    Hash treePart(const std::vector<std::pair<Key, Hash>>& entries,
                  std::size_t first, std::size_t end) {
        if (end == first + 1) return leaf(entries[first].first, entries[first].second);
        unsigned depth = 0;
        while (depth < 32 && nibble(entries[first].first, depth) ==
                                     nibble(entries[end - 1].first, depth)) {
            ++depth;
        }
        expect(depth < 32, "duplicate test UUID");
        Key prefix = entries[first].first;
        for (unsigned position = depth; position < 32; ++position) {
            const unsigned byte = position / 2U;
            if (position % 2U == 0U) prefix[byte] &= 0x0fU;
            else prefix[byte] &= 0xf0U;
        }
        Bytes branch{'B', static_cast<std::uint8_t>(depth)};
        branch.insert(branch.end(), prefix.begin(), prefix.end());
        std::array<Hash, 16> children{};
        std::size_t cursor = first;
        while (cursor < end) {
            const unsigned slot = nibble(entries[cursor].first, depth);
            std::size_t next = cursor + 1;
            while (next < end && nibble(entries[next].first, depth) == slot) ++next;
            children[slot] = treePart(entries, cursor, next);
            cursor = next;
        }
        for (const Hash& child : children) appendHash(branch, child);
        return add(branch);
    }

    Hash revision(const Hash& modelRoot, const Hash& incomingRoot,
                  const std::optional<Hash>& parent = std::nullopt,
                  const std::optional<Hash>& mergeParent = std::nullopt) {
        Bytes encoded{'S'};
        appendHash(encoded, modelRoot);
        appendHash(encoded, incomingRoot);
        encoded.push_back(1);
        expect(!mergeParent || parent.has_value(), "merge parent requires first parent");
        encoded.push_back(mergeParent ? 2U : parent ? 1U : 0U);
        if (parent) appendHash(encoded, *parent);
        if (mergeParent) appendHash(encoded, *mergeParent);
        return add(encoded);
    }

    void write(const std::filesystem::path& path, const Hash& tip,
               const std::optional<Hash>& base = std::nullopt) const {
        Bytes bundle{'A', 'O', 'W', 'S', 'Y', 'N', 'C',
                     static_cast<std::uint8_t>(base ? '2' : '1')};
        appendHash(bundle, tip);
        appendU64(bundle, this->objects.size());
        if (base) appendHash(bundle, *base);
        for (const std::pair<const Hash, Bytes>& object : this->objects) {
            appendHash(bundle, object.first);
            appendU64(bundle, object.second.size());
            bundle.insert(bundle.end(), object.second.begin(), object.second.end());
        }
        std::ofstream output(path, std::ios::binary);
        expect(output.good(), "cannot create model import test bundle");
        output.write(reinterpret_cast<const char*>(bundle.data()),
                     static_cast<std::streamsize>(bundle.size()));
        expect(output.good(), "cannot write model import test bundle");
    }
};

// Every adversarial input below is SHA-256-consistent, reachable, and has
// canonical Merkle paths. Only the relationship between the primary models,
// their records and their reverse-reference index is malicious.
enum class Fault {
    MissingIncoming, ExtraIncoming, WrongIncomingSource,
    MissingTarget, ZeroTarget, SelfReference, UnsortedReferences,
    DuplicateReferences, ZeroModelUuid, ZeroIncomingTarget,
    InvalidAncestor, InvalidSecondParent, SharedRootDifferentReverseIndex
};

Hash makeFixture(BundleBuilder& builder, Fault fault) {
    std::map<Key, std::vector<Key>> records{{key(1), {}}, {key(2), {key(1)}}};
    std::map<Key, std::vector<Key>> incoming{{key(1), {key(2)}}};
    switch (fault) {
    case Fault::MissingIncoming:
        incoming.clear();
        break;
    case Fault::ExtraIncoming:
        incoming[key(3)] = {key(2)};
        break;
    case Fault::WrongIncomingSource:
        incoming[key(1)] = {key(3)};
        break;
    case Fault::MissingTarget:
        records[key(2)] = {key(9)};
        incoming.clear();
        incoming[key(9)] = {key(2)};
        break;
    case Fault::ZeroTarget:
        records[key(2)] = {Key{}};
        incoming.clear();
        break;
    case Fault::SelfReference:
        records[key(2)] = {key(2)};
        incoming.clear();
        incoming[key(2)] = {key(2)};
        break;
    case Fault::UnsortedReferences:
        records[key(3)] = {};
        records[key(2)] = {key(1), key(3)};
        incoming[key(3)] = {key(2)};
        break;
    case Fault::DuplicateReferences:
        records[key(3)] = {};
        records[key(2)] = {key(1), key(1)};
        break;
    case Fault::ZeroModelUuid:
        records[Key{}] = {};
        break;
    case Fault::ZeroIncomingTarget:
        incoming[Key{}] = {key(2)};
        break;
    case Fault::InvalidAncestor:
    case Fault::InvalidSecondParent:
    case Fault::SharedRootDifferentReverseIndex:
        break;
    }
    std::map<Key, Hash> models;
    for (const std::pair<const Key, std::vector<Key>>& record : records) {
        if (record.first == key(2) && fault == Fault::UnsortedReferences) {
            models.emplace(record.first, builder.modelValue(record.second, true));
        } else if (record.first == key(2) && fault == Fault::DuplicateReferences) {
            models.emplace(record.first, builder.duplicateReferenceValue(key(1)));
        } else {
            models.emplace(record.first, builder.modelValue(record.second));
        }
    }
    std::map<Key, Hash> index;
    for (const std::pair<const Key, std::vector<Key>>& entry : incoming) {
        index.emplace(entry.first, builder.incomingValue(entry.second));
    }
    const Hash modelRoot = builder.tree(models);
    const Hash reverseRoot = builder.tree(index);
    if (fault == Fault::InvalidAncestor) {
        const Hash badParent = builder.revision(modelRoot, Hash{});
        return builder.revision(modelRoot, reverseRoot, badParent);
    }
    if (fault == Fault::InvalidSecondParent) {
        const Hash goodParent = builder.revision(modelRoot, reverseRoot);
        const Hash badOtherParent = builder.revision(modelRoot, Hash{});
        return builder.revision(modelRoot, reverseRoot, goodParent, badOtherParent);
    }
    if (fault == Fault::SharedRootDifferentReverseIndex) {
        const Hash goodParent = builder.revision(modelRoot, reverseRoot);
        return builder.revision(modelRoot, Hash{}, goodParent);
    }
    return builder.revision(modelRoot, reverseRoot);
}

void expectRejected(Repository& target, const std::filesystem::path& bundle,
                    const std::string& label, std::string_view reason,
                    std::string_view branch = "incoming") {
    const std::uint64_t objectsBefore = target.objectCount();
    const std::uint64_t packBefore = target.packSize();
    const Hash headBefore = target.head("stable").id;
    bool rejected = false;
    try {
        (void)target.importBranchBundle(bundle, branch);
    } catch (const aowis::native::StorageError& error) {
        rejected = true;
        expect(std::string(error.what()).find(reason) != std::string::npos,
               "wrong rejection reason for " + label + ": " + error.what());
        // A malformed bundle is client input, never local corruption (review F2).
        expect(error.kind() == aowis::native::StorageErrorKind::InvalidInput,
               "malformed bundle not classified as InvalidInput: " + label + " (" +
               aowis::native::StorageError::kindName(error.kind()) + ")");
    }
    expect(rejected, "forged model history accepted: " + label);
    expect(target.head("stable").id == headBefore &&
           target.objectCount() == objectsBefore && target.packSize() == packBefore,
           "rejected model import wrote pack objects: " + label);
    expect(target.listBranches() == std::vector<std::string>{"stable"},
           "rejected model import changed branch catalogue: " + label);
}

void testMalformedFull() {
    TemporaryDirectory temporary("forged-full");
    Repository target(temporary.path / "target");
    target.createBranch("stable");
    target.commit("stable", {{key(42), Bytes{'s', 'a', 'f', 'e'}}});
    struct Case { Fault fault; const char* label; const char* reason; };
    const std::vector<Case> cases{
        {Fault::MissingIncoming, "missing incoming", "missing model references"},
        {Fault::ExtraIncoming, "extra incoming", "does not match model records"},
        {Fault::WrongIncomingSource, "wrong source", "does not match model records"},
        {Fault::MissingTarget, "dangling outgoing", "target does not exist"},
        {Fault::ZeroTarget, "zero outgoing", "invalid bundled model UUID reference"},
        {Fault::SelfReference, "self outgoing", "invalid bundled model UUID reference"},
        {Fault::UnsortedReferences, "noncanonical order", "noncanonical bundled model references"},
        {Fault::DuplicateReferences, "duplicate outgoing", "noncanonical bundled model references"},
        {Fault::ZeroModelUuid, "zero model UUID", "model tree contains zero UUID"},
        {Fault::ZeroIncomingTarget, "zero index UUID", "incoming-reference target is zero"},
        {Fault::InvalidAncestor, "invalid historical parent", "missing model references"},
        {Fault::InvalidSecondParent, "invalid merged ancestor", "missing model references"},
        {Fault::SharedRootDifferentReverseIndex, "poisoned unchanged snapshot",
         "missing model references"}
    };
    for (const Case& invalid : cases) {
        BundleBuilder builder;
        const Hash tip = makeFixture(builder, invalid.fault);
        const std::filesystem::path path = temporary.path / "untrusted.bundle";
        builder.write(path, tip);
        expectRejected(target, path, invalid.label, invalid.reason);
        std::filesystem::remove(path);
    }
    target.verifyPack();
}

void testReproducedUnsafeDeletion() {
    TemporaryDirectory temporary("unsafe-deletion");
    Repository target(temporary.path / "target");
    target.createBranch("stable");
    target.commit("stable", {{key(42), Bytes{'b', 'a', 's', 'e'}}});
    BundleBuilder forged;
    const Hash tip = makeFixture(forged, Fault::MissingIncoming);
    const std::filesystem::path path = temporary.path / "forged.bundle";
    forged.write(path, tip);
    expectRejected(target, path, "dependent B invisible to index", "missing model references");
    // A valid import of the SAME two model records must preserve the deletion
    // invariant, rather than simply rejecting all model-containing bundles.
    BundleBuilder valid;
    std::map<Key, Hash> models{{key(1), valid.modelValue({})},
                                {key(2), valid.modelValue({key(1)})}};
    std::map<Key, Hash> inbound{{key(1), valid.incomingValue({key(2)})}};
    const Hash good = valid.revision(valid.tree(models), valid.tree(inbound));
    valid.write(temporary.path / "valid.bundle", good);
    expect(target.importBranchBundle(temporary.path / "valid.bundle", "models").head == good,
           "valid model import rejected");
    expect(target.referencingModel("models", key(1)) == std::vector<Key>{key(2)},
           "imported dependency not indexed");
    bool refused = false;
    try {
        (void)target.commitModel("models", {{key(1), std::nullopt}});
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused && target.head("models").id == good &&
           target.getModel("models", key(1)).has_value() &&
           target.getModel("models", key(2)).has_value(),
           "valid imported model allowed deletion of referenced UUID");
}

void testValidCyclesAndHistory() {
    TemporaryDirectory temporary("valid-graphs");
    Repository target(temporary.path / "target");
    BundleBuilder builder;
    std::map<Key, Hash> models{{key(1), builder.modelValue({key(2)})},
                                {key(2), builder.modelValue({key(1)})},
                                {key(3), builder.modelValue({})}};
    std::map<Key, Hash> inbound{{key(1), builder.incomingValue({key(2)})},
                                {key(2), builder.incomingValue({key(1)})}};
    const Hash root = builder.tree(models);
    const Hash references = builder.tree(inbound);
    const Hash initial = builder.revision(root, references);
    const Hash next = builder.revision(root, references, initial);
    builder.write(temporary.path / "cycles.bundle", next);
    expect(target.importBranchBundle(temporary.path / "cycles.bundle", "models").head == next &&
           target.referencingModelAtRevision(initial, key(1)) == std::vector<Key>{key(2)} &&
           target.referencingModel("models", key(2)) == std::vector<Key>{key(1)},
           "valid model cycles/shared historical roots not preserved");
    target.verifyPack();
}

// Exercise multiple externally sorted runs, including a reverse-index defect
// after the in-memory sort threshold. Small fixtures do not exercise the
// disk-backed merge, so this is deliberately >8,192 distinct edges.
void testLargeReferenceGraph() {
    TemporaryDirectory temporary("large-reference-graph");
    // Scratch files live in <repository>/tmp (review F6). A nonexistent
    // TMPDIR proves that external sorting no longer depends on it.
    const char* previous = ::getenv("TMPDIR");
    const std::optional<std::string> oldTmp = previous == nullptr ?
        std::nullopt : std::optional<std::string>(previous);
    expect(::setenv("TMPDIR", (temporary.path / "no-such-tmpdir").c_str(), 1) == 0,
           "set unusable TMPDIR");
    struct RestoreEnvironment {
        std::optional<std::string> value;
        ~RestoreEnvironment() {
            if (this->value) ::setenv("TMPDIR", this->value->c_str(), 1);
            else ::unsetenv("TMPDIR");
        }
    } restore{oldTmp};
    BundleBuilder builder;
    std::vector<Key> targets;
    std::vector<Key> sources;
    for (unsigned value = 1; value <= 64; ++value)
        targets.push_back(key(static_cast<std::uint8_t>(value)));
    for (unsigned value = 65; value <= 224; ++value)
        sources.push_back(key(static_cast<std::uint8_t>(value)));
    std::map<Key, Hash> models;
    for (const Key& target : targets)
        models.emplace(target, builder.modelValue({}));
    for (const Key& source : sources)
        models.emplace(source, builder.modelValue(targets));
    const Hash root = builder.tree(models);
    std::map<Key, Hash> incoming;
    for (const Key& target : targets)
        incoming.emplace(target, builder.incomingValue(sources));
    const Hash valid = builder.revision(root, builder.tree(incoming));
    builder.write(temporary.path / "valid.bundle", valid);
    Repository receiver(temporary.path / "receiver");
    expect(receiver.importBranchBundle(temporary.path / "valid.bundle", "models").head == valid,
           "large reference graph import rejected");
    expect(receiver.referencingModel("models", targets[17]).size() == sources.size(),
           "large graph reverse index lost edges");
    (void)receiver.verifyRepository();
    expect(std::filesystem::is_empty(receiver.scratchDirectory()),
           "reference sorting left temporary files after valid import");

    // If scratch space cannot be created, fail before importing any objects
    // or publishing a branch. This exercises the external-sort failure path
    // rather than allowing a fallback that silently skips verification.
    Repository inaccessible(temporary.path / "inaccessible");
    const std::filesystem::path unusable = inaccessible.scratchDirectory();
    std::filesystem::remove(unusable);
    { std::ofstream blocker(unusable); } // a regular file where the directory belongs
    bool failedWithoutScratch = false;
    try {
        (void)inaccessible.importBranchBundle(temporary.path / "valid.bundle", "models");
    } catch (const std::exception&) {
        failedWithoutScratch = true;
    }
    expect(failedWithoutScratch && inaccessible.listBranches().empty() &&
           inaccessible.objectCount() == 0,
           "external-sort scratch failure allowed partial imported state");

    // Change just one stored reverse index value while keeping every object
    // hash and trie path valid. The sorted comparison must find the omission.
    BundleBuilder forged;
    std::map<Key, Hash> badModels;
    for (const Key& target : targets)
        badModels.emplace(target, forged.modelValue({}));
    for (const Key& source : sources)
        badModels.emplace(source, forged.modelValue(targets));
    std::map<Key, Hash> badIncoming;
    for (const Key& target : targets) {
        std::vector<Key> actual = sources;
        if (target == targets[37]) actual.erase(actual.begin() + 87);
        badIncoming.emplace(target, forged.incomingValue(actual));
    }
    const Hash invalid = forged.revision(forged.tree(badModels), forged.tree(badIncoming));
    forged.write(temporary.path / "forged.bundle", invalid);
    Repository rejected(temporary.path / "rejected");
    rejected.createBranch("stable");
    (void)rejected.commit("stable", {{key(230), Bytes{'s'}}});
    expectRejected(rejected, temporary.path / "forged.bundle", "large missing incoming edge",
                   "does not match model records");
    expect(std::filesystem::is_empty(rejected.scratchDirectory()),
           "reference sorting left temporary files after invalid import");
}

// Revision IDs are hash-sorted in the bundle, not ancestry-sorted. The
// verifier must resolve parents before children without a recursive C++ call
// stack, even when thousands of revisions share the same immutable roots.
void testDeepSharedModelHistory() {
    TemporaryDirectory temporary("deep-model-history");
    BundleBuilder builder;
    const Hash value = builder.modelValue({});
    const Hash modelRoot = builder.tree({{key(1), value}});
    Hash tip = builder.revision(modelRoot, Hash{});
    for (unsigned i = 0; i < 2500; ++i) {
        tip = builder.revision(modelRoot, Hash{}, tip);
    }
    const std::filesystem::path bundle = temporary.path / "long.bundle";
    builder.write(bundle, tip);
    Repository receiver(temporary.path / "receiver");
    expect(receiver.importBranchBundle(bundle, "models").head == tip,
           "deep history model import failed");
    expect(receiver.verifyRepository().revisions == 2501,
           "deep history lost revision ancestry");
}

// Independent, correctly hashed historical snapshots exercise the differential
// validator's trust boundary: a consistent parent alone must never make a
// forged child safe, particularly when it deletes a still-referenced UUID.
void testHistoricalReferenceDeltas() {
    TemporaryDirectory temporary("reference-deltas");
    BundleBuilder builder;
    const Hash empty = builder.modelValue({});
    const Hash depends = builder.modelValue({key(1)});
    const Hash dependsOnMissing = builder.modelValue({key(9)});
    const Hash incomingTwo = builder.incomingValue({key(2)});
    const Hash incomingTwoThree = builder.incomingValue({key(2), key(3)});
    std::map<Key, Hash> baseModels{{key(1), empty}, {key(2), depends}};
    std::map<Key, Hash> baseIndex{{key(1), incomingTwo}};
    const Hash baseModelRoot = builder.tree(baseModels);
    const Hash baseRefRoot = builder.tree(baseIndex);
    const Hash parent = builder.revision(baseModelRoot, baseRefRoot);

    struct Case { std::string label; Hash tip; bool valid; };
    std::vector<Case> cases;
    {
        std::map<Key, Hash> models = baseModels;
        models.emplace(key(3), depends);
        // A changed model with a missing incoming edge must be rejected.
        cases.push_back({"new dependent absent from reverse index",
                         builder.revision(builder.tree(models), baseRefRoot, parent), false});
        std::map<Key, Hash> correct = baseIndex;
        correct[key(1)] = incomingTwoThree;
        cases.push_back({"correct added dependent",
                         builder.revision(builder.tree(models), builder.tree(correct), parent), true});
    }
    {
        std::map<Key, Hash> models = baseModels;
        models.erase(key(1));
        // The referencing model at UUID 2 is unchanged. The old reverse
        // index still records that incoming edge, so deleting 1 is illegal.
        cases.push_back({"delete referenced target without index change",
                         builder.revision(builder.tree(models), baseRefRoot, parent), false});
        cases.push_back({"delete referenced target and hide incoming edges",
                         builder.revision(builder.tree(models), Hash{}, parent), false});
    }
    {
        std::map<Key, Hash> models = baseModels;
        models[key(2)] = dependsOnMissing;
        std::map<Key, Hash> refs{{key(9), builder.incomingValue({key(2)})}};
        cases.push_back({"changed dependent points to nonexistent model",
                         builder.revision(builder.tree(models), builder.tree(refs), parent), false});
    }
    {
        // An unchanged dependency set may accompany an updated record value.
        // This is a valid differential update with no reference-index edits.
        std::map<Key, Hash> models = baseModels;
        models[key(2)] = builder.add([] {
            Bytes value{'V'};
            const Bytes record = aowis::native::encodeModelRecord(
                ModelRecord{1, {key(1)}, Bytes{'n', 'e', 'w'}});
            value.insert(value.end(), record.begin(), record.end());
            return value;
        }());
        cases.push_back({"payload-only edit",
                         builder.revision(builder.tree(models), baseRefRoot, parent), true});
    }
    {
        std::map<Key, Hash> models = baseModels;
        models.erase(key(2));
        cases.push_back({"remove dependent and its incoming edge",
                         builder.revision(builder.tree(models), Hash{}, parent), true});
    }
    {
        // A valid merge tip cannot launder a poisoned second-parent revision.
        const Hash bad = builder.revision(baseModelRoot, Hash{}, parent);
        cases.push_back({"poisoned second merge parent",
                         builder.revision(baseModelRoot, baseRefRoot, parent, bad), false});
    }
    for (const Case& fixture : cases) {
        const std::filesystem::path bundle = temporary.path / (fixture.label + ".bundle");
        // BundleBuilder accumulates cases; export only the objects reachable
        // from this particular tip, exactly as a real branch export does.
        BundleBuilder filtered;
        const std::function<void(const Hash&)> include = [&](const Hash& hash) {
            if (hash == Hash{} || filtered.objects.contains(hash)) return;
            const Bytes& object = builder.objects.at(hash);
            filtered.objects.emplace(hash, object);
            const std::function<Hash(std::size_t)> hashAt = [&](std::size_t offset) {
                Hash child{};
                std::copy_n(object.begin() + static_cast<std::ptrdiff_t>(offset),
                            32, child.begin());
                return child;
            };
            if (object[0] == 'S') {
                include(hashAt(1));
                include(hashAt(33));
                for (unsigned i = 0; i < object[66]; ++i) include(hashAt(67 + 32U * i));
            } else if (object[0] == 'B') {
                for (unsigned i = 0; i < 16; ++i) include(hashAt(18 + 32U * i));
            } else if (object[0] == 'L') {
                include(hashAt(17));
            }
        };
        include(fixture.tip);
        filtered.write(bundle, fixture.tip);
        Repository receiver(temporary.path / fixture.label);
        const std::uint64_t initialObjects = receiver.objectCount();
        if (fixture.valid) {
            expect(receiver.importBranchBundle(bundle, "models").head == fixture.tip,
                   "valid historical model delta was rejected: " + fixture.label);
            expect(receiver.verifyRepository().revisions >= 2,
                   "valid historical history failed independent verification");
        } else {
            bool rejected = false;
            try {
                (void)receiver.importBranchBundle(bundle, "models");
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            expect(rejected && receiver.listBranches().empty() &&
                   receiver.objectCount() == initialObjects,
                   "invalid historical reference delta accepted: " + fixture.label);
        }
    }
}

void testMalformedIncremental() {
    TemporaryDirectory temporary("incremental");
    Repository source(temporary.path / "source");
    Repository destination(temporary.path / "destination");
    source.createBranch("models");
    const Hash base = source.commitModel("models", {
        {key(1), ModelRecord{1, {}, Bytes{'a'}}},
        {key(2), ModelRecord{1, {key(1)}, Bytes{'b'}}}}).id;
    source.exportBranchBundle("models", temporary.path / "base.bundle");
    destination.importBranchBundle(temporary.path / "base.bundle", "stable");

    BundleBuilder forged;
    std::map<Key, Hash> models{{key(1), forged.modelValue({})},
                                {key(2), forged.modelValue({key(1)})},
                                {key(3), forged.modelValue({key(1)})}};
    const Hash root = forged.tree(models);
    std::map<Key, Hash> badInbound{{key(1), forged.incomingValue({key(2)})}};
    const Hash malicious = forged.revision(root, forged.tree(badInbound), base);
    forged.write(temporary.path / "bad-incremental.bundle", malicious, base);
    expectRejected(destination, temporary.path / "bad-incremental.bundle",
                   "forged incremental fast-forward", "does not match model records", "stable");

    // A real incremental bundle should still round-trip between two
    // stores, including borrowed tree/value objects from the existing base.
    const Hash latest = source.commitModel("models", {
        {key(3), ModelRecord{1, {key(1)}, Bytes{'c'}}}}).id;
    source.exportIncrementalBranchBundle("models", base,
                                         temporary.path / "valid-incremental.bundle");
    expect(destination.importBranchBundle(temporary.path / "valid-incremental.bundle",
                                          "stable").status == BundleImportStatus::FastForward &&
           destination.head("stable").id == latest &&
           destination.referencingModel("stable", key(1)) ==
               (std::vector<Key>{key(2), key(3)}),
           "valid incremental model synchronization failed");
}

} // namespace

int main() {
    try {
        {
            testMalformedFull();
            testReproducedUnsafeDeletion();
            testValidCyclesAndHistory();
            testLargeReferenceGraph();
            testHistoricalReferenceDeltas();
            testDeepSharedModelHistory();
        }
        testMalformedIncremental();
        std::cout << "Native model import integrity tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native model import integrity tests failed: " << error.what() << '\n';
        return 1;
    }
}
