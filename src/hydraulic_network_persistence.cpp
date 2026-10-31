#include "aowis/dvmdb/hydraulic_network_persistence.h"
#include "aowis/dvmdb/revision_record_v2.h"
#include "aowis/dvmdb/storage_error.h"

#include <aowis/dvmdb/model_validation.h>
#include <aowis/dvmdb/model_field_merge.h>

#include <QDataStream>
#include <QCryptographicHash>
#include <QHash>
#include <QIODevice>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <array>
#include <functional>
#include <limits>
#include <initializer_list>
#include <set>
#include <map>
#include <type_traits>
#include <stdexcept>
#include <utility>

namespace aowis::dvmdb::hydraulic {
namespace {

// Persist all 18 canonical hydraulic entity types using native numeric
// kind IDs for the experimental engine. The original junction/pipe
// values MUST remain unchanged. These are NOT public stable wire type tags.
#define AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(X) \
    X(pipe_materials, HydraulicPipeMaterial, 8) \
    X(patterns_time, HydraulicPatternTime, 9) \
    X(curves_tank_volume, HydraulicCurveTankVolume, 10) \
    X(curves_pump_head, HydraulicCurvePumpHead, 11) \
    X(curves_pump_efficiency, HydraulicCurvePumpEfficiency, 12) \
    X(curves_valve_headloss, HydraulicCurveValveHeadloss, 13) \
    X(curves_valve_characteristic, HydraulicCurveValveCharacteristic, 14) \
    X(curves_generic, HydraulicCurveGeneric, 15) \
    X(nodes_reservoirs, HydraulicNodeReservoir, 2) \
    X(nodes_junctions, HydraulicNodeJunction, junctionKind) \
    X(nodes_tanks, HydraulicNodeTank, 3) \
    X(demand_points, HydraulicDemandPoint, 7) \
    X(links_pipes, HydraulicLinkPipe, pipeKind) \
    X(links_pumps, HydraulicLinkPump, 5) \
    X(links_valves, HydraulicLinkValve, 6) \
    X(controls_simple, HydraulicControlSimple, 16) \
    X(controls_rules, HydraulicControlRule, 17) \
    X(map_labels, HydraulicMapLabel, 18)

constexpr quint32 manifestMagic = 0x414E5835U;
constexpr quint32 manifestVersion = 2; // pre-release format: chunk references, not flat lists
constexpr quint32 chunkMagic = 0x41434B32U; // ACK2
constexpr quint32 chunkVersion = 1;
constexpr std::size_t collectionCount = 18;
constexpr qsizetype maxManifestBytes = 256 * 1024 * 1024;
constexpr qsizetype chunkCapacity = 256;
constexpr std::uint16_t chunkKind = 101;

struct Manifest {
    NetworkHydraulic metadata;
    // Populated only by manifestFor()/loadMembersAtRevision(), never serialized inline.
    std::array<QList<QUuid>, collectionCount> members;
    std::array<QList<QUuid>, collectionCount> chunks;
    std::array<quint32, collectionCount> nextChunk{};
};

struct MemberChunk {
    QUuid network;
    std::size_t collection = 0;
    QList<QUuid> members;
};

void clearCollections(NetworkHydraulic& network) {
#define CLEAR(name, type, kind) network.name.clear();
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(CLEAR)
#undef CLEAR
}

QByteArray encodeManifest(const Manifest& manifest) {
    QByteArray output;
    QDataStream stream(&output, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    const QByteArray metadata = aowis::dvmdb::ModelCodec<NetworkHydraulic>::encode(manifest.metadata);
    if (metadata.isEmpty()) throw StorageError(StorageErrorKind::Internal, "cannot encode hydraulic network metadata");
    stream << manifestMagic << manifestVersion << metadata;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        stream << manifest.nextChunk[index] << manifest.chunks[index];
    }
    if (stream.status() != QDataStream::Ok || output.size() > maxManifestBytes)
        throw StorageError(StorageErrorKind::LimitExceeded, "hydraulic network manifest exceeds encoding limit");
    return output;
}

Manifest decodeManifest(const QByteArray& bytes) {
    if (bytes.size() > maxManifestBytes) throw StorageError(StorageErrorKind::LimitExceeded, "hydraulic network manifest too large");
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray metadata;
    stream >> magic >> version >> metadata;
    if (stream.status() != QDataStream::Ok || magic != manifestMagic || version != manifestVersion)
        throw StorageError(StorageErrorKind::Corruption, "unsupported hydraulic network manifest (requires fresh v2 repository)");
    QString error;
    std::optional<NetworkHydraulic> decoded = aowis::dvmdb::ModelCodec<NetworkHydraulic>::decode(metadata, &error);
    if (!decoded) throw StorageError(StorageErrorKind::Corruption, "invalid hydraulic network metadata: " + error.toStdString());
    Manifest result;
    result.metadata = std::move(*decoded);
#define CHECK_EMPTY(name, type, kind) \
    if (!result.metadata.name.isEmpty()) throw StorageError(StorageErrorKind::Corruption, "network metadata contains inline " #name);
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(CHECK_EMPTY)
#undef CHECK_EMPTY
    std::set<Key> seen;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        stream >> result.nextChunk[index] >> result.chunks[index];
        for (const QUuid& id : result.chunks[index]) {
            if (id.isNull() || id == result.metadata.uuid || !seen.insert(keyFromUuid(id)).second)
                throw StorageError(StorageErrorKind::Corruption, "duplicate/invalid hydraulic membership chunk ID");
        }
        if (result.chunks[index].size() > result.nextChunk[index])
            throw StorageError(StorageErrorKind::Corruption, "invalid hydraulic membership chunk serial");
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        throw StorageError(StorageErrorKind::Corruption, "truncated or trailing hydraulic manifest bytes");
    return result;
}

QUuid chunkUuid(const QUuid& network, std::size_t collection, quint32 serial) {
    QByteArray seed("aowis-network-member-chunk-v2");
    seed.append(network.toRfc4122());
    for (int shift = 24; shift >= 0; shift -= 8) seed.append(static_cast<char>((collection >> shift) & 255U));
    for (int shift = 24; shift >= 0; shift -= 8) seed.append(static_cast<char>((serial >> shift) & 255U));
    QByteArray digest = QCryptographicHash::hash(seed, QCryptographicHash::Sha256).left(16);
    digest[6] = static_cast<char>((static_cast<unsigned char>(digest[6]) & 0x0fU) | 0x80U);
    digest[8] = static_cast<char>((static_cast<unsigned char>(digest[8]) & 0x3fU) | 0x80U);
    return QUuid::fromRfc4122(digest);
}

ModelRecord encodeChunk(const MemberChunk& chunk) {
    if (chunk.network.isNull() || chunk.collection >= collectionCount ||
        chunk.members.isEmpty() || chunk.members.size() > chunkCapacity)
        throw StorageError(StorageErrorKind::Corruption, "invalid membership chunk");
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << chunkMagic << chunkVersion << chunk.network << quint32(chunk.collection) << chunk.members;
    if (stream.status() != QDataStream::Ok) throw StorageError(StorageErrorKind::Internal, "membership chunk encoding failed");
    ModelRecord record;
    record.kind = chunkKind;
    record.payload = bytesFromQByteArray(bytes);
    std::set<Key> unique;
    for (const QUuid& id : chunk.members) {
        if (id.isNull() || id == chunk.network || !unique.insert(keyFromUuid(id)).second)
            throw StorageError(StorageErrorKind::Corruption, "invalid or duplicate member in chunk");
    }
    record.references.assign(unique.begin(), unique.end());
    return record;
}

MemberChunk decodeChunk(const ModelRecord& record, const QUuid& network, std::size_t collection) {
    if (record.kind != chunkKind) throw StorageError(StorageErrorKind::Corruption, "wrong membership chunk kind");
    QDataStream stream(qByteArrayFromBytes(record.payload));
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    quint32 ordinal = 0;
    MemberChunk chunk;
    stream >> magic >> version >> chunk.network >> ordinal >> chunk.members;
    chunk.collection = ordinal;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || magic != chunkMagic ||
        version != chunkVersion || chunk.network != network || ordinal != collection)
        throw StorageError(StorageErrorKind::Corruption, "invalid membership chunk payload");
    if (encodeChunk(chunk).references != record.references)
        throw StorageError(StorageErrorKind::Corruption, "membership chunk reference index mismatch");
    return chunk;
}

std::vector<Key> chunkKeys(const Manifest& manifest) {
    std::set<Key> keys;
    for (const QList<QUuid>& group : manifest.chunks)
        for (const QUuid& id : group) keys.insert(keyFromUuid(id));
    return std::vector<Key>(keys.begin(), keys.end());
}

void checkRoot(const Manifest& manifest, const ModelRecord& root) {
    if (root.kind != hydraulicNetworkKind || root.references != chunkKeys(manifest))
        throw StorageError(StorageErrorKind::Corruption, "hydraulic root/chunk references disagree");
}

// Resolve every chunk against the caller's chosen snapshot. Writers retain
// branch-based reads during preparation (their commit checks expectedHead),
// while public readers always supply one immutable revision.
// maxMembers bounds untrusted listings: chunks hold at most chunkCapacity
// members, so at most one chunk beyond the limit is ever decoded.
template<typename ReadModel>
void loadMembersUsing(ReadModel readModel, Manifest& manifest,
                      std::optional<std::size_t> onlyCollection = std::nullopt,
                      std::size_t maxMembers = std::numeric_limits<std::size_t>::max()) {
    std::set<Key> seen;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        if (onlyCollection && *onlyCollection != index) continue;
        for (const QUuid& id : manifest.chunks[index]) {
            const std::optional<ModelRecord> record = readModel(keyFromUuid(id));
            if (!record) throw StorageError(StorageErrorKind::Corruption, "missing hydraulic membership chunk");
            const MemberChunk chunk = decodeChunk(*record, manifest.metadata.uuid, index);
            for (const QUuid& member : chunk.members) {
                if (!seen.insert(keyFromUuid(member)).second)
                    throw StorageError(StorageErrorKind::Corruption, "duplicate network member");
                manifest.members[index].append(member);
            }
            if (static_cast<std::size_t>(manifest.members[index].size()) > maxMembers)
                throw StorageError(StorageErrorKind::LimitExceeded,
                                   "hydraulic entity collection exceeds the requested limit");
        }
    }
}

void loadMembersAtRevision(Repository& repository, const Hash& revisionId, Manifest& manifest,
                           std::optional<std::size_t> onlyCollection = std::nullopt,
                           std::size_t maxMembers = std::numeric_limits<std::size_t>::max()) {
    loadMembersUsing([&repository, &revisionId](const Key& key) {
        return repository.getModelAtRevision(revisionId, key);
    }, manifest, onlyCollection, maxMembers);
}

void loadMembersAtRevisionV2(Repository& repository, const ProjectId& project,
                             const RevisionId& revisionId, Manifest& manifest,
                             std::optional<std::size_t> onlyCollection = std::nullopt,
                             std::size_t maxMembers = std::numeric_limits<std::size_t>::max()) {
    loadMembersUsing([&repository, &project, &revisionId](const Key& key) {
        return repository.getModelAtRevisionV2(project, revisionId, key);
    }, manifest, onlyCollection, maxMembers);
}

std::set<Key> memberKeys(const Manifest& manifest) {
    std::set<Key> keys;
    for (const QList<QUuid>& group : manifest.members) {
        for (const QUuid& id : group) {
            const Key key = keyFromUuid(id);
            if (id.isNull() || id == manifest.metadata.uuid || !keys.insert(key).second)
                throw StorageError(StorageErrorKind::InvalidInput, "duplicate or network-owned hydraulic entity UUID");
        }
    }
    return keys;
}

// A root owns a chunk; a chunk owns the entity. Validate both links before
// using the reverse-reference index to establish network ownership.
template<typename ReadModel, typename ReadReferrers>
std::optional<Key> ownerChunkUsing(ReadModel readModel, ReadReferrers readReferrers,
                                    const QUuid& network, const Key& member) {
    const std::optional<ModelRecord> root = readModel(keyFromUuid(network));
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    std::optional<Key> found;
    for (const Key& parent : readReferrers(member)) {
        const std::optional<ModelRecord> candidate = readModel(parent);
        if (!candidate || candidate->kind != chunkKind) continue;
        if (!std::binary_search(root->references.begin(), root->references.end(), parent)) continue;
        const Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
        bool declared = false;
        for (std::size_t group = 0; group < collectionCount; ++group) {
            if (manifest.chunks[group].contains(uuidFromKey(parent))) {
                const MemberChunk owned = decodeChunk(*candidate, network, group);
                if (!owned.members.contains(uuidFromKey(member)))
                    throw StorageError(StorageErrorKind::Corruption, "reverse index claims member absent from chunk");
                declared = true;
                break;
            }
        }
        if (!declared) throw StorageError(StorageErrorKind::Corruption, "root references undeclared membership chunk");
        if (found) throw StorageError(StorageErrorKind::Corruption, "entity owned by multiple membership chunks");
        found = parent;
    }
    return found;
}

std::optional<Key> ownerChunk(Repository& repository, std::string_view branch,
                              const QUuid& network, const Key& member) {
    return ownerChunkUsing([&repository, branch](const Key& key) {
        return repository.getModel(branch, key);
    }, [&repository, branch](const Key& key) {
        return repository.referencingModel(branch, key);
    }, network, member);
}

std::optional<Key> ownerChunkAtRevision(Repository& repository, const Hash& revisionId,
                                        const QUuid& network, const Key& member) {
    return ownerChunkUsing([&repository, &revisionId](const Key& key) {
        return repository.getModelAtRevision(revisionId, key);
    }, [&repository, &revisionId](const Key& key) {
        return repository.referencingModelAtRevision(revisionId, key);
    }, network, member);
}

std::optional<Key> ownerChunkAtRevisionV2(Repository& repository, const ProjectId& project,
                                          const RevisionId& revisionId,
                                          const QUuid& network, const Key& member) {
    return ownerChunkUsing([&repository, &project, &revisionId](const Key& key) {
        return repository.getModelAtRevisionV2(project, revisionId, key);
    }, [&repository, &project, &revisionId](const Key& key) {
        return repository.referencingModelAtRevisionV2(project, revisionId, key);
    }, network, member);
}

// Rebuild membership chunks from a complete in-memory model. Used for full
// save and conservative fallback; fast membership edits only touch one chunk.
void buildChunks(Manifest& manifest, std::vector<ModelChange>& changes) {
    for (std::size_t collection = 0; collection < collectionCount; ++collection) {
        const QList<QUuid>& all = manifest.members[collection];
        for (qsizetype offset = 0; offset < all.size(); offset += chunkCapacity) {
            const QUuid id = chunkUuid(manifest.metadata.uuid, collection, manifest.nextChunk[collection]++);
            manifest.chunks[collection].append(id);
            MemberChunk chunk{manifest.metadata.uuid, collection, all.mid(offset, chunkCapacity)};
            changes.push_back(ModelChange{keyFromUuid(id), encodeChunk(chunk)});
        }
    }
}

// The native hydraulic validator gives the semantic relationships, including
// demand patterns, pipe material, controls, and MSX references. Nested MSX
// entities are *inside* the network metadata and must not become external
// object dependencies; only top-level persisted model members are indexed.
// Sort fixed-size UUID edges instead of maintaining one Qt hash/set allocation
// per source. This is particularly important for 100k+ pipe imports. The
// canonical validator has already checked all edges in this graph.
using Dependency = std::pair<Key, Key>;
using DependencyIndex = std::vector<Dependency>;

DependencyIndex indexedDependencies(
    const QVector<aowis::dvmdb::HydraulicDependency>& dependencies,
    const std::set<Key>& members) {
    DependencyIndex result;
    result.reserve(static_cast<std::size_t>(dependencies.size()));
    for (const aowis::dvmdb::HydraulicDependency& dependency : dependencies) {
        if (dependency.source_uuid.isNull() || dependency.target_uuid.isNull()) continue;
        const Key source = keyFromUuid(dependency.source_uuid);
        const Key target = keyFromUuid(dependency.target_uuid);
        if (members.contains(source) && members.contains(target))
            result.emplace_back(source, target);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<Key> entityReferences(const DependencyIndex& references, const Key& source) {
    const Dependency first{source, Key{}};
    DependencyIndex::const_iterator it = std::lower_bound(references.begin(), references.end(), first);
    std::vector<Key> result;
    while (it != references.end() && it->first == source) {
        result.push_back(it->second);
        ++it;
    }
    return result;
}

template<typename T>
void addEntities(const QList<T>& entities, std::uint16_t kind,
                 const DependencyIndex& references,
                 std::vector<ModelChange>& changes) {
    for (const T& entity : entities) {
        ModelRecord record;
        record.kind = kind;
        record.payload = bytesFromQByteArray(aowis::dvmdb::ModelCodec<T>::encode(entity));
        if (record.payload.empty()) throw StorageError(StorageErrorKind::Internal, "cannot encode hydraulic model entity");
        const Key key = keyFromUuid(entity.uuid);
        record.references = entityReferences(references, key);
        changes.push_back(ModelChange{key, std::move(record)});
    }
}

template<typename T>
void restoreEntities(Repository& repository, const Hash& revisionId,
                     const QList<QUuid>& ids, std::uint16_t kind,
                     QList<T>& output, std::vector<std::pair<Key, Key>>& storedReferences) {
    for (const QUuid& uuid : ids) {
        const std::optional<ModelRecord> record =
            repository.getModelAtRevision(revisionId, keyFromUuid(uuid));
        if (!record || record->kind != kind) {
            throw StorageError(StorageErrorKind::Corruption, "missing or wrong-type hydraulic entity: " + uuid.toString().toStdString());
        }
        QString error;
        std::optional<T> entity = aowis::dvmdb::ModelCodec<T>::decode(qByteArrayFromBytes(record->payload), &error);
        if (!entity || entity->uuid != uuid) {
            throw StorageError(StorageErrorKind::Corruption, "invalid hydraulic entity payload: " + uuid.toString().toStdString() +
                                     " " + error.toStdString());
        }
        for (const Key& target : record->references)
            storedReferences.emplace_back(keyFromUuid(uuid), target);
        output.append(std::move(*entity));
    }
}

template<typename T>
void restoreEntitiesV2(Repository& repository, const ProjectId& project,
                       const RevisionId& revisionId, const QList<QUuid>& ids,
                       std::uint16_t kind, QList<T>& output,
                       std::vector<std::pair<Key, Key>>& storedReferences) {
    for (const QUuid& uuid : ids) {
        const std::optional<ModelRecord> record =
            repository.getModelAtRevisionV2(project, revisionId, keyFromUuid(uuid));
        if (!record || record->kind != kind)
            throw StorageError(StorageErrorKind::Corruption, "missing or wrong-type hydraulic entity");
        QString error;
        std::optional<T> entity = ModelCodec<T>::decode(qByteArrayFromBytes(record->payload), &error);
        if (!entity || entity->uuid != uuid)
            throw StorageError(StorageErrorKind::Corruption,
                               "invalid hydraulic entity payload: " + error.toStdString());
        for (const Key& target : record->references)
            storedReferences.emplace_back(keyFromUuid(uuid), target);
        output.append(std::move(*entity));
    }
}

// Build the membership/index representation shared by whole-network saves
// and edit-oriented commits. The manifest never duplicates entity payloads.
Manifest manifestFor(const NetworkHydraulic& network) {
    Manifest result;
    result.metadata = network;
    clearCollections(result.metadata);
    std::size_t ordinal = 0;
#define LIST_IDS(name, type, kind) \
    for (const type& item : network.name) result.members[ordinal].append(item.uuid); \
    ++ordinal;
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(LIST_IDS)
#undef LIST_IDS
    return result;
}

// Map UUIDs to the collection that owns them. The memberKeys check also
// rejects UUID collisions across different collections and with the root.
std::map<Key, std::size_t> collectionOwners(const Manifest& manifest) {
    memberKeys(manifest);
    std::map<Key, std::size_t> owners;
    for (std::size_t i = 0; i < manifest.members.size(); ++i) {
        for (const QUuid& id : manifest.members[i]) owners.emplace(keyFromUuid(id), i);
    }
    return owners;
}

void applyEntityChange(NetworkHydraulic& network, const HydraulicEntityChange& change,
                       std::size_t collection) {
    std::size_t ordinal = 0;
#define APPLY_EDIT(name, type, kind) \
    if (collection == ordinal) { \
        QList<type>& list = network.name; \
        qsizetype found = -1; \
        for (qsizetype i = 0; i < list.size(); ++i) { \
            if (list[i].uuid == change.uuid) { found = i; break; } \
        } \
        if (!change.value) { \
            if (found < 0) throw StorageError(StorageErrorKind::Internal, "hydraulic entity not found for deletion"); \
            list.removeAt(found); \
        } else { \
            const type* value = std::get_if<type>(&*change.value); \
            if (value == nullptr) throw StorageError(StorageErrorKind::Internal, "hydraulic entity kind mismatch"); \
            if (found >= 0) list[found] = *value; \
            else list.append(*value); \
        } \
        return; \
    } \
    ++ordinal;
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(APPLY_EDIT)
#undef APPLY_EDIT
    throw StorageError(StorageErrorKind::Internal, "unknown hydraulic entity collection");
}

// The same canonical codec and model reference extraction as full saves.
ModelChange encodeChangedEntity(const HydraulicEntityChange& change,
                                const DependencyIndex& references) {
    ModelChange result;
    result.key = keyFromUuid(change.uuid);
    if (!change.value) return result;
    ModelRecord record;
    static_assert(std::variant_size_v<HydraulicEntity> == collectionCount);
    const std::array<std::uint16_t, collectionCount> kinds = {
#define MEMBER_KIND(name, type, kind) static_cast<std::uint16_t>(kind),
        AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(MEMBER_KIND)
#undef MEMBER_KIND
    };
    record.kind = kinds[change.value->index()];
    std::visit([&record](const auto& entity) {
        using T = std::decay_t<decltype(entity)>;
        record.payload = bytesFromQByteArray(aowis::dvmdb::ModelCodec<T>::encode(entity));
    }, *change.value);
    if (record.payload.empty()) throw StorageError(StorageErrorKind::Internal, "cannot encode changed hydraulic entity");
    record.references = entityReferences(references, result.key);
    // The persisted ModelRecord's reference list is canonicalized by
    // commitModel(). Match that ordering before testing for no-op changes.
    std::sort(record.references.begin(), record.references.end());
    record.references.erase(std::unique(record.references.begin(), record.references.end()),
                            record.references.end());
    result.record = std::move(record);
    return result;
}

// The canonical validator currently checks UUID identity, cross-object
// references, demand-point pipe allocation and MSX metadata. The only safe
// bypass is an explicitly whitelisted payload edit that cannot modify *any*
// of these inputs. Compare canonical bytes after copying selected fields from
// the proposed value into the already validated stored value. A newly added
// model field therefore falls back to full validation automatically.
bool scalarOnlyChange(const HydraulicEntity& proposed, const ModelRecord& old,
                      const QUuid& uuid) {
    if (const HydraulicLinkPipe* changed = std::get_if<HydraulicLinkPipe>(&proposed)) {
        if (old.kind != pipeKind) return false;
        QString error;
        const std::optional<HydraulicLinkPipe> prior = ModelCodec<HydraulicLinkPipe>::decode(
            qByteArrayFromBytes(old.payload), &error);
        if (!prior || prior->uuid != uuid) return false;
        HydraulicLinkPipe expected = *prior;
        expected.diameter_mm = changed->diameter_mm;
        expected.length_calculated_m = changed->length_calculated_m;
        expected.length_measured_m = changed->length_measured_m;
        expected.roughness_hazen_williams = changed->roughness_hazen_williams;
        expected.roughness_darcy_weisbach_mm = changed->roughness_darcy_weisbach_mm;
        expected.roughness_chezy_manning = changed->roughness_chezy_manning;
        expected.minor_loss_coefficient = changed->minor_loss_coefficient;
        // No endpoint, material, metadata or other structural edit is allowed.
        return ModelCodec<HydraulicLinkPipe>::encode(expected) ==
               ModelCodec<HydraulicLinkPipe>::encode(*changed);
    }
    if (const HydraulicNodeJunction* changed = std::get_if<HydraulicNodeJunction>(&proposed)) {
        if (old.kind != junctionKind) return false;
        QString error;
        const std::optional<HydraulicNodeJunction> prior = ModelCodec<HydraulicNodeJunction>::decode(
            qByteArrayFromBytes(old.payload), &error);
        if (!prior || prior->uuid != uuid) return false;
        HydraulicNodeJunction expected = *prior;
        expected.elevation_m = changed->elevation_m;
        expected.terrain_elevation_m = changed->terrain_elevation_m;
        expected.elevation_offset_m = changed->elevation_offset_m;
        return ModelCodec<HydraulicNodeJunction>::encode(expected) ==
               ModelCodec<HydraulicNodeJunction>::encode(*changed);
    }
    if (const HydraulicMapLabel* changed = std::get_if<HydraulicMapLabel>(&proposed)) {
        if (old.kind != 18) return false;
        QString error;
        const std::optional<HydraulicMapLabel> prior = ModelCodec<HydraulicMapLabel>::decode(
            qByteArrayFromBytes(old.payload), &error);
        if (!prior || prior->uuid != uuid) return false;
        HydraulicMapLabel expected = *prior;
        expected.text = changed->text;
        return ModelCodec<HydraulicMapLabel>::encode(expected) ==
               ModelCodec<HydraulicMapLabel>::encode(*changed);
    }
    return false;
}

// Exactly match the canonical validator's reference extraction for the three
// scalar-edit types. An invalid or noncanonical old reference index must not
// be propagated by the fast path, even if the new payload changes no links.
std::vector<Key> scalarReferences(const HydraulicEntity& entity) {
    std::vector<Key> references;
    const auto append = [&references](const QUuid& id) {
        if (!id.isNull()) references.push_back(keyFromUuid(id));
    };
    if (const HydraulicLinkPipe* pipe = std::get_if<HydraulicLinkPipe>(&entity)) {
        append(pipe->node_uuid_from);
        append(pipe->node_uuid_to);
        append(pipe->material_uuid);
    } else if (const HydraulicNodeJunction* node = std::get_if<HydraulicNodeJunction>(&entity)) {
        for (const HydraulicDemand& demand : node->demands) append(demand.pattern_uuid);
        append(node->quality_source.pattern_uuid);
    } else if (const HydraulicMapLabel* label = std::get_if<HydraulicMapLabel>(&entity)) {
        append(label->anchor_node_uuid);
    }
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    return references;
}

// The first eight collections are reference-free catalogue entries. The
// canonical hydraulic validator only registers their UUIDs and checks typed
// references *from other entities*. Their own fields never affect structural
// validation; incoming references are unchanged by an in-place update.
// Decode the old record before trusting this invariant on the indexed path.
bool validCatalogueRecord(const ModelRecord& record, const QUuid& uuid) {
    if (record.kind < 8 || record.kind > 15 || !record.references.empty()) return false;
    const QByteArray bytes = qByteArrayFromBytes(record.payload);
    switch (record.kind) {
#define CHECK_CATALOG(name, type, kind) \
        case kind: { \
            const std::optional<type> decoded = ModelCodec<type>::decode(bytes); \
            return decoded && decoded->uuid == uuid; \
        }
        CHECK_CATALOG(pipe_materials, HydraulicPipeMaterial, 8)
        CHECK_CATALOG(patterns_time, HydraulicPatternTime, 9)
        CHECK_CATALOG(curves_tank_volume, HydraulicCurveTankVolume, 10)
        CHECK_CATALOG(curves_pump_head, HydraulicCurvePumpHead, 11)
        CHECK_CATALOG(curves_pump_efficiency, HydraulicCurvePumpEfficiency, 12)
        CHECK_CATALOG(curves_valve_headloss, HydraulicCurveValveHeadloss, 13)
        CHECK_CATALOG(curves_valve_characteristic, HydraulicCurveValveCharacteristic, 14)
        CHECK_CATALOG(curves_generic, HydraulicCurveGeneric, 15)
#undef CHECK_CATALOG
        default: return false;
    }
}

// For existing reservoirs, tanks, pumps and valves, the canonical validator
// currently inspects UUID identity and typed outgoing references only. Permit
// an explicit set of physical/operating scalar fields to change without
// rebuilding the full network. Canonical byte comparison guarantees that any
// other field (including newly added Model fields) triggers full validation.
// In particular, link endpoints, curves, patterns and quality-source links
// must remain exactly as they were in the previously validated model.
bool physicalOnlyChange(const HydraulicEntity& proposed, const ModelRecord& old,
                        const QUuid& uuid) {
    if (const HydraulicNodeReservoir* changed = std::get_if<HydraulicNodeReservoir>(&proposed)) {
        if (old.kind != 2) return false;
        const std::optional<HydraulicNodeReservoir> prior = ModelCodec<HydraulicNodeReservoir>::decode(
            qByteArrayFromBytes(old.payload));
        if (!prior || prior->uuid != uuid) return false;
        HydraulicNodeReservoir expected = *prior;
        expected.hydraulic_head_m = changed->hydraulic_head_m;
        expected.terrain_elevation_m = changed->terrain_elevation_m;
        expected.hydraulic_head_offset_m = changed->hydraulic_head_offset_m;
        expected.initial_chemical_concentration_mg_per_l = changed->initial_chemical_concentration_mg_per_l;
        expected.initial_water_age_h = changed->initial_water_age_h;
        return ModelCodec<HydraulicNodeReservoir>::encode(expected) ==
               ModelCodec<HydraulicNodeReservoir>::encode(*changed);
    }
    if (const HydraulicNodeTank* changed = std::get_if<HydraulicNodeTank>(&proposed)) {
        if (old.kind != 3) return false;
        const std::optional<HydraulicNodeTank> prior = ModelCodec<HydraulicNodeTank>::decode(
            qByteArrayFromBytes(old.payload));
        if (!prior || prior->uuid != uuid) return false;
        HydraulicNodeTank expected = *prior;
        expected.bottom_elevation_m = changed->bottom_elevation_m;
        expected.terrain_elevation_m = changed->terrain_elevation_m;
        expected.bottom_offset_m = changed->bottom_offset_m;
        expected.water_level_initial_m = changed->water_level_initial_m;
        expected.water_level_minimum_m = changed->water_level_minimum_m;
        expected.water_level_maximum_m = changed->water_level_maximum_m;
        expected.diameter_m = changed->diameter_m;
        expected.cross_section_area_m2 = changed->cross_section_area_m2;
        expected.volume_at_maximum_level_m3 = changed->volume_at_maximum_level_m3;
        expected.minimum_volume_m3 = changed->minimum_volume_m3;
        expected.mixing_fraction = changed->mixing_fraction;
        return ModelCodec<HydraulicNodeTank>::encode(expected) ==
               ModelCodec<HydraulicNodeTank>::encode(*changed);
    }
    if (const HydraulicLinkPump* changed = std::get_if<HydraulicLinkPump>(&proposed)) {
        if (old.kind != 5) return false;
        const std::optional<HydraulicLinkPump> prior = ModelCodec<HydraulicLinkPump>::decode(
            qByteArrayFromBytes(old.payload));
        if (!prior || prior->uuid != uuid) return false;
        HydraulicLinkPump expected = *prior;
        expected.constant_power_kw = changed->constant_power_kw;
        expected.initial_speed_ratio = changed->initial_speed_ratio;
        expected.constant_efficiency_percent = changed->constant_efficiency_percent;
        expected.energy_price_per_kw_h = changed->energy_price_per_kw_h;
        return ModelCodec<HydraulicLinkPump>::encode(expected) ==
               ModelCodec<HydraulicLinkPump>::encode(*changed);
    }
    if (const HydraulicLinkValve* changed = std::get_if<HydraulicLinkValve>(&proposed)) {
        if (old.kind != 6) return false;
        const std::optional<HydraulicLinkValve> prior = ModelCodec<HydraulicLinkValve>::decode(
            qByteArrayFromBytes(old.payload));
        if (!prior || prior->uuid != uuid) return false;
        HydraulicLinkValve expected = *prior;
        expected.setting_pressure_head_m = changed->setting_pressure_head_m;
        expected.setting_flow_m3_per_h = changed->setting_flow_m3_per_h;
        expected.setting_loss_coefficient = changed->setting_loss_coefficient;
        expected.setting_position_percent = changed->setting_position_percent;
        expected.diameter_mm = changed->diameter_mm;
        expected.minor_loss_coefficient = changed->minor_loss_coefficient;
        return ModelCodec<HydraulicLinkValve>::encode(expected) ==
               ModelCodec<HydraulicLinkValve>::encode(*changed);
    }
    return false;
}

// Verify the stored outgoing-reference index against the canonical validator's
// extraction before reusing that index for a bounded physical edit.
std::vector<Key> physicalReferences(const HydraulicEntity& entity) {
    std::vector<Key> references;
    const auto append = [&references](const QUuid& id) {
        if (!id.isNull()) references.push_back(keyFromUuid(id));
    };
    if (const HydraulicNodeReservoir* item = std::get_if<HydraulicNodeReservoir>(&entity)) {
        append(item->head_pattern_uuid);
        append(item->quality_source.pattern_uuid);
    } else if (const HydraulicNodeTank* item = std::get_if<HydraulicNodeTank>(&entity)) {
        append(item->volume_curve_uuid);
        append(item->quality_source.pattern_uuid);
    } else if (const HydraulicLinkPump* item = std::get_if<HydraulicLinkPump>(&entity)) {
        append(item->node_uuid_from);
        append(item->node_uuid_to);
        append(item->head_curve_uuid);
        append(item->speed_pattern_uuid);
        append(item->efficiency_curve_uuid);
        append(item->price_pattern_uuid);
    } else if (const HydraulicLinkValve* item = std::get_if<HydraulicLinkValve>(&entity)) {
        append(item->node_uuid_from);
        append(item->node_uuid_to);
        append(item->head_loss_curve_uuid);
        append(item->characteristic_curve_uuid);
    }
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    return references;
}

// Mirror the canonical validator's outgoing references for demand points and
// hydraulic controls. Only fields inspected by NetworkReferenceValidator are
// indexed: unrelated UUID-valued metadata is not a hydraulic reference.
std::vector<Key> dependentReferences(const HydraulicEntity& entity) {
    std::vector<Key> references;
    const auto append = [&references](const QUuid& id) {
        if (!id.isNull()) references.push_back(keyFromUuid(id));
    };
    if (const HydraulicDemandPoint* point = std::get_if<HydraulicDemandPoint>(&entity)) {
        for (const HydraulicDemand& demand : point->demands) append(demand.pattern_uuid);
        if (point->attachment.type == HydraulicDemandPointAttachmentType::Pipe) {
            append(point->attachment.pipe_uuid);
            if (point->attachment.pipe_allocation_mode ==
                HydraulicDemandPointPipeAllocationMode::AssignedJunction)
                append(point->attachment.pipe_assigned_junction_uuid);
        } else if (point->attachment.type == HydraulicDemandPointAttachmentType::Junction) {
            append(point->attachment.junction_uuid);
        }
    } else if (const HydraulicControlSimple* control = std::get_if<HydraulicControlSimple>(&entity)) {
        append(control->link_uuid);
        append(control->trigger_node_uuid);
    } else if (const HydraulicControlRule* rule = std::get_if<HydraulicControlRule>(&entity)) {
        for (const HydraulicControlRulePremise& premise : rule->premises) {
            if (premise.object == HydraulicControlRuleObject::Node ||
                premise.object == HydraulicControlRuleObject::Link)
                append(premise.object_uuid);
        }
        for (const HydraulicControlRuleAction& action : rule->actions_then) append(action.link_uuid);
        for (const HydraulicControlRuleAction& action : rule->actions_else) append(action.link_uuid);
    }
    std::sort(references.begin(), references.end());
    references.erase(std::unique(references.begin(), references.end()), references.end());
    return references;
}

// Preserve the exact canonical validator reference sets across indexed edits.
std::vector<Key> membershipReferences(const HydraulicEntity& entity) {
    if (std::holds_alternative<HydraulicNodeReservoir>(entity) ||
        std::holds_alternative<HydraulicNodeTank>(entity) ||
        std::holds_alternative<HydraulicLinkPump>(entity) ||
        std::holds_alternative<HydraulicLinkValve>(entity)) return physicalReferences(entity);
    if (std::holds_alternative<HydraulicDemandPoint>(entity) ||
        std::holds_alternative<HydraulicControlSimple>(entity) ||
        std::holds_alternative<HydraulicControlRule>(entity)) return dependentReferences(entity);
    return scalarReferences(entity);
}

// Existing demand points and controls may change their references without
// touching membership. Verify the old canonical reference index before relying
// on it, and validate *all* proposed targets against the same typed rules used
// by NetworkReferenceValidator. Unsupported/mixed batches fall back to a full
// network validation rather than committing a partially validated subset.
std::optional<Revision> tryIndexedDependentUpdates(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    const auto validTarget = [&](const QUuid& id,
                                 std::initializer_list<std::uint16_t> accepted) -> bool {
        if (id.isNull()) return true; // Same policy as the canonical validator.
        const Key key = keyFromUuid(id);
        const std::optional<ModelRecord> target = repository.getModel(branch, key);
        if (!target || std::find(accepted.begin(), accepted.end(), target->kind) == accepted.end())
            return false;
        return ownerChunk(repository, branch, networkUuid, key).has_value();
    };
    const auto nodeTarget = [&](const QUuid& id) {
        return validTarget(id, {junctionKind, 2, 3});
    };
    for (const HydraulicEntityChange& change : changes) {
        if (change.uuid.isNull() || change.uuid == networkUuid || !change.value ||
            change.value->valueless_by_exception() ||
            (!std::holds_alternative<HydraulicDemandPoint>(*change.value) &&
             !std::holds_alternative<HydraulicControlSimple>(*change.value) &&
             !std::holds_alternative<HydraulicControlRule>(*change.value)) ||
            std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
            return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second || !ownerChunk(repository, branch, networkUuid, key))
            return std::nullopt;
        const std::optional<ModelRecord> old = repository.getModel(branch, key);
        if (!old) return std::nullopt;
        std::optional<HydraulicEntity> previous;
        const QByteArray oldBytes = qByteArrayFromBytes(old->payload);
        if (std::holds_alternative<HydraulicDemandPoint>(*change.value) && old->kind == 7) {
            const std::optional<HydraulicDemandPoint> decoded = ModelCodec<HydraulicDemandPoint>::decode(oldBytes);
            if (decoded && decoded->uuid == change.uuid) previous = HydraulicEntity{*decoded};
        } else if (std::holds_alternative<HydraulicControlSimple>(*change.value) && old->kind == 16) {
            const std::optional<HydraulicControlSimple> decoded = ModelCodec<HydraulicControlSimple>::decode(oldBytes);
            if (decoded && decoded->uuid == change.uuid) previous = HydraulicEntity{*decoded};
        } else if (std::holds_alternative<HydraulicControlRule>(*change.value) && old->kind == 17) {
            const std::optional<HydraulicControlRule> decoded = ModelCodec<HydraulicControlRule>::decode(oldBytes);
            if (decoded && decoded->uuid == change.uuid) previous = HydraulicEntity{*decoded};
        }
        if (!previous || old->references != dependentReferences(*previous)) return std::nullopt;

        if (const HydraulicDemandPoint* point = std::get_if<HydraulicDemandPoint>(&*change.value)) {
            for (const HydraulicDemand& demand : point->demands)
                if (!validTarget(demand.pattern_uuid, {9})) return std::nullopt;
            if (point->attachment.type == HydraulicDemandPointAttachmentType::Pipe) {
                const QUuid pipeUuid = point->attachment.pipe_uuid;
                if (!validTarget(pipeUuid, {pipeKind}) ||
                    !std::isfinite(point->attachment.pipe_position) ||
                    point->attachment.pipe_position < 0.0 || point->attachment.pipe_position > 1.0)
                    return std::nullopt;
                if (point->attachment.pipe_allocation_mode ==
                    HydraulicDemandPointPipeAllocationMode::AssignedJunction) {
                    const QUuid junctionUuid = point->attachment.pipe_assigned_junction_uuid;
                    if (!validTarget(junctionUuid, {junctionKind})) return std::nullopt;
                    if (!pipeUuid.isNull() && !junctionUuid.isNull()) {
                        const std::optional<ModelRecord> pipeRecord =
                            repository.getModel(branch, keyFromUuid(pipeUuid));
                        if (!pipeRecord || pipeRecord->kind != pipeKind) return std::nullopt;
                        const std::optional<HydraulicLinkPipe> pipe = ModelCodec<HydraulicLinkPipe>::decode(
                            qByteArrayFromBytes(pipeRecord->payload));
                        if (!pipe || pipe->uuid != pipeUuid ||
                            pipeRecord->references != scalarReferences(HydraulicEntity{*pipe}) ||
                            (pipe->node_uuid_from != junctionUuid && pipe->node_uuid_to != junctionUuid))
                            return std::nullopt;
                    }
                }
            } else if (point->attachment.type == HydraulicDemandPointAttachmentType::Junction) {
                if (!validTarget(point->attachment.junction_uuid, {junctionKind})) return std::nullopt;
            } else if (point->attachment.type != HydraulicDemandPointAttachmentType::None) {
                return std::nullopt;
            }
        } else if (const HydraulicControlSimple* control =
                   std::get_if<HydraulicControlSimple>(&*change.value)) {
            if (!validTarget(control->link_uuid, {pipeKind, 5, 6}) ||
                !nodeTarget(control->trigger_node_uuid)) return std::nullopt;
        } else if (const HydraulicControlRule* rule =
                   std::get_if<HydraulicControlRule>(&*change.value)) {
            for (const HydraulicControlRulePremise& premise : rule->premises) {
                if (premise.object == HydraulicControlRuleObject::Node &&
                    !nodeTarget(premise.object_uuid)) return std::nullopt;
                if (premise.object == HydraulicControlRuleObject::Link &&
                    !validTarget(premise.object_uuid, {pipeKind, 5, 6})) return std::nullopt;
            }
            for (const HydraulicControlRuleAction& action : rule->actions_then)
                if (!validTarget(action.link_uuid, {pipeKind, 5, 6})) return std::nullopt;
            for (const HydraulicControlRuleAction& action : rule->actions_else)
                if (!validTarget(action.link_uuid, {pipeKind, 5, 6})) return std::nullopt;
        }

        ModelRecord updated = *old;
        std::visit([&updated](const auto& entity) {
            using T = std::decay_t<decltype(entity)>;
            updated.payload = bytesFromQByteArray(ModelCodec<T>::encode(entity));
        }, *change.value);
        if (updated.payload.empty()) return std::nullopt;
        updated.references = dependentReferences(*change.value);
        if (updated.payload != old->payload || updated.references != old->references)
            pending.push_back(ModelChange{key, std::move(updated)});
    }
    Revision result;
    if (pending.empty()) {
        if (repository.head(branch).id != starting.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic dependent edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, std::move(pending), starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedDependentUpdateFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// All changes in a batch must satisfy the whitelist before publishing *any*
// changes. Existing membership chunks and network metadata remain untouched.
std::optional<Revision> tryIndexedPhysicalUpdates(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    for (const HydraulicEntityChange& change : changes) {
        if (change.uuid.isNull() || change.uuid == networkUuid || !change.value ||
            change.value->valueless_by_exception() ||
            std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
            return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second || !ownerChunk(repository, branch, networkUuid, key))
            return std::nullopt;
        const std::optional<ModelRecord> old = repository.getModel(branch, key);
        if (!old || !physicalOnlyChange(*change.value, *old, change.uuid) ||
            old->references != physicalReferences(*change.value)) return std::nullopt;
        ModelRecord updated = *old;
        std::visit([&updated](const auto& entity) {
            using T = std::decay_t<decltype(entity)>;
            updated.payload = bytesFromQByteArray(ModelCodec<T>::encode(entity));
        }, *change.value);
        if (updated.payload.empty()) return std::nullopt;
        if (updated.payload != old->payload)
            pending.push_back(ModelChange{key, std::move(updated)});
    }
    Revision result;
    if (pending.empty()) {
        if (repository.head(branch).id != starting.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic physical edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, std::move(pending), starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedPhysicalFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// Catalogue payload updates need no other model reads. A single modified
// catalogue cannot invalidate typed references to that same UUID and kind.
std::optional<Revision> tryIndexedCatalogueUpdates(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    for (const HydraulicEntityChange& change : changes) {
        if (change.uuid.isNull() || change.uuid == networkUuid || !change.value ||
            change.value->valueless_by_exception() || change.value->index() >= 8 ||
            std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
            return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second || !ownerChunk(repository, branch, networkUuid, key))
            return std::nullopt;
        const std::optional<ModelRecord> old = repository.getModel(branch, key);
        if (!old || old->kind != 8 + change.value->index() ||
            !validCatalogueRecord(*old, change.uuid)) return std::nullopt;
        ModelRecord updated = *old;
        std::visit([&updated](const auto& entity) {
            using T = std::decay_t<decltype(entity)>;
            updated.payload = bytesFromQByteArray(ModelCodec<T>::encode(entity));
        }, *change.value);
        if (updated.payload.empty()) return std::nullopt;
        if (updated.payload != old->payload)
            pending.push_back(ModelChange{key, std::move(updated)});
    }
    Revision result;
    if (pending.empty()) {
        if (repository.head(branch).id != starting.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic catalogue edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, std::move(pending), starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedCatalogueFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// A rewire changes the set of outgoing references and therefore cannot use the
// scalar-only path. Only allow the same restricted pipe hydraulic properties
// plus its two endpoints; any other field change requires canonical validation.
bool pipeEndpointOnlyChange(const HydraulicLinkPipe& proposed,
                            const HydraulicLinkPipe& prior) {
    HydraulicLinkPipe expected = prior;
    expected.node_uuid_from = proposed.node_uuid_from;
    expected.node_uuid_to = proposed.node_uuid_to;
    expected.diameter_mm = proposed.diameter_mm;
    expected.length_calculated_m = proposed.length_calculated_m;
    expected.length_measured_m = proposed.length_measured_m;
    expected.roughness_hazen_williams = proposed.roughness_hazen_williams;
    expected.roughness_darcy_weisbach_mm = proposed.roughness_darcy_weisbach_mm;
    expected.roughness_chezy_manning = proposed.roughness_chezy_manning;
    expected.minor_loss_coefficient = proposed.minor_loss_coefficient;
    return ModelCodec<HydraulicLinkPipe>::encode(expected) ==
           ModelCodec<HydraulicLinkPipe>::encode(proposed);
}

// Only independently rewired, already-owned pipes are eligible. In particular,
// references *to* the pipe (demand-point attachments etc.) may introduce
// cross-entity constraints on endpoint changes. If any such dependency exists,
// return to the complete canonical validator rather than guessing.
std::optional<Revision> tryIndexedPipeRewires(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    const Key networkKey = keyFromUuid(networkUuid);
    const std::optional<ModelRecord> networkRoot = repository.getModel(branch, networkKey);
    if (!networkRoot || networkRoot->kind != hydraulicNetworkKind) return std::nullopt;

    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    // A typed target must be an existing node owned by this same network.
    const auto validTarget = [&](const QUuid& target, bool material) -> bool {
        if (target.isNull()) return true; // The canonical validator permits null references.
        const Key key = keyFromUuid(target);
        const std::optional<ModelRecord> record = repository.getModel(branch, key);
        if (!record) return false;
        if (material) {
            if (record->kind != 8) return false;
        } else if (record->kind != junctionKind && record->kind != 2 && record->kind != 3) {
            return false;
        }
        return ownerChunk(repository, branch, networkUuid, key).has_value();
    };

    for (const HydraulicEntityChange& change : changes) {
        if (!change.value || change.value->valueless_by_exception() ||
            change.uuid.isNull() || change.uuid == networkUuid) return std::nullopt;
        const HydraulicLinkPipe* proposed = std::get_if<HydraulicLinkPipe>(&*change.value);
        if (!proposed || proposed->uuid != change.uuid) return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second) return std::nullopt;
        const std::optional<Key> owning = ownerChunk(repository, branch, networkUuid, key);
        if (!owning) return std::nullopt;
        const std::vector<Key> incoming = repository.referencingModel(branch, key);
        if (incoming.size() != 1 || incoming.front() != *owning) return std::nullopt;
        const std::optional<ModelRecord> old = repository.getModel(branch, key);
        if (!old || old->kind != pipeKind) return std::nullopt;
        QString error;
        const std::optional<HydraulicLinkPipe> prior = ModelCodec<HydraulicLinkPipe>::decode(
            qByteArrayFromBytes(old->payload), &error);
        if (!prior || prior->uuid != change.uuid ||
            !pipeEndpointOnlyChange(*proposed, *prior)) return std::nullopt;
        if (old->references != scalarReferences(HydraulicEntity{*prior})) return std::nullopt;
        if (!validTarget(proposed->node_uuid_from, false) ||
            !validTarget(proposed->node_uuid_to, false) ||
            !validTarget(proposed->material_uuid, true)) return std::nullopt;

        ModelRecord updated = *old;
        updated.payload = bytesFromQByteArray(ModelCodec<HydraulicLinkPipe>::encode(*proposed));
        if (updated.payload.empty()) return std::nullopt;
        updated.references = scalarReferences(*change.value);
        if (updated.payload != old->payload) {
            pending.push_back(ModelChange{key, std::move(updated)});
        }
    }

    Revision result;
    if (pending.empty()) {
        if (repository.head(branch).id != starting.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic topology edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, std::move(pending), starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedTopologyFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// A carefully limited indexed membership path. The canonical validator's
// relevant checks for supported new entities are UUID uniqueness and typed
// outgoing references. Existing members of those kinds may be removed if
// every object that refers to them is removed in the SAME batch.
// References in the network's embedded report and multi-species options are
// checked separately: they do not appear in the model reverse-reference index.
//
// Bounded membership chunks allow edits without scanning existing members.
std::optional<Revision> tryIndexedMembershipEdits(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    const Key networkKey = keyFromUuid(networkUuid);
    const std::optional<ModelRecord> networkRoot = repository.getModel(branch, networkKey);
    if (!networkRoot || networkRoot->kind != hydraulicNetworkKind) return std::nullopt;
    Manifest manifest = decodeManifest(qByteArrayFromBytes(networkRoot->payload));
    if (manifest.metadata.uuid != networkUuid) return std::nullopt;
    checkRoot(manifest, *networkRoot);

    static constexpr std::array<std::uint16_t, collectionCount> kinds = {
#define MEMBER_KIND(name, type, kind) static_cast<std::uint16_t>(kind),
        AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(MEMBER_KIND)
#undef MEMBER_KIND
    };
    std::set<Key> seen;
    std::map<Key, const HydraulicEntityChange*> insertions;
    struct Removal {
        Key owner;
        std::size_t collection;
    };
    std::map<Key, Removal> removals;
    for (const HydraulicEntityChange& change : changes) {
        if (change.uuid.isNull() || change.uuid == networkUuid ||
            !seen.insert(keyFromUuid(change.uuid)).second) return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (change.value) {
            if (change.value->valueless_by_exception() ||
                std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
                return std::nullopt;
            if (change.value->index() >= 8 &&
                !std::holds_alternative<HydraulicNodeReservoir>(*change.value) &&
                !std::holds_alternative<HydraulicNodeJunction>(*change.value) &&
                !std::holds_alternative<HydraulicNodeTank>(*change.value) &&
                !std::holds_alternative<HydraulicLinkPipe>(*change.value) &&
                !std::holds_alternative<HydraulicLinkPump>(*change.value) &&
                !std::holds_alternative<HydraulicLinkValve>(*change.value) &&
                !std::holds_alternative<HydraulicDemandPoint>(*change.value) &&
                !std::holds_alternative<HydraulicControlSimple>(*change.value) &&
                !std::holds_alternative<HydraulicControlRule>(*change.value) &&
                !std::holds_alternative<HydraulicMapLabel>(*change.value)) return std::nullopt;
            if (repository.getModel(branch, key)) return std::nullopt;
            insertions.emplace(key, &change);
        } else {
            const std::optional<ModelRecord> old = repository.getModel(branch, key);
            if (!old) return std::nullopt;
            std::size_t collection = 0;
            std::optional<HydraulicEntity> decoded;
            QString error;
            if (old->kind >= 8 && old->kind <= 15) {
                collection = old->kind - 8;
                if (!validCatalogueRecord(*old, change.uuid)) return std::nullopt;
            } else if (old->kind == 2 || old->kind == 3 ||
                       old->kind == 5 || old->kind == 6) {
                // Decode the previous record before trusting its outgoing
                // reference index. A corrupt index must never authorize a
                // deletion based on incomplete incoming dependencies.
                switch (old->kind) {
#define DECODE_PHYSICAL(type, expectedKind, ordinal) \
                    case expectedKind: { \
                        const std::optional<type> value = ModelCodec<type>::decode( \
                            qByteArrayFromBytes(old->payload), &error); \
                        if (!value || value->uuid != change.uuid) return std::nullopt; \
                        collection = static_cast<std::size_t>(ordinal); \
                        decoded = HydraulicEntity{*value}; \
                        break; \
                    }
                    DECODE_PHYSICAL(HydraulicNodeReservoir, 2, HydraulicEntityType::Reservoir)
                    DECODE_PHYSICAL(HydraulicNodeTank, 3, HydraulicEntityType::Tank)
                    DECODE_PHYSICAL(HydraulicLinkPump, 5, HydraulicEntityType::Pump)
                    DECODE_PHYSICAL(HydraulicLinkValve, 6, HydraulicEntityType::Valve)
#undef DECODE_PHYSICAL
                    default: return std::nullopt;
                }
            } else if (old->kind == junctionKind) {
                collection = static_cast<std::size_t>(HydraulicEntityType::Junction);
                const std::optional<HydraulicNodeJunction> value =
                    ModelCodec<HydraulicNodeJunction>::decode(qByteArrayFromBytes(old->payload), &error);
                if (!value || value->uuid != change.uuid) return std::nullopt;
                decoded = HydraulicEntity{*value};
            } else if (old->kind == pipeKind) {
                collection = static_cast<std::size_t>(HydraulicEntityType::Pipe);
                const std::optional<HydraulicLinkPipe> value =
                    ModelCodec<HydraulicLinkPipe>::decode(qByteArrayFromBytes(old->payload), &error);
                if (!value || value->uuid != change.uuid) return std::nullopt;
                decoded = HydraulicEntity{*value};
            } else if (old->kind == 7 || old->kind == 16 || old->kind == 17 || old->kind == 18) {
                switch (old->kind) {
#define DECODE_DEPENDENT(type, expectedKind, ordinal) \
                    case expectedKind: { \
                        const std::optional<type> value = ModelCodec<type>::decode( \
                            qByteArrayFromBytes(old->payload), &error); \
                        if (!value || value->uuid != change.uuid) return std::nullopt; \
                        collection = static_cast<std::size_t>(ordinal); \
                        decoded = HydraulicEntity{*value}; \
                        break; \
                    }
                    DECODE_DEPENDENT(HydraulicDemandPoint, 7, HydraulicEntityType::DemandPoint)
                    DECODE_DEPENDENT(HydraulicControlSimple, 16, HydraulicEntityType::SimpleControl)
                    DECODE_DEPENDENT(HydraulicControlRule, 17, HydraulicEntityType::RuleControl)
                    DECODE_DEPENDENT(HydraulicMapLabel, 18, HydraulicEntityType::MapLabel)
#undef DECODE_DEPENDENT
                    default: return std::nullopt;
                }
            } else return std::nullopt;
            const std::optional<Key> owning = ownerChunk(repository, branch, networkUuid, key);
            if (!owning || !manifest.chunks[collection].contains(uuidFromKey(*owning)) ||
                (decoded && old->references != membershipReferences(*decoded))) return std::nullopt;
            removals.emplace(key, Removal{*owning, collection});
        }
    }

    // Metadata references do not use persisted outgoing entity references.
    // Extract them with the SAME canonical reference walker as full validation;
    // this includes report selections and embedded multi-species relationships.
    QSet<QUuid> metadataReferences;
    if (!removals.empty()) {
        for (const aowis::dvmdb::HydraulicReference& reference :
             aowis::dvmdb::hydraulicReferences(manifest.metadata)) {
            if (reference.source_uuid == networkUuid && !reference.target_uuid.isNull())
                metadataReferences.insert(reference.target_uuid);
        }
    }
    for (const std::pair<const Key, Removal>& removal : removals) {
        if (metadataReferences.contains(uuidFromKey(removal.first))) return std::nullopt;
        const std::vector<Key> incoming = repository.referencingModel(branch, removal.first);
        if (std::find(incoming.begin(), incoming.end(), removal.second.owner) == incoming.end())
            return std::nullopt;
        for (const Key& referring : incoming) {
            if (referring != removal.second.owner && !removals.contains(referring))
                return std::nullopt;
        }
    }

    // Every inserted reference is validated against the exact accepted kinds,
    // regardless of whether the target already exists or is another insertion
    // in this same atomic batch. A reference into this batch's removals is
    // always illegal. Null references are allowed by the canonical validator.
    const auto validTarget = [&](const QUuid& id,
                                 std::initializer_list<std::uint16_t> accepted) -> bool {
        if (id.isNull()) return true;
        const Key key = keyFromUuid(id);
        if (removals.contains(key)) return false;
        const std::map<Key, const HydraulicEntityChange*>::const_iterator added = insertions.find(key);
        if (added != insertions.end()) {
            const std::size_t index = added->second->value->index();
            return std::find(accepted.begin(), accepted.end(), kinds[index]) != accepted.end();
        }
        const std::optional<ModelRecord> target = repository.getModel(branch, key);
        if (!target || std::find(accepted.begin(), accepted.end(), target->kind) == accepted.end())
            return false;
        return ownerChunk(repository, branch, networkUuid, key).has_value();
    };
    const auto nodeTarget = [&](const QUuid& id) {
        return validTarget(id, {junctionKind, 2, 3});
    };
    const auto patternTarget = [&](const QUuid& id) {
        return validTarget(id, {9});
    };
    for (const std::pair<const Key, const HydraulicEntityChange*>& entry : insertions) {
        const HydraulicEntityChange& change = *entry.second;
        if (const HydraulicNodeJunction* node = std::get_if<HydraulicNodeJunction>(&*change.value)) {
            for (const HydraulicDemand& demand : node->demands)
                if (!patternTarget(demand.pattern_uuid)) return std::nullopt;
            if (!patternTarget(node->quality_source.pattern_uuid)) return std::nullopt;
        } else if (const HydraulicNodeReservoir* reservoir =
                   std::get_if<HydraulicNodeReservoir>(&*change.value)) {
            if (!patternTarget(reservoir->head_pattern_uuid) ||
                !patternTarget(reservoir->quality_source.pattern_uuid)) return std::nullopt;
        } else if (const HydraulicNodeTank* tank = std::get_if<HydraulicNodeTank>(&*change.value)) {
            if (!validTarget(tank->volume_curve_uuid, {10}) ||
                !patternTarget(tank->quality_source.pattern_uuid)) return std::nullopt;
        } else if (const HydraulicLinkPipe* pipe = std::get_if<HydraulicLinkPipe>(&*change.value)) {
            if (!nodeTarget(pipe->node_uuid_from) || !nodeTarget(pipe->node_uuid_to) ||
                !validTarget(pipe->material_uuid, {8})) return std::nullopt;
        } else if (const HydraulicLinkPump* pump = std::get_if<HydraulicLinkPump>(&*change.value)) {
            if (!nodeTarget(pump->node_uuid_from) || !nodeTarget(pump->node_uuid_to) ||
                !validTarget(pump->head_curve_uuid, {11}) ||
                !patternTarget(pump->speed_pattern_uuid) ||
                !validTarget(pump->efficiency_curve_uuid, {12}) ||
                !patternTarget(pump->price_pattern_uuid)) return std::nullopt;
        } else if (const HydraulicLinkValve* valve = std::get_if<HydraulicLinkValve>(&*change.value)) {
            if (!nodeTarget(valve->node_uuid_from) || !nodeTarget(valve->node_uuid_to) ||
                !validTarget(valve->head_loss_curve_uuid, {13}) ||
                !validTarget(valve->characteristic_curve_uuid, {14})) return std::nullopt;
        } else if (const HydraulicDemandPoint* point = std::get_if<HydraulicDemandPoint>(&*change.value)) {
            for (const HydraulicDemand& demand : point->demands)
                if (!patternTarget(demand.pattern_uuid)) return std::nullopt;
            if (point->attachment.type == HydraulicDemandPointAttachmentType::Pipe) {
                if (!validTarget(point->attachment.pipe_uuid, {pipeKind}) ||
                    !std::isfinite(point->attachment.pipe_position) ||
                    point->attachment.pipe_position < 0.0 || point->attachment.pipe_position > 1.0)
                    return std::nullopt;
                if (point->attachment.pipe_allocation_mode ==
                    HydraulicDemandPointPipeAllocationMode::AssignedJunction) {
                    const QUuid assigned = point->attachment.pipe_assigned_junction_uuid;
                    if (!validTarget(assigned, {junctionKind})) return std::nullopt;
                    // The canonical validator also requires an assigned
                    // junction to be a real endpoint of the attached pipe.
                    // Check both existing and same-batch inserted pipes.
                    if (!point->attachment.pipe_uuid.isNull() && !assigned.isNull()) {
                        const Key pipeKey = keyFromUuid(point->attachment.pipe_uuid);
                        const std::map<Key, const HydraulicEntityChange*>::const_iterator inserted =
                            insertions.find(pipeKey);
                        std::optional<HydraulicLinkPipe> existingPipe;
                        const HydraulicLinkPipe* pipe = nullptr;
                        if (inserted != insertions.end()) {
                            pipe = std::get_if<HydraulicLinkPipe>(&*inserted->second->value);
                        } else {
                            const std::optional<ModelRecord> record = repository.getModel(branch, pipeKey);
                            if (!record || record->kind != pipeKind) return std::nullopt;
                            existingPipe = ModelCodec<HydraulicLinkPipe>::decode(
                                qByteArrayFromBytes(record->payload));
                            if (!existingPipe || existingPipe->uuid != point->attachment.pipe_uuid ||
                                record->references != scalarReferences(HydraulicEntity{*existingPipe}))
                                return std::nullopt;
                            pipe = &*existingPipe;
                        }
                        if (!pipe || (pipe->node_uuid_from != assigned &&
                                      pipe->node_uuid_to != assigned)) return std::nullopt;
                    }
                }
            } else if (point->attachment.type == HydraulicDemandPointAttachmentType::Junction) {
                if (!validTarget(point->attachment.junction_uuid, {junctionKind})) return std::nullopt;
            } else if (point->attachment.type != HydraulicDemandPointAttachmentType::None) {
                return std::nullopt;
            }
        } else if (const HydraulicControlSimple* control =
                   std::get_if<HydraulicControlSimple>(&*change.value)) {
            if (!validTarget(control->link_uuid, {pipeKind, 5, 6}) ||
                !nodeTarget(control->trigger_node_uuid)) return std::nullopt;
        } else if (const HydraulicControlRule* rule =
                   std::get_if<HydraulicControlRule>(&*change.value)) {
            for (const HydraulicControlRulePremise& premise : rule->premises) {
                if (premise.object == HydraulicControlRuleObject::Node &&
                    !nodeTarget(premise.object_uuid)) return std::nullopt;
                if (premise.object == HydraulicControlRuleObject::Link &&
                    !validTarget(premise.object_uuid, {pipeKind, 5, 6})) return std::nullopt;
            }
            for (const HydraulicControlRuleAction& action : rule->actions_then)
                if (!validTarget(action.link_uuid, {pipeKind, 5, 6})) return std::nullopt;
            for (const HydraulicControlRuleAction& action : rule->actions_else)
                if (!validTarget(action.link_uuid, {pipeKind, 5, 6})) return std::nullopt;
        } else if (const HydraulicMapLabel* label = std::get_if<HydraulicMapLabel>(&*change.value)) {
            if (!nodeTarget(label->anchor_node_uuid)) return std::nullopt;
        } else if (change.value->index() >= 8) return std::nullopt;
        // Catalogue entries have no outgoing refs.
    }

    std::map<Key, std::optional<MemberChunk>> modifiedChunks;
    const auto mutableChunk = [&](const Key& key, std::size_t collection) -> MemberChunk& {
        std::map<Key, std::optional<MemberChunk>>::iterator found = modifiedChunks.find(key);
        if (found == modifiedChunks.end()) {
            const std::optional<ModelRecord> record = repository.getModel(branch, key);
            if (!record) throw StorageError(StorageErrorKind::Corruption, "missing chunk while editing membership");
            found = modifiedChunks.emplace(key, decodeChunk(*record, networkUuid, collection)).first;
        }
        if (!found->second) throw StorageError(StorageErrorKind::Internal, "membership chunk removed twice");
        return *found->second;
    };

    std::vector<ModelChange> pending;
    pending.reserve(changes.size() + 24);
    for (const HydraulicEntityChange& change : changes) {
        const Key key = keyFromUuid(change.uuid);
        if (change.value) {
            const std::size_t collection = change.value->index();
            QList<QUuid>& ids = manifest.chunks[collection];
            if (ids.isEmpty() || mutableChunk(keyFromUuid(ids.last()), collection).members.size() >= chunkCapacity) {
                if (manifest.nextChunk[collection] == std::numeric_limits<quint32>::max())
                    throw StorageError(StorageErrorKind::LimitExceeded, "hydraulic chunk serial exhausted");
                const QUuid id = chunkUuid(networkUuid, collection, manifest.nextChunk[collection]++);
                if (repository.getModel(branch, keyFromUuid(id)) || modifiedChunks.contains(keyFromUuid(id)))
                    throw StorageError(StorageErrorKind::Internal, "hydraulic membership chunk ID collision");
                ids.append(id);
                modifiedChunks.emplace(keyFromUuid(id), MemberChunk{networkUuid, collection, {}});
            }
            mutableChunk(keyFromUuid(ids.last()), collection).members.append(change.uuid);
            ModelRecord record;
            record.kind = kinds[collection];
            std::visit([&record](const auto& entity) {
                using T = std::decay_t<decltype(entity)>;
                record.payload = bytesFromQByteArray(ModelCodec<T>::encode(entity));
            }, *change.value);
            if (record.payload.empty()) throw StorageError(StorageErrorKind::Internal, "cannot encode inserted entity");
            record.references = membershipReferences(*change.value);
            pending.push_back(ModelChange{key, std::move(record)});
        } else {
            const Removal& removal = removals.at(key);
            const std::size_t collection = removal.collection;
            const Key owner = removal.owner;
            MemberChunk& chunk = mutableChunk(owner, collection);
            if (chunk.members.removeAll(change.uuid) != 1)
                throw StorageError(StorageErrorKind::Corruption, "member missing from declared membership chunk");
            if (chunk.members.isEmpty()) {
                modifiedChunks[owner] = std::nullopt;
                if (!manifest.chunks[collection].removeOne(uuidFromKey(owner)))
                    throw StorageError(StorageErrorKind::Corruption, "membership chunk not declared by manifest");
            }
            pending.push_back(ModelChange{key, std::nullopt});
        }
    }
    for (const std::pair<const Key, std::optional<MemberChunk>>& entry : modifiedChunks) {
        if (entry.second) pending.push_back(ModelChange{entry.first, encodeChunk(*entry.second)});
        else pending.push_back(ModelChange{entry.first, std::nullopt});
    }
    const Bytes encodedManifest = bytesFromQByteArray(encodeManifest(manifest));
    if (networkRoot->payload != encodedManifest || networkRoot->references != chunkKeys(manifest)) {
        ModelRecord root = *networkRoot;
        root.payload = encodedManifest;
        root.references = chunkKeys(manifest);
        pending.push_back(ModelChange{networkKey, std::move(root)});
    }
    const Revision result = repository.commitModel(branch, std::move(pending), starting.id);
    if (diagnostics) {
        diagnostics->indexedMembershipFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// Return nullopt if any change needs full-network validation. This routine
// never publishes a partially validated subset: it first checks the ENTIRE
// batch, then commits it under the same expected-head guard as the slow path.
std::optional<Revision> tryIndexedScalarEdits(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const std::vector<HydraulicEntityChange>& changes, const Revision& starting,
    HydraulicEditDiagnostics* diagnostics) {
    // Network membership was indexed by a previously validated manifest.
    // Do not decode that flat O(N) manifest here: indexed incoming ownership
    // is sufficient for scalar-only edits of already validated records.
    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    for (const HydraulicEntityChange& change : changes) {
        if (!change.value || change.value->valueless_by_exception() || change.uuid.isNull() ||
            change.uuid == networkUuid) return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second) return std::nullopt;
        if (std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
            return std::nullopt;
        if (!ownerChunk(repository, branch, networkUuid, key)) return std::nullopt;
        const std::optional<ModelRecord> old = repository.getModel(branch, key);
        if (!old || !scalarOnlyChange(*change.value, *old, change.uuid)) return std::nullopt;
        if (old->references != scalarReferences(*change.value)) return std::nullopt;
        ModelRecord updated = *old;
        std::visit([&updated](const auto& entity) {
            using T = std::decay_t<decltype(entity)>;
            updated.payload = bytesFromQByteArray(ModelCodec<T>::encode(entity));
        }, *change.value);
        if (updated.payload.empty()) return std::nullopt;
        if (updated.payload != old->payload) {
            pending.push_back(ModelChange{key, std::move(updated)});
        }
    }
    Revision result;
    if (pending.empty()) {
        if (repository.head(branch).id != starting.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic scalar edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, std::move(pending), starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedScalarFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// UUIDv7 indexed scalar-edit counterpart. All ownership and payload reads are
// pinned to one immutable project revision; commitModelV2() atomically checks
// the same expected HEAD before publishing. An ineligible batch returns
// nullopt WITHOUT applying any of its changes, so the caller can use the
// complete-network validator instead.
std::optional<RevisionId> tryIndexedUuidScalarEdits(
    Repository& repository, const ProjectId& project, std::string_view branch,
    const QUuid& networkUuid, const std::vector<HydraulicEntityChange>& changes,
    const RevisionId& expectedHead, HydraulicEditDiagnostics* diagnostics) {
    std::set<Key> seen;
    std::vector<ModelChange> pending;
    pending.reserve(changes.size());
    for (const HydraulicEntityChange& change : changes) {
        if (!change.value || change.value->valueless_by_exception() ||
            change.uuid.isNull() || change.uuid == networkUuid) return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (!seen.insert(key).second) return std::nullopt;
        // Only the three scalar-whitelisted entity types are eligible.
        // Use concrete types, never an unchecked variant alternative.
        const HydraulicLinkPipe* pipe = std::get_if<HydraulicLinkPipe>(&*change.value);
        const HydraulicNodeJunction* junction =
            std::get_if<HydraulicNodeJunction>(&*change.value);
        const HydraulicMapLabel* label = std::get_if<HydraulicMapLabel>(&*change.value);
        if ((!pipe || pipe->uuid != change.uuid) &&
            (!junction || junction->uuid != change.uuid) &&
            (!label || label->uuid != change.uuid)) return std::nullopt;
        if (!ownerChunkAtRevisionV2(repository, project, expectedHead, networkUuid, key))
            return std::nullopt;
        const std::optional<ModelRecord> old =
            repository.getModelAtRevisionV2(project, expectedHead, key);
        if (!old || !scalarOnlyChange(*change.value, *old, change.uuid))
            return std::nullopt;
        if (old->references != scalarReferences(*change.value)) return std::nullopt;
        aowis::dvmdb::ModelValidationReport values;
        ModelRecord updated = *old;
        if (pipe) {
            values = aowis::dvmdb::validateHydraulicEntityValues(*pipe);
            updated.payload = bytesFromQByteArray(ModelCodec<HydraulicLinkPipe>::encode(*pipe));
        } else if (junction) {
            values = aowis::dvmdb::validateHydraulicEntityValues(*junction);
            updated.payload = bytesFromQByteArray(ModelCodec<HydraulicNodeJunction>::encode(*junction));
        } else {
            values = aowis::dvmdb::validateHydraulicEntityValues(*label);
            updated.payload = bytesFromQByteArray(ModelCodec<HydraulicMapLabel>::encode(*label));
        }
        if (!values.valid())
            throw StorageError(StorageErrorKind::InvalidInput,
                               "invalid hydraulic scalar edit: " + values.summary().toStdString());
        if (updated.payload.empty()) return std::nullopt;
        if (updated.payload != old->payload)
            pending.push_back(ModelChange{key, std::move(updated)});
    }
    RevisionId result = expectedHead;
    if (pending.empty()) {
        if (repository.headV2(project, branch) != expectedHead)
            throw StorageError(StorageErrorKind::Conflict,
                               "branch head changed during UUIDv7 hydraulic scalar edit");
    } else {
        result = repository.commitModelV2(project, branch, std::move(pending), expectedHead);
    }
    if (diagnostics) {
        diagnostics->indexedScalarFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

} // namespace

ModelChange emptyDraftHydraulicNetworkRoot(const QUuid& networkId, const QString& name) {
    if (networkId.isNull() || name.trimmed().isEmpty() || name.size() > 255)
        throw StorageError(StorageErrorKind::InvalidInput, "invalid new hydraulic network identity or name");
    NetworkHydraulic network;
    network.uuid = networkId;
    network.id = name;
    Manifest manifest = manifestFor(network);
    ModelRecord root;
    root.kind = hydraulicNetworkKind;
    root.payload = bytesFromQByteArray(encodeManifest(manifest));
    root.references = chunkKeys(manifest);
    return ModelChange{keyFromUuid(networkId), std::move(root)};
}

std::optional<NetworkHydraulic> loadDraftHydraulicNetwork(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const QUuid& networkId) {
    if (networkId.isNull())
        throw StorageError(StorageErrorKind::InvalidInput, "hydraulic network UUID is null");
    const std::optional<ModelRecord> root = readCurrent(keyFromUuid(networkId));
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
    if (manifest.metadata.uuid != networkId)
        throw StorageError(StorageErrorKind::Corruption, "draft network root UUID mismatch");
    checkRoot(manifest, *root);
    loadMembersUsing(readCurrent, manifest);
    const std::set<Key> members = memberKeys(manifest);
    NetworkHydraulic network = std::move(manifest.metadata);
    DependencyIndex storedReferences;
    std::size_t ordinal = 0;
#define RESTORE_DRAFT(name, type, entityKind) \
    for (const QUuid& uuid : manifest.members[ordinal]) { \
        const std::optional<ModelRecord> record = readCurrent(keyFromUuid(uuid)); \
        if (!record || record->kind != static_cast<std::uint16_t>(entityKind)) \
            throw StorageError(StorageErrorKind::Corruption, "missing or wrong-type draft hydraulic entity"); \
        QString error; \
        const std::optional<type> entity = ModelCodec<type>::decode(qByteArrayFromBytes(record->payload), &error); \
        if (!entity || entity->uuid != uuid || \
            ModelCodec<type>::encode(*entity) != qByteArrayFromBytes(record->payload)) \
            throw StorageError(StorageErrorKind::Corruption, "invalid draft hydraulic entity: " + error.toStdString()); \
        for (const Key& target : record->references) \
            storedReferences.emplace_back(keyFromUuid(uuid), target); \
        network.name.append(*entity); \
    } \
    ++ordinal;
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(RESTORE_DRAFT)
#undef RESTORE_DRAFT
    const aowis::dvmdb::HydraulicDependencyValidation validated =
        aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
    if (!validated.report.valid())
        throw StorageError(StorageErrorKind::Corruption,
                           "draft hydraulic network invalid: " + validated.report.summary().toStdString());
    const DependencyIndex canonicalReferences = indexedDependencies(validated.dependencies, members);
    std::sort(storedReferences.begin(), storedReferences.end());
    storedReferences.erase(std::unique(storedReferences.begin(), storedReferences.end()),
                           storedReferences.end());
    if (storedReferences != canonicalReferences)
        throw StorageError(StorageErrorKind::Corruption,
                           "draft hydraulic dependencies disagree with canonical model");
    return network;
}

void materializeDraftMembership(
    const std::function<std::optional<ModelRecord>(const Key&)>& readCurrent,
    const std::vector<DraftMembershipIntent>& intents,
    std::vector<ModelChange>& changes) {
    if (intents.empty()) return;
    if (intents.size() > 128)
        throw StorageError(StorageErrorKind::InvalidInput, "too many hydraulic membership intents");
    constexpr std::array<std::uint16_t, collectionCount> kinds = {
        8, 9, 10, 11, 12, 13, 14, 15, 2, junctionKind, 3, 7,
        pipeKind, 5, 6, 16, 17, 18
    };
    const std::function<void(std::uint16_t, const Key&, const ModelRecord&)> validatePayload =
        [](std::uint16_t expectedKind, const Key& entityId, const ModelRecord& record) {
        const QUuid uuid = uuidFromKey(entityId);
        const QByteArray payload = qByteArrayFromBytes(record.payload);
#define CHECK_CANONICAL(name, type, typeKind) \
        if (expectedKind == typeKind) { \
            QString error; \
            const std::optional<type> decoded = ModelCodec<type>::decode(payload, &error); \
            if (!decoded || decoded->uuid != uuid || \
                ModelCodec<type>::encode(*decoded) != payload) \
                throw StorageError(StorageErrorKind::InvalidInput, \
                    "invalid canonical hydraulic entity payload"); \
            return; \
        }
        AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(CHECK_CANONICAL)
#undef CHECK_CANONICAL
        throw StorageError(StorageErrorKind::InvalidInput, "unsupported hydraulic entity kind");
    };
    std::map<Key, std::optional<ModelRecord>> incoming;
    for (const ModelChange& change : changes)
        if (!incoming.emplace(change.key, change.record).second)
            throw StorageError(StorageErrorKind::InvalidInput, "duplicate draft entity change");
    std::map<Key, std::vector<DraftMembershipIntent>> grouped;
    std::set<Key> seenMembers;
    for (const DraftMembershipIntent& intent : intents) {
        if (intent.collection >= collectionCount ||
            kinds[intent.collection] != intent.kind ||
            intent.network == intent.member ||
            !seenMembers.insert(intent.member).second)
            throw StorageError(StorageErrorKind::InvalidInput,
                               "invalid or duplicate hydraulic membership intent");
        const std::map<Key, std::optional<ModelRecord>>::const_iterator proposed =
            incoming.find(intent.member);
        if (proposed == incoming.end() || proposed->second.has_value() != intent.inserted ||
            (intent.inserted && proposed->second->kind != intent.kind))
            throw StorageError(StorageErrorKind::InvalidInput,
                               "membership intent disagrees with candidate entity change");
        if (intent.inserted)
            validatePayload(intent.kind, intent.member, *proposed->second);
        const std::optional<ModelRecord> previous = readCurrent(intent.member);
        if (intent.inserted ? previous.has_value()
                            : !previous.has_value() || previous->kind != intent.kind)
            throw StorageError(StorageErrorKind::Conflict,
                               "hydraulic membership conflicts with staged entity state");
        grouped[intent.network].push_back(intent);
    }
    // Never accept user-supplied native manifest/chunk writes in the same batch.
    for (const ModelChange& change : changes)
        if (change.record && (change.record->kind == hydraulicNetworkKind ||
                              change.record->kind == chunkKind))
            throw StorageError(StorageErrorKind::InvalidInput,
                               "internal hydraulic records are server-owned");

    std::vector<ModelChange> nativeChanges;
    for (const std::pair<const Key, std::vector<DraftMembershipIntent>>& group : grouped) {
        const QUuid networkId = uuidFromKey(group.first);
        if (networkId.isNull())
            throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic network UUID");
        if (incoming.contains(group.first))
            throw StorageError(StorageErrorKind::InvalidInput,
                               "network metadata and membership cannot change together");
        const std::optional<ModelRecord> root = readCurrent(group.first);
        if (!root || root->kind != hydraulicNetworkKind)
            throw StorageError(StorageErrorKind::Conflict,
                               "hydraulic membership requires an existing network root");
        Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
        if (manifest.metadata.uuid != networkId)
            throw StorageError(StorageErrorKind::Corruption, "wrong hydraulic network root identity");
        checkRoot(manifest, *root);
        // Read only the affected collections. Unchanged hydraulic lists and
        // their chunks are never decoded or rewritten for this draft Save.
        std::set<std::size_t> touchedCollections;
        for (const DraftMembershipIntent& intent : group.second)
            touchedCollections.insert(intent.collection);
        for (const std::size_t collection : touchedCollections)
            loadMembersUsing(readCurrent, manifest, collection);
        std::set<Key> allMembers = memberKeys(manifest);
        std::map<Key, std::optional<ModelRecord>> changedChunks;
        bool rootChanged = false;
        for (const DraftMembershipIntent& intent : group.second) {
            const std::size_t collection = intent.collection;
            const QUuid memberId = uuidFromKey(intent.member);
            if (memberId.isNull())
                throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic member UUID");
            QList<QUuid>& chunkIds = manifest.chunks[collection];
            if (intent.inserted) {
                if (!allMembers.insert(intent.member).second)
                    throw StorageError(StorageErrorKind::Conflict,
                                       "hydraulic entity already belongs to network");
                manifest.members[collection].append(memberId);
                bool appended = false;
                if (!chunkIds.isEmpty()) {
                    const Key lastKey = keyFromUuid(chunkIds.last());
                    const std::map<Key, std::optional<ModelRecord>>::const_iterator staged =
                        changedChunks.find(lastKey);
                    const std::optional<ModelRecord> last = staged == changedChunks.end()
                        ? readCurrent(lastKey) : staged->second;
                    if (!last)
                        throw StorageError(StorageErrorKind::Corruption, "missing last membership chunk");
                    MemberChunk lastChunk = decodeChunk(*last, networkId, collection);
                    if (lastChunk.members.size() < chunkCapacity) {
                        lastChunk.members.append(memberId);
                        changedChunks[lastKey] = encodeChunk(lastChunk);
                        appended = true;
                    }
                }
                if (!appended) {
                    if (manifest.nextChunk[collection] == std::numeric_limits<quint32>::max())
                        throw StorageError(StorageErrorKind::LimitExceeded,
                                           "hydraulic chunk serial exhausted");
                    const QUuid chunkId = chunkUuid(networkId, collection,
                                                    manifest.nextChunk[collection]++);
                    const Key chunkKey = keyFromUuid(chunkId);
                    if (readCurrent(chunkKey) || changedChunks.contains(chunkKey))
                        throw StorageError(StorageErrorKind::Conflict,
                                           "hydraulic membership chunk identity collision");
                    chunkIds.append(chunkId);
                    changedChunks[chunkKey] = encodeChunk(MemberChunk{
                        networkId, collection, QList<QUuid>{memberId}});
                    rootChanged = true;
                }
            } else {
                if (!allMembers.erase(intent.member) ||
                    !manifest.members[collection].removeOne(memberId))
                    throw StorageError(StorageErrorKind::Conflict,
                                       "hydraulic entity is absent from expected collection");
                bool removed = false;
                for (qsizetype index = 0; index < chunkIds.size(); ++index) {
                    const Key chunkKey = keyFromUuid(chunkIds.at(index));
                    const std::map<Key, std::optional<ModelRecord>>::const_iterator staged =
                        changedChunks.find(chunkKey);
                    const std::optional<ModelRecord> old = staged == changedChunks.end()
                        ? readCurrent(chunkKey) : staged->second;
                    if (!old)
                        throw StorageError(StorageErrorKind::Corruption, "missing membership chunk");
                    MemberChunk chunk = decodeChunk(*old, networkId, collection);
                    if (!chunk.members.removeOne(memberId)) continue;
                    if (chunk.members.isEmpty()) {
                        changedChunks[chunkKey] = std::nullopt;
                        chunkIds.removeAt(index);
                        rootChanged = true;
                    } else {
                        changedChunks[chunkKey] = encodeChunk(chunk);
                    }
                    removed = true;
                    break;
                }
                if (!removed)
                    throw StorageError(StorageErrorKind::Corruption,
                                       "member missing from declared hydraulic chunk");
            }
        }
        for (const std::pair<const Key, std::optional<ModelRecord>>& change : changedChunks)
            nativeChanges.push_back(ModelChange{change.first, change.second});
        if (rootChanged) {
            ModelRecord changedRoot = *root;
            changedRoot.payload = bytesFromQByteArray(encodeManifest(manifest));
            changedRoot.references = chunkKeys(manifest);
            nativeChanges.push_back(ModelChange{group.first, std::move(changedRoot)});
        }
    }
    if (changes.size() + nativeChanges.size() > 4096)
        throw StorageError(StorageErrorKind::LimitExceeded,
                           "hydraulic membership draft has too many native changes");
    // The incoming vector is untouched if any intent fails validation.
    changes.insert(changes.end(), nativeChanges.begin(), nativeChanges.end());
}


std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimitedLegacy(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type, std::size_t maxCount) {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= collectionCount)
        throw StorageError(StorageErrorKind::InvalidInput, "unknown hydraulic entity collection");
    const std::optional<ModelRecord> root =
        repository.getModelAtRevision(revisionId, keyFromUuid(networkUuid));
    // Missing UUID, or one owned by another model entity: no such network.
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
    if (manifest.metadata.uuid != networkUuid)
        throw StorageError(StorageErrorKind::Corruption, "hydraulic network root UUID mismatch");
    checkRoot(manifest, *root);
    loadMembersAtRevision(repository, revisionId, manifest, index, maxCount);
    return manifest.members[index];
}

std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevision(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    HydraulicEntityType type) {
    return listHydraulicEntityIdsAtRevisionLimitedLegacy(
        repository, revisionId, networkUuid, type, std::numeric_limits<std::size_t>::max());
}

std::optional<QList<QUuid>> listHydraulicEntityIds(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    HydraulicEntityType type) {
    // Invalid collection selectors must fail even on an empty branch.
    if (static_cast<std::size_t>(type) >= collectionCount)
        throw StorageError(StorageErrorKind::InvalidInput, "unknown hydraulic entity collection");
    const Revision snapshot = repository.head(branch);
    if (!snapshot.modelIndexed && snapshot.root == Hash{}) {
        // An untouched branch contains no models, not a malformed network.
        return std::nullopt;
    }
    return listHydraulicEntityIdsAtRevision(repository, snapshot.id, networkUuid, type);
}

std::optional<HydraulicEntity> loadHydraulicEntityAtRevisionLegacy(
    Repository& repository, const Hash& revisionId, const QUuid& networkUuid,
    const QUuid& entityUuid) {
    if (networkUuid.isNull() || entityUuid.isNull() || networkUuid == entityUuid) {
        return std::nullopt;
    }
    // The root, owning chunk, reverse-reference index and entity payload
    // must all come from the same immutable snapshot.
    if (!ownerChunkAtRevision(repository, revisionId, networkUuid, keyFromUuid(entityUuid))) {
        return std::nullopt;
    }
    const std::optional<ModelRecord> record =
        repository.getModelAtRevision(revisionId, keyFromUuid(entityUuid));
    if (!record) throw StorageError(StorageErrorKind::Corruption, "owned hydraulic entity payload is missing");
    std::optional<HydraulicEntity> decoded;
    switch (record->kind) {
#define LOAD_ONE(name, type, kind) \
    case static_cast<std::uint16_t>(kind): { \
        QString decodeError; \
        std::optional<type> item = ModelCodec<type>::decode(qByteArrayFromBytes(record->payload), &decodeError); \
        if (!item || item->uuid != entityUuid) \
            throw StorageError(StorageErrorKind::Corruption, "invalid hydraulic entity payload: " + decodeError.toStdString()); \
        decoded.emplace(std::move(*item)); \
        break; \
    }
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(LOAD_ONE)
#undef LOAD_ONE
    default:
        throw StorageError(StorageErrorKind::Corruption, "owned hydraulic entity has unknown storage kind");
    }
    return decoded;
}

std::optional<HydraulicEntity> loadHydraulicEntity(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const QUuid& entityUuid) {
    const Revision snapshot = repository.head(branch);
    if (!snapshot.modelIndexed && snapshot.root == Hash{}) {
        // An untouched branch contains no models, not a malformed network.
        return std::nullopt;
    }
    return loadHydraulicEntityAtRevisionLegacy(repository, snapshot.id, networkUuid, entityUuid);
}

// The candidate is a private, lazy copy-on-write view held under the native
// merge lock. Do not call Repository APIs from this function or its helpers.
using CandidateReader = std::function<std::optional<ModelRecord>(const Key&)>;

void candidateMembers(const CandidateReader& read, Manifest& manifest) {
    std::set<Key> seen;
    for (std::size_t collection = 0; collection < collectionCount; ++collection) {
        for (const QUuid& id : manifest.chunks[collection]) {
            const std::optional<ModelRecord> record = read(keyFromUuid(id));
            if (!record) throw StorageError(StorageErrorKind::Corruption, "merged network is missing a membership chunk");
            const MemberChunk chunk = decodeChunk(*record, manifest.metadata.uuid, collection);
            for (const QUuid& member : chunk.members) {
                if (!seen.insert(keyFromUuid(member)).second)
                    throw StorageError(StorageErrorKind::Corruption, "merged network has duplicate members");
                manifest.members[collection].append(member);
            }
        }
    }
}

template<typename T>
void candidateEntities(const CandidateReader& read, const QList<QUuid>& ids,
                       std::uint16_t kind, QList<T>& output, DependencyIndex& recorded) {
    for (const QUuid& id : ids) {
        const std::optional<ModelRecord> record = read(keyFromUuid(id));
        if (!record || record->kind != kind)
            throw StorageError(StorageErrorKind::Corruption, "merged network contains a missing/wrong-kind entity");
        const QByteArray bytes = qByteArrayFromBytes(record->payload);
        const std::optional<T> decoded = aowis::dvmdb::ModelCodec<T>::decode(bytes);
        if (!decoded || decoded->uuid != id ||
            aowis::dvmdb::ModelCodec<T>::encode(*decoded) != bytes)
            throw StorageError(StorageErrorKind::Corruption, "merged network contains noncanonical entity bytes");
        for (const Key& ref : record->references)
            recorded.emplace_back(keyFromUuid(id), ref);
        output.append(*decoded);
    }
}

std::optional<std::string> validateMergedHydraulicSnapshot(const MergeCandidate& candidate) {
    try {
        std::vector<Key> roots;
        std::set<Key> existing;
        candidate.forEach([&roots, &existing](const Key& key, const ModelRecord& record) {
            if (!existing.insert(key).second)
                throw StorageError(StorageErrorKind::Corruption, "duplicate object UUID in merged snapshot");
            if (record.kind == hydraulicNetworkKind) roots.push_back(key);
        });
        if (roots.empty()) return std::string("hydraulic merge has no network root");
        std::set<Key> globallyOwned;
        for (const Key& rootKey : roots) {
            const std::optional<ModelRecord> record = candidate.get(rootKey);
            if (!record) throw StorageError(StorageErrorKind::Corruption, "missing merged hydraulic root");
            Manifest manifest = decodeManifest(qByteArrayFromBytes(record->payload));
            if (keyFromUuid(manifest.metadata.uuid) != rootKey)
                throw StorageError(StorageErrorKind::Corruption, "merged root UUID and payload mismatch");
            checkRoot(manifest, *record);
            candidateMembers(candidate.get, manifest);
            const std::set<Key> members = memberKeys(manifest);
            if (!globallyOwned.insert(rootKey).second)
                throw StorageError(StorageErrorKind::Corruption, "duplicate network root");
            for (const Key& chunk : chunkKeys(manifest)) {
                if (!globallyOwned.insert(chunk).second)
                    throw StorageError(StorageErrorKind::Corruption, "membership chunk belongs to multiple networks");
            }
            for (const Key& member : members) {
                if (!globallyOwned.insert(member).second)
                    throw StorageError(StorageErrorKind::Corruption, "hydraulic entity belongs to multiple networks");
            }
            NetworkHydraulic network = std::move(manifest.metadata);
            DependencyIndex stored;
            std::size_t ordinal = 0;
#define RESTORE_MERGED(name, type, kind) \
            candidateEntities(candidate.get, manifest.members[ordinal], \
                              static_cast<std::uint16_t>(kind), network.name, stored); \
            ++ordinal;
            AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(RESTORE_MERGED)
#undef RESTORE_MERGED
            const aowis::dvmdb::HydraulicDependencyValidation validated =
                aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
            if (!validated.report.valid())
                throw StorageError(StorageErrorKind::Corruption, "invalid combined hydraulic model: " +
                                         validated.report.summary().toStdString());
            DependencyIndex canonical = indexedDependencies(validated.dependencies, members);
            std::sort(stored.begin(), stored.end());
            stored.erase(std::unique(stored.begin(), stored.end()), stored.end());
            if (stored != canonical)
                throw StorageError(StorageErrorKind::Corruption, "merged model references disagree with canonical model payloads");
        }
        if (existing != globallyOwned)
            throw StorageError(StorageErrorKind::Corruption, "merged hydraulic snapshot contains unowned or orphaned objects");
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

// Reconcile independent membership edits using the common-ancestor membership
// order. Removals made on either branch are retained; independent additions
// from both branches survive. Existing members retain their original order.
QList<QUuid> mergeOrderedMembers(const QList<QUuid>& base, const QList<QUuid>& target,
                                const QList<QUuid>& source) {
    QSet<QUuid> baseline;
    QSet<QUuid> left;
    QSet<QUuid> right;
    for (const QUuid& id : base) baseline.insert(id);
    for (const QUuid& id : target) left.insert(id);
    for (const QUuid& id : source) right.insert(id);
    QList<QUuid> output;
    // Membership order is model data. Never silently replace a concurrent
    // reorder or a positional insertion with our deterministic append order.
    // This reconciler is deliberately restricted to append/remove edits.
    const auto appendOrRemoveOnly = [&base, &baseline](const QList<QUuid>& branch) {
        qsizetype previous = -1;
        bool encounteredNew = false;
        for (const QUuid& id : branch) {
            if (!baseline.contains(id)) { encounteredNew = true; continue; }
            if (encounteredNew) return false;
            const qsizetype position = base.indexOf(id);
            if (position <= previous) return false;
            previous = position;
        }
        return true;
    };
    if (!appendOrRemoveOnly(target) || !appendOrRemoveOnly(source))
        throw StorageError(StorageErrorKind::Conflict, "membership ordering changes require explicit resolution");
    QSet<QUuid> seen;
    const auto insert = [&output, &seen](const QUuid& id) {
        if (!seen.contains(id)) { output.append(id); seen.insert(id); }
    };
    for (const QUuid& id : base)
        if (left.contains(id) && right.contains(id)) insert(id);
    for (const QUuid& id : target)
        if (!baseline.contains(id)) insert(id);
    for (const QUuid& id : source)
        if (!baseline.contains(id)) insert(id);
    return output;
}

std::optional<ModelRecord> mergeHydraulicMembershipRecord(
    const ModelRecord& base, const ModelRecord& target, const ModelRecord& source) {
    if (base.kind != target.kind || base.kind != source.kind) return std::nullopt;
    if (base.kind == chunkKind) {
        // Read only enough of the envelope to determine the expected owner;
        // decodeChunk then verifies all bytes and the stored reference index.
        QDataStream stream(qByteArrayFromBytes(base.payload));
        stream.setVersion(QDataStream::Qt_6_0);
        quint32 magic = 0, version = 0, collection = 0;
        QUuid network;
        stream >> magic >> version >> network >> collection;
        if (stream.status() != QDataStream::Ok || magic != chunkMagic ||
            version != chunkVersion || collection >= collectionCount) return std::nullopt;
        const MemberChunk ancestor = decodeChunk(base, network, collection);
        const MemberChunk left = decodeChunk(target, network, collection);
        const MemberChunk right = decodeChunk(source, network, collection);
        MemberChunk merged{network, collection,
            mergeOrderedMembers(ancestor.members, left.members, right.members)};
        if (merged.members.isEmpty() || merged.members.size() > chunkCapacity)
            return std::nullopt;
        return encodeChunk(merged);
    }
    if (base.kind == hydraulicNetworkKind) {
        const Manifest ancestor = decodeManifest(qByteArrayFromBytes(base.payload));
        const Manifest left = decodeManifest(qByteArrayFromBytes(target.payload));
        const Manifest right = decodeManifest(qByteArrayFromBytes(source.payload));
        if (ancestor.metadata.uuid != left.metadata.uuid ||
            ancestor.metadata.uuid != right.metadata.uuid) return std::nullopt;
        const QByteArray baseMetadata = ModelCodec<NetworkHydraulic>::encode(ancestor.metadata);
        const QByteArray leftMetadata = ModelCodec<NetworkHydraulic>::encode(left.metadata);
        const QByteArray rightMetadata = ModelCodec<NetworkHydraulic>::encode(right.metadata);
        if (leftMetadata != rightMetadata && leftMetadata != baseMetadata &&
            rightMetadata != baseMetadata) return std::nullopt;
        Manifest merged = leftMetadata == baseMetadata ? right : left;
        for (std::size_t i = 0; i < collectionCount; ++i) {
            merged.chunks[i] = mergeOrderedMembers(ancestor.chunks[i],
                                                   left.chunks[i], right.chunks[i]);
            merged.nextChunk[i] = std::max(left.nextChunk[i], right.nextChunk[i]);
        }
        ModelRecord record;
        record.kind = hydraulicNetworkKind;
        record.payload = bytesFromQByteArray(encodeManifest(merged));
        record.references = chunkKeys(merged);
        return record;
    }
    return std::nullopt;
}

std::optional<ModelRecord> mergeConcurrentHydraulicChunk(
    const ModelRecord& target, const ModelRecord& source) {
    if (target.kind != chunkKind || source.kind != chunkKind) return std::nullopt;
    QDataStream stream(qByteArrayFromBytes(target.payload));
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0, collection = 0;
    QUuid network;
    stream >> magic >> version >> network >> collection;
    if (stream.status() != QDataStream::Ok || magic != chunkMagic ||
        version != chunkVersion || collection >= collectionCount) return std::nullopt;
    const MemberChunk left = decodeChunk(target, network, collection);
    const MemberChunk right = decodeChunk(source, network, collection);
    MemberChunk merged{network, collection, left.members};
    for (const QUuid& id : right.members)
        if (!merged.members.contains(id)) merged.members.append(id);
    if (merged.members.size() > chunkCapacity) return std::nullopt;
    return encodeChunk(merged);
}

// A selected object may have been removed from its old membership chunk by
// the other branch. Locate its *actual historical owner*, then restore only
// the chosen entity to the candidate membership; never restore unrelated
// records deleted by either branch. All updates are ephemeral until the
// repository's reference and full hydraulic validators accept the snapshot.
std::optional<std::string> reconcileExplicitHydraulicMembership(
    const MergeCandidate& candidate,
    const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
    const CandidateReader& target,
    const CandidateReader& source,
    const std::map<Key, MergeChoiceSide>& choices) {
    try {
        std::vector<Key> networks;
        candidate.forEach([&networks](const Key& key, const ModelRecord& record) {
            if (record.kind == hydraulicNetworkKind) networks.push_back(key);
        });
        for (const std::pair<const Key, MergeChoiceSide>& choice : choices) {
            const std::optional<ModelRecord> historicalTarget = target(choice.first);
            const std::optional<ModelRecord> historicalSource = source(choice.first);
            for (const std::optional<ModelRecord>& record : {historicalTarget, historicalSource}) {
                if (record && (record->kind == hydraulicNetworkKind || record->kind == chunkKind))
                    throw StorageError(StorageErrorKind::InvalidInput, "internal membership records cannot be selected directly");
            }
            const std::optional<ModelRecord> kept = candidate.get(choice.first);
            if (!kept || kept->kind == hydraulicNetworkKind || kept->kind == chunkKind)
                continue; // a selected deletion, or a non-entity choice
            std::size_t type = collectionCount;
            const std::array<std::uint16_t, collectionCount> kinds = {
#define RESTORE_KIND(name, entityType, kind) static_cast<std::uint16_t>(kind),
                AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(RESTORE_KIND)
#undef RESTORE_KIND
            };
            for (std::size_t i = 0; i < kinds.size(); ++i)
                if (kept->kind == kinds[i]) type = i;
            if (type == collectionCount) continue;

            // Already represented by a surviving membership chunk? Nothing
            // to repair; the normal validator will check all relationships.
            std::optional<Key> owner;
            bool alreadyMember = false;
            for (const Key& networkKey : networks) {
                const ModelRecord root = *candidate.get(networkKey);
                const Manifest manifest = decodeManifest(qByteArrayFromBytes(root.payload));
                for (const QUuid& chunkId : manifest.chunks[type]) {
                    const std::optional<ModelRecord> record = candidate.get(keyFromUuid(chunkId));
                    if (!record) continue; // validator handles unrelated inconsistencies
                    const MemberChunk chunk = decodeChunk(*record, manifest.metadata.uuid, type);
                    if (chunk.members.contains(uuidFromKey(choice.first))) {
                        if (owner) throw StorageError(StorageErrorKind::Conflict, "entity belongs to multiple candidate networks");
                        owner = networkKey;
                        alreadyMember = true;
                    }
                }
            }
            if (alreadyMember) continue;

            // Look for membership on the selected side of the original edit.
            const CandidateReader& selectedSide = choice.second == MergeChoiceSide::Source
                ? source : target;
            for (const Key& networkKey : networks) {
                const std::optional<ModelRecord> historicRoot = selectedSide(networkKey);
                if (!historicRoot || historicRoot->kind != hydraulicNetworkKind) continue;
                const Manifest original = decodeManifest(qByteArrayFromBytes(historicRoot->payload));
                for (const QUuid& chunkId : original.chunks[type]) {
                    const std::optional<ModelRecord> historicChunk = selectedSide(keyFromUuid(chunkId));
                    if (!historicChunk) throw StorageError(StorageErrorKind::Corruption, "missing historical membership chunk");
                    const MemberChunk members = decodeChunk(*historicChunk, original.metadata.uuid, type);
                    if (members.members.contains(uuidFromKey(choice.first))) {
                        if (owner) throw StorageError(StorageErrorKind::Conflict, "ambiguous historical entity ownership");
                        owner = networkKey;
                    }
                }
            }
            if (!owner) throw StorageError(StorageErrorKind::Conflict, "chosen entity has no historical network membership");
            ModelRecord root = *candidate.get(*owner);
            Manifest manifest = decodeManifest(qByteArrayFromBytes(root.payload));
            const QUuid id = uuidFromKey(choice.first);
            bool appended = false;
            if (!manifest.chunks[type].isEmpty()) {
                const Key lastKey = keyFromUuid(manifest.chunks[type].last());
                const std::optional<ModelRecord> previous = candidate.get(lastKey);
                if (previous) {
                    MemberChunk chunk = decodeChunk(*previous, manifest.metadata.uuid, type);
                    if (chunk.members.size() < chunkCapacity) {
                        chunk.members.append(id);
                        set(lastKey, encodeChunk(chunk));
                        appended = true;
                    }
                }
            }
            if (!appended) {
                if (manifest.nextChunk[type] == std::numeric_limits<quint32>::max())
                    throw StorageError(StorageErrorKind::LimitExceeded, "membership serial exhausted while restoring entity");
                const QUuid newId = chunkUuid(manifest.metadata.uuid, type, manifest.nextChunk[type]++);
                if (candidate.get(keyFromUuid(newId)))
                    throw StorageError(StorageErrorKind::Internal, "membership UUID collision during merge");
                manifest.chunks[type].append(newId);
                set(keyFromUuid(newId), encodeChunk(MemberChunk{manifest.metadata.uuid, type, {id}}));
                root.payload = bytesFromQByteArray(encodeManifest(manifest));
                root.references = chunkKeys(manifest);
                set(*owner, std::move(root));
            }
        }
        return std::nullopt;
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
}

// Core has no Qt dependency. Only canonical, already-valid typed entities
// can reach this resolver, and only if their stored references are unchanged.
MergeResolution resolveHydraulicRecord(const Key& key, const ModelRecord& base,
                                        const ModelRecord& target, const ModelRecord& source,
                                        const QMap<QString, bool>& fieldChoices) {
    MergeResolution rejected{};
    if (base.kind == chunkKind || base.kind == hydraulicNetworkKind) {
        try {
            const std::optional<ModelRecord> merged =
                mergeHydraulicMembershipRecord(base, target, source);
            if (merged) return MergeResolution{*merged, {}};
        } catch (const std::exception&) { /* conservative conflict */ }
        return rejected;
    }
    if (base.kind != target.kind || target.kind != source.kind ||
        base.references != target.references || base.references != source.references)
        return rejected; // topology or ownership change
    QString kindName;
    // Map persisted kind codes to the codec's canonical field-merge descriptors.
    static const std::array<const char*, collectionCount> canonicalKinds = {
        "hydraulic.pipe_material", "hydraulic.pattern_time", "hydraulic.curve_tank_volume",
        "hydraulic.curve_pump_head", "hydraulic.curve_pump_efficiency",
        "hydraulic.curve_valve_headloss", "hydraulic.curve_valve_characteristic",
        "hydraulic.curve_generic", "hydraulic.reservoir", "hydraulic.junction",
        "hydraulic.tank", "hydraulic.demand_point", "hydraulic.pipe",
        "hydraulic.pump", "hydraulic.valve", "hydraulic.control_simple",
        "hydraulic.control_rule", "hydraulic.map_label"
    };
    const std::array<std::uint16_t, collectionCount> kinds = {
#define MERGE_CODE(name, type, kind) static_cast<std::uint16_t>(kind),
        AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(MERGE_CODE)
#undef MERGE_CODE
    };
    for (std::size_t i = 0; i < kinds.size(); ++i)
        if (kinds[i] == base.kind) kindName = QString::fromLatin1(canonicalKinds[i]);
    if (kindName.isEmpty()) return rejected;
    const QByteArray baseBytes = qByteArrayFromBytes(base.payload);
    const QByteArray targetBytes = qByteArrayFromBytes(target.payload);
    const QByteArray sourceBytes = qByteArrayFromBytes(source.payload);
    if (!fieldChoices.isEmpty()) {
        const std::optional<aowis::dvmdb::ModelFieldMerge> preview =
            aowis::dvmdb::mergeModelFields(uuidFromKey(key), kindName,
                baseBytes, targetBytes, sourceBytes);
        if (!preview) throw StorageError(StorageErrorKind::Corruption, "cannot resolve fields of an invalid canonical model");
        for (QMap<QString, bool>::const_iterator choice = fieldChoices.cbegin();
             choice != fieldChoices.cend(); ++choice) {
            if (!preview->conflicting_fields.contains(choice.key()))
                throw StorageError(StorageErrorKind::InvalidInput, "merge selection is not a conflicting field: " +
                                         choice.key().toStdString());
        }
    }
    const std::optional<aowis::dvmdb::ModelFieldMerge> merged =
        aowis::dvmdb::mergeModelFields(uuidFromKey(key), kindName,
            baseBytes, targetBytes, sourceBytes, fieldChoices);
    if (!merged) return rejected;
    if (!merged->conflicting_fields.isEmpty()) {
        for (const QString& field : merged->conflicting_fields)
            rejected.conflictingFields.push_back(field.toStdString());
        return rejected;
    }
    if (merged->content.isEmpty()) return rejected;
    ModelRecord record = target;
    record.payload = bytesFromQByteArray(merged->content);
    return MergeResolution{std::move(record), {}};
}

// Shared canonical hydraulic merge machinery: identical behavior for SHA and
// UUIDv7 revision identities. All captured selection state remains live until
// the repository finishes invoking the pure resolution callbacks.
template<typename Callback>
std::invoke_result_t<Callback, const MergeOptions&> mergeHydraulicWithChoices(
        const std::vector<HydraulicFieldChoice>& fields,
        const std::vector<HydraulicObjectChoice>& objects, Callback invoke) {
    // Build an immutable lookup before entering the Repository's merge lock.
    // A duplicate (UUID, field) is rejected rather than choosing an arbitrary
    // last-write-wins interpretation of the user's instructions.
    std::map<Key, QMap<QString, bool>> choices;
    for (const HydraulicFieldChoice& choice : fields) {
        if (choice.entityUuid.isNull() || choice.field.isEmpty())
            throw StorageError(StorageErrorKind::InvalidInput, "merge choice needs a non-null UUID and field name");
        const Key key = keyFromUuid(choice.entityUuid);
        QMap<QString, bool>& perEntity = choices[key];
        if (perEntity.contains(choice.field))
            throw StorageError(StorageErrorKind::InvalidInput, "duplicate merge choice: " + choice.field.toStdString());
        perEntity.insert(choice.field, choice.side == HydraulicMergeSide::Source);
    }
    std::map<Key, MergeChoiceSide> objectChoices;
    for (const HydraulicObjectChoice& choice : objects) {
        if (choice.entityUuid.isNull())
            throw StorageError(StorageErrorKind::InvalidInput, "merge object choice needs a non-null UUID");
        const Key key = keyFromUuid(choice.entityUuid);
        if (choices.contains(key))
            throw StorageError(StorageErrorKind::InvalidInput, "cannot select both object and fields for one UUID");
        if (!objectChoices.emplace(key, choice.side == HydraulicMergeSide::Source
                                     ? MergeChoiceSide::Source : MergeChoiceSide::Target).second)
            throw StorageError(StorageErrorKind::InvalidInput, "duplicate merge object choice");
    }
    std::set<Key> consumed;
    std::set<Key> consumedObjects;
    MergeOptions options;
    options.chooseConflict = [&objectChoices, &consumedObjects](const MergeConflict& conflict)
        -> std::optional<MergeChoiceSide> {
        const std::map<Key, MergeChoiceSide>::const_iterator choice =
            objectChoices.find(conflict.key);
        if (choice == objectChoices.end()) return std::nullopt;
        consumedObjects.insert(conflict.key);
        return choice->second;
    };
    options.resolve = [&choices, &consumed](const Key& key, const ModelRecord& base,
                                            const ModelRecord& targetRecord,
                                            const ModelRecord& sourceRecord) {
        const std::map<Key, QMap<QString, bool>>::const_iterator match = choices.find(key);
        const QMap<QString, bool> empty;
        const QMap<QString, bool>& fieldChoices = match == choices.end() ? empty : match->second;
        MergeResolution result = resolveHydraulicRecord(key, base, targetRecord,
                                                        sourceRecord, fieldChoices);
        if (match != choices.end()) consumed.insert(key);
        return result;
    };
    options.resolveConcurrentAddition = [](const Key&, const ModelRecord& targetRecord,
                                            const ModelRecord& sourceRecord) {
        try {
            const std::optional<ModelRecord> merged =
                mergeConcurrentHydraulicChunk(targetRecord, sourceRecord);
            if (merged) return MergeResolution{*merged, {}};
        } catch (const std::exception&) { /* fail closed */ }
        return MergeResolution{};
    };
    options.checkSelections = [&choices, &consumed, &objectChoices, &consumedObjects]()
        -> std::optional<std::string> {
        if (choices.size() != consumed.size())
            return std::string("merge contains unused or inapplicable field choices");
        if (objectChoices.size() != consumedObjects.size())
            return std::string("merge contains unused or inapplicable object choices");
        return std::nullopt;
    };
    options.reconcile = [&objectChoices](
        const MergeCandidate& candidate,
        const std::function<void(const Key&, std::optional<ModelRecord>)>& set,
        const CandidateReader& targetReader, const CandidateReader& sourceReader) {
        return reconcileExplicitHydraulicMembership(candidate, set, targetReader,
                                                    sourceReader, objectChoices);
    };
    options.validate = validateMergedHydraulicSnapshot;
    return invoke(options);
}


MergeResult mergeHydraulicBranchesLegacy(Repository& repository, std::string_view target,
                                   std::string_view source,
                                   const HydraulicMergeSelectionsLegacy& selections) {
    if ((!selections.fields.empty() || !selections.objects.empty()) &&
        (!selections.expectedTargetHead || !selections.expectedSourceHead))
        throw StorageError(StorageErrorKind::InvalidInput,
                           "explicit merge choices require both previewed branch heads");
    return mergeHydraulicWithChoices(selections.fields, selections.objects,
        [&](const MergeOptions& original) {
            MergeOptions pinned = original;
            pinned.expectedTargetHead = selections.expectedTargetHead;
            pinned.expectedSourceHead = selections.expectedSourceHead;
            return repository.mergeBranches(target, source, pinned);
        });
}

MergeResultV2 mergeHydraulicBranches(Repository& repository, const ProjectId& project,
                                       std::string_view target, std::string_view source,
                                       const HydraulicMergeSelections& selections) {
    return mergeHydraulicWithChoices(selections.fields, selections.objects,
        [&](const MergeOptions& options) {
            return repository.mergeBranchesV2(project, target, source,
                selections.expectedTargetHead, selections.expectedSourceHead, options);
        });
}

Revision saveHydraulicNetworkLegacy(Repository& repository, std::string_view branch,
                              const NetworkHydraulic& network, std::optional<Hash> expectedHeadFromCaller) {
    // Read a stable logical starting point. commitModel() verifies this head
    // again under its repository lock, preventing lost updates between the
    // membership read and final revision publication.
    const Revision startingRevision = repository.head(branch);
    if (expectedHeadFromCaller && startingRevision.id != *expectedHeadFromCaller)
        throw StorageError(StorageErrorKind::Conflict, "branch head changed since hydraulic network save was prepared");
    const Hash expectedHead = expectedHeadFromCaller.value_or(startingRevision.id);
    Manifest next = manifestFor(network);
    const std::set<Key> newMembers = memberKeys(next);
    DependencyIndex references;
    {
        const aowis::dvmdb::HydraulicDependencyValidation validated =
            aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
        if (!validated.report.valid())
            throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic network: " + validated.report.summary().toStdString());
        references = indexedDependencies(validated.dependencies, newMembers);
    }

    // Discover the old membership first. Updates outside the old membership
    // are only valid when their UUID is not already claimed by another object.
    const Key networkKey = keyFromUuid(network.uuid);
    // A newly created branch starts with an empty raw revision. Its first
    // model commit is valid, but getModelAtRevision() intentionally rejects
    // raw revisions, even when the tree is empty.
    if (!startingRevision.modelIndexed && startingRevision.root != Hash{}) {
        throw StorageError(StorageErrorKind::InvalidInput, "cannot save a hydraulic network to a populated raw branch");
    }
    std::optional<ModelRecord> currentRoot;
    if (startingRevision.modelIndexed) {
        currentRoot = repository.getModelAtRevision(expectedHead, networkKey);
    }
    std::set<Key> oldMembers;
    if (currentRoot) {
        if (currentRoot->kind != hydraulicNetworkKind) {
            throw StorageError(StorageErrorKind::Conflict, "network UUID already used by another model entity");
        }
        Manifest previous = decodeManifest(qByteArrayFromBytes(currentRoot->payload));
        if (previous.metadata.uuid != network.uuid) throw StorageError(StorageErrorKind::Corruption, "network root identity mismatch");
        checkRoot(previous, *currentRoot);
        loadMembersAtRevision(repository, expectedHead, previous);
        oldMembers = memberKeys(previous);
    }
    for (const Key& key : newMembers) {
        if (startingRevision.modelIndexed && !oldMembers.contains(key) &&
            repository.getModelAtRevision(expectedHead, key).has_value()) {
            throw StorageError(StorageErrorKind::Conflict, "hydraulic entity UUID already exists outside this network");
        }
    }

    std::vector<ModelChange> changes;
    changes.reserve(newMembers.size() + oldMembers.size() + 1);
    // IDs are already in the manifest; build canonical payload records only.
#define APPEND(name, type, kind) \
    addEntities(network.name, static_cast<std::uint16_t>(kind), references, changes);
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(APPEND)
#undef APPEND
    for (const Key& key : oldMembers) {
        if (!newMembers.contains(key)) changes.push_back(ModelChange{key, std::nullopt});
    }

    ModelRecord root;
    root.kind = hydraulicNetworkKind;
    buildChunks(next, changes);
    if (currentRoot) {
        const Manifest previous = decodeManifest(qByteArrayFromBytes(currentRoot->payload));
        const std::vector<Key> oldChunks = chunkKeys(previous);
        const std::vector<Key> newChunks = chunkKeys(next);
        for (const Key& oldChunk : oldChunks)
            if (!std::binary_search(newChunks.begin(), newChunks.end(), oldChunk))
                changes.push_back(ModelChange{oldChunk, std::nullopt});
    }
    root.payload = bytesFromQByteArray(encodeManifest(next));
    root.references = chunkKeys(next);
    changes.push_back(ModelChange{networkKey, std::move(root)});
    return repository.commitModel(branch, std::move(changes), expectedHead);
}


Revision editHydraulicNetworkLegacy(Repository& repository, std::string_view branch,
                              const QUuid& networkUuid,
                              const std::vector<HydraulicEntityChange>& changes,
                              HydraulicEditDiagnostics* diagnostics,
                              std::optional<Hash> expectedHeadFromCaller) {
    if (networkUuid.isNull()) throw StorageError(StorageErrorKind::InvalidInput, "hydraulic network UUID is null");
    if (diagnostics) *diagnostics = HydraulicEditDiagnostics{};
    const Revision startingRevision = repository.head(branch);
    if (expectedHeadFromCaller && startingRevision.id != *expectedHeadFromCaller)
        throw StorageError(StorageErrorKind::Conflict, "branch head changed since hydraulic edit was prepared");
    if (changes.empty()) {
        if (repository.head(branch).id != startingRevision.id)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic edit");
        return startingRevision;
    }
    // Physical value rules apply to every edited entity before any of the
    // indexed fast paths below may store it.
    for (const HydraulicEntityChange& change : changes) {
        if (!change.value || change.value->valueless_by_exception()) continue;
        const aowis::dvmdb::ModelValidationReport values = std::visit(
            [](const auto& entity) { return aowis::dvmdb::validateHydraulicEntityValues(entity); },
            *change.value);
        if (!values.valid())
            throw StorageError(StorageErrorKind::InvalidInput,
                               "invalid hydraulic edit: " + values.summary().toStdString());
    }
    if (const std::optional<Revision> indexed = tryIndexedScalarEdits(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }
    if (const std::optional<Revision> indexed = tryIndexedPipeRewires(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    if (const std::optional<Revision> indexed = tryIndexedPhysicalUpdates(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    if (const std::optional<Revision> indexed = tryIndexedCatalogueUpdates(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    if (const std::optional<Revision> indexed = tryIndexedDependentUpdates(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    if (const std::optional<Revision> indexed = tryIndexedMembershipEdits(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    // The whole-network loader checks all stored UUIDs, kinds, reference edges and
    // network semantics; damage it finds propagates as a typed StorageError.
    std::optional<NetworkHydraulic> network =
        loadHydraulicNetworkAtRevisionLegacy(repository, startingRevision.id, networkUuid);
    if (!network) {
        throw StorageError(StorageErrorKind::NotFound,
                           "cannot edit hydraulic network " + networkUuid.toString().toStdString() + ": not found");
    }
    const Manifest previous = manifestFor(*network);
    const std::map<Key, std::size_t> oldOwners = collectionOwners(previous);
    std::set<Key> editKeys;

    for (const HydraulicEntityChange& change : changes) {
        const Key key = keyFromUuid(change.uuid);
        if (change.uuid.isNull() || change.uuid == networkUuid || !editKeys.insert(key).second) {
            throw StorageError(StorageErrorKind::InvalidInput, "null, network-owned, or duplicate hydraulic edit UUID");
        }
        const std::map<Key, std::size_t>::const_iterator existing = oldOwners.find(key);
        std::size_t collection = 0;
        if (change.value) {
            const QUuid payloadId = std::visit([](const auto& entity) { return entity.uuid; }, *change.value);
            if (payloadId != change.uuid) throw StorageError(StorageErrorKind::InvalidInput, "hydraulic edit UUID disagrees with payload");
            if (change.value->valueless_by_exception()) {
                throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic entity variant");
            }
            collection = change.value->index();
            if (existing != oldOwners.end() && existing->second != collection) {
                throw StorageError(StorageErrorKind::InvalidInput, "cannot change a hydraulic entity's collection kind");
            }
            if (existing == oldOwners.end() && repository.getModelAtRevision(startingRevision.id, key)) {
                throw StorageError(StorageErrorKind::Conflict, "hydraulic UUID already owned outside this network");
            }
        } else {
            if (existing == oldOwners.end()) throw StorageError(StorageErrorKind::InvalidInput, "cannot delete an unowned hydraulic UUID");
            collection = existing->second;
        }
        applyEntityChange(*network, change, collection);
    }

    // Use the complete canonical model validator until we have a proven
    // incremental validator with identical cross-entity semantics.
    const Manifest next = manifestFor(*network);
    const std::set<Key> nextKeys = memberKeys(next);
    DependencyIndex references;
    {
        const aowis::dvmdb::HydraulicDependencyValidation validated =
            aowis::dvmdb::validateHydraulicNetworkWithDependencies(*network);
        if (!validated.report.valid())
            throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic edit: " + validated.report.summary().toStdString());
        references = indexedDependencies(validated.dependencies, nextKeys);
    }

    std::vector<ModelChange> pending;
    pending.reserve(changes.size() + 1);
    for (const HydraulicEntityChange& change : changes) {
        ModelChange encoded = encodeChangedEntity(change, references);
        const std::optional<ModelRecord> old = repository.getModelAtRevision(startingRevision.id, encoded.key);
        // Avoid changing the revision for byte-for-byte unchanged records.
        if (old && encoded.record && encodeModelRecord(*old) == encodeModelRecord(*encoded.record)) continue;
        pending.push_back(std::move(encoded));
    }
    if (next.members != previous.members) {
        Manifest updated = next;
        buildChunks(updated, pending);
        const std::optional<ModelRecord> oldRoot =
            repository.getModelAtRevision(startingRevision.id, keyFromUuid(networkUuid));
        if (!oldRoot) throw StorageError(StorageErrorKind::Corruption, "missing network root during full edit");
        const Manifest oldManifest = decodeManifest(qByteArrayFromBytes(oldRoot->payload));
        const std::vector<Key> newChunks = chunkKeys(updated);
        for (const Key& oldChunk : chunkKeys(oldManifest))
            if (!std::binary_search(newChunks.begin(), newChunks.end(), oldChunk))
                pending.push_back(ModelChange{oldChunk, std::nullopt});
        ModelRecord root;
        root.kind = hydraulicNetworkKind;
        root.payload = bytesFromQByteArray(encodeManifest(updated));
        root.references = newChunks;
        pending.push_back(ModelChange{keyFromUuid(networkUuid), std::move(root)});
    }
    if (pending.empty()) {
        // Even a no-op must not silently return success after another writer
        // has advanced the branch while this batch was being prepared.
        if (repository.head(branch).id != startingRevision.id) {
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic edit");
        }
        return startingRevision;
    }
    return repository.commitModel(branch, std::move(pending), startingRevision.id);
}

std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevisionLegacy(Repository& repository,
                                                               const Hash& revisionId,
                                                               const QUuid& networkUuid) {
    const std::optional<ModelRecord> root =
        repository.getModelAtRevision(revisionId, keyFromUuid(networkUuid));
    if (!root) {
        return std::nullopt;
    }
    if (root->kind != hydraulicNetworkKind) {
        // Another model entity owns this UUID: there is no such network.
        return std::nullopt;
    }
    Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
    if (manifest.metadata.uuid != networkUuid) throw StorageError(StorageErrorKind::Corruption, "hydraulic network root UUID mismatch");
    checkRoot(manifest, *root);
    loadMembersAtRevision(repository, revisionId, manifest);
    const std::set<Key> members = memberKeys(manifest);

    NetworkHydraulic network = std::move(manifest.metadata);
    DependencyIndex storedReferences;
    std::size_t ordinal = 0;
#define RESTORE(name, type, kind) \
    restoreEntities(repository, revisionId, manifest.members[ordinal], \
                    static_cast<std::uint16_t>(kind), network.name, storedReferences); \
    ++ordinal;
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(RESTORE)
#undef RESTORE
    const aowis::dvmdb::HydraulicDependencyValidation validated =
        aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
    if (!validated.report.valid())
        throw StorageError(StorageErrorKind::Corruption, "loaded network invalid: " + validated.report.summary().toStdString());
    const DependencyIndex canonicalReferences = indexedDependencies(validated.dependencies, members);
    std::sort(storedReferences.begin(), storedReferences.end());
    storedReferences.erase(std::unique(storedReferences.begin(), storedReferences.end()),
                           storedReferences.end());
    if (storedReferences != canonicalReferences)
        throw StorageError(StorageErrorKind::Corruption, "stored hydraulic references differ from canonical model payload");
    return network;
}

std::optional<NetworkHydraulic> loadHydraulicNetwork(Repository& repository,
                                                      std::string_view branch,
                                                      const QUuid& networkUuid) {
    const Revision snapshot = repository.head(branch);
    if (!snapshot.modelIndexed && snapshot.root == Hash{}) {
        // An untouched branch contains no models, not a malformed network.
        return std::nullopt;
    }
    return loadHydraulicNetworkAtRevisionLegacy(repository, snapshot.id, networkUuid);
}

// UUIDv7 hydraulic operations deliberately use the same manifest codec,
// membership chunks, canonical entity payloads and semantic validator as V1.
// There is no translation of revision UUIDs into content-address identities.
std::optional<QList<QUuid>> listHydraulicEntityIdsAtRevisionLimited(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, HydraulicEntityType type, std::size_t maxCount) {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= collectionCount)
        throw StorageError(StorageErrorKind::InvalidInput, "unknown hydraulic entity collection");
    const std::optional<ModelRecord> root =
        repository.getModelAtRevisionV2(project, revisionId, keyFromUuid(networkUuid));
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
    if (manifest.metadata.uuid != networkUuid)
        throw StorageError(StorageErrorKind::Corruption, "hydraulic network root UUID mismatch");
    checkRoot(manifest, *root);
    loadMembersAtRevisionV2(repository, project, revisionId, manifest, index, maxCount);
    return manifest.members[index];
}

std::optional<HydraulicEntity> loadHydraulicEntityAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid, const QUuid& entityUuid) {
    if (networkUuid.isNull() || entityUuid.isNull() || networkUuid == entityUuid)
        return std::nullopt;
    if (!ownerChunkAtRevisionV2(repository, project, revisionId, networkUuid, keyFromUuid(entityUuid)))
        return std::nullopt;
    const std::optional<ModelRecord> record =
        repository.getModelAtRevisionV2(project, revisionId, keyFromUuid(entityUuid));
    if (!record) throw StorageError(StorageErrorKind::Corruption, "owned hydraulic entity payload is missing");
    std::optional<HydraulicEntity> decoded;
    switch (record->kind) {
#define LOAD_ONE_V2(name, type, kind) \
    case static_cast<std::uint16_t>(kind): { \
        QString error; \
        std::optional<type> item = ModelCodec<type>::decode(qByteArrayFromBytes(record->payload), &error); \
        if (!item || item->uuid != entityUuid) \
            throw StorageError(StorageErrorKind::Corruption, "invalid hydraulic entity payload: " + error.toStdString()); \
        decoded.emplace(std::move(*item)); \
        break; \
    }
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(LOAD_ONE_V2)
#undef LOAD_ONE_V2
    default:
        throw StorageError(StorageErrorKind::Corruption, "owned hydraulic entity has unknown storage kind");
    }
    return decoded;
}

std::optional<NetworkHydraulic> loadHydraulicNetworkAtRevision(
    Repository& repository, const ProjectId& project, const RevisionId& revisionId,
    const QUuid& networkUuid) {
    const std::optional<ModelRecord> root =
        repository.getModelAtRevisionV2(project, revisionId, keyFromUuid(networkUuid));
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
    if (manifest.metadata.uuid != networkUuid)
        throw StorageError(StorageErrorKind::Corruption, "hydraulic network root UUID mismatch");
    checkRoot(manifest, *root);
    loadMembersAtRevisionV2(repository, project, revisionId, manifest);
    const std::set<Key> members = memberKeys(manifest);
    NetworkHydraulic network = std::move(manifest.metadata);
    DependencyIndex storedReferences;
    std::size_t ordinal = 0;
#define RESTORE_V2(name, type, kind) \
    restoreEntitiesV2(repository, project, revisionId, manifest.members[ordinal], \
                      static_cast<std::uint16_t>(kind), network.name, storedReferences); \
    ++ordinal;
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(RESTORE_V2)
#undef RESTORE_V2
    const aowis::dvmdb::HydraulicDependencyValidation validated =
        aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
    if (!validated.report.valid())
        throw StorageError(StorageErrorKind::Corruption,
                           "loaded network invalid: " + validated.report.summary().toStdString());
    const DependencyIndex canonicalReferences = indexedDependencies(validated.dependencies, members);
    std::sort(storedReferences.begin(), storedReferences.end());
    storedReferences.erase(std::unique(storedReferences.begin(), storedReferences.end()),
                           storedReferences.end());
    if (storedReferences != canonicalReferences)
        throw StorageError(StorageErrorKind::Corruption,
                           "stored hydraulic references differ from canonical model payload");
    return network;
}

RevisionId saveHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const NetworkHydraulic& network,
                                  const RevisionId& expectedHead) {
    if (network.uuid.isNull())
        throw StorageError(StorageErrorKind::InvalidInput, "hydraulic network UUID is null");
    // Pin one immutable starting point. Native commitModelV2() rechecks the
    // expected HEAD under the writer lock before publishing any new revision.
    if (repository.headV2(project, branch) != expectedHead)
        throw StorageError(StorageErrorKind::Conflict, "branch head changed since hydraulic save was prepared");
    const std::optional<Bytes> startingBytes = repository.readRevisionV2(project, expectedHead);
    if (!startingBytes) throw StorageError(StorageErrorKind::NotFound, "revision UUID not found");
    const RevisionRecordV2 starting = decodeRevisionRecordV2(*startingBytes);
    if (!starting.modelIndexed && starting.root != Hash{})
        throw StorageError(StorageErrorKind::InvalidInput, "cannot save a hydraulic network to a populated raw branch");
    Manifest next = manifestFor(network);
    const std::set<Key> newMembers = memberKeys(next);
    const aowis::dvmdb::HydraulicDependencyValidation validated =
        aowis::dvmdb::validateHydraulicNetworkWithDependencies(network);
    if (!validated.report.valid())
        throw StorageError(StorageErrorKind::InvalidInput,
                           "invalid hydraulic network: " + validated.report.summary().toStdString());
    const DependencyIndex references = indexedDependencies(validated.dependencies, newMembers);
    const Key networkKey = keyFromUuid(network.uuid);
    std::optional<ModelRecord> currentRoot;
    if (starting.modelIndexed)
        currentRoot = repository.getModelAtRevisionV2(project, expectedHead, networkKey);
    std::set<Key> oldMembers;
    if (currentRoot) {
        if (currentRoot->kind != hydraulicNetworkKind)
            throw StorageError(StorageErrorKind::Conflict, "network UUID already used by another entity");
        Manifest previous = decodeManifest(qByteArrayFromBytes(currentRoot->payload));
        if (previous.metadata.uuid != network.uuid)
            throw StorageError(StorageErrorKind::Corruption, "network root identity mismatch");
        checkRoot(previous, *currentRoot);
        loadMembersAtRevisionV2(repository, project, expectedHead, previous);
        oldMembers = memberKeys(previous);
    }
    for (const Key& key : newMembers) {
        if (starting.modelIndexed && !oldMembers.contains(key) &&
            repository.getModelAtRevisionV2(project, expectedHead, key))
            throw StorageError(StorageErrorKind::Conflict, "hydraulic entity UUID already exists outside network");
    }
    std::vector<ModelChange> changes;
    changes.reserve(newMembers.size() + oldMembers.size() + 1);
#define APPEND_V2(name, type, kind) \
    addEntities(network.name, static_cast<std::uint16_t>(kind), references, changes);
    AOWIS_DVMDB_HYDRAULIC_COLLECTIONS(APPEND_V2)
#undef APPEND_V2
    for (const Key& key : oldMembers)
        if (!newMembers.contains(key)) changes.push_back(ModelChange{key, std::nullopt});
    buildChunks(next, changes);
    if (currentRoot) {
        const Manifest previous = decodeManifest(qByteArrayFromBytes(currentRoot->payload));
        const std::vector<Key> newChunks = chunkKeys(next);
        for (const Key& oldChunk : chunkKeys(previous))
            if (!std::binary_search(newChunks.begin(), newChunks.end(), oldChunk))
                changes.push_back(ModelChange{oldChunk, std::nullopt});
    }
    ModelRecord root;
    root.kind = hydraulicNetworkKind;
    root.payload = bytesFromQByteArray(encodeManifest(next));
    root.references = chunkKeys(next);
    changes.push_back(ModelChange{networkKey, std::move(root)});
    return repository.commitModelV2(project, branch, std::move(changes), expectedHead);
}

RevisionId editHydraulicNetwork(Repository& repository, const ProjectId& project,
                                  std::string_view branch, const QUuid& networkUuid,
                                  const std::vector<HydraulicEntityChange>& changes,
                                  const RevisionId& expectedHead,
                                  HydraulicEditDiagnostics* diagnostics) {
    if (diagnostics) *diagnostics = HydraulicEditDiagnostics{};
    if (networkUuid.isNull())
        throw StorageError(StorageErrorKind::InvalidInput, "hydraulic network UUID is null");
    if (changes.size() > 128)
        throw StorageError(StorageErrorKind::LimitExceeded, "hydraulic edit exceeds 128 changes");
    if (repository.headV2(project, branch) != expectedHead)
        throw StorageError(StorageErrorKind::Conflict, "branch head changed since edit was prepared");
    // The indexed helper validates scalar values before any indexed commit.
    // Other batches retain the complete-network validator below.
    if (!changes.empty()) {
        const std::optional<RevisionId> indexed = tryIndexedUuidScalarEdits(
            repository, project, branch, networkUuid, changes, expectedHead, diagnostics);
        if (indexed) return *indexed;
    }
    std::optional<NetworkHydraulic> network =
        loadHydraulicNetworkAtRevision(repository, project, expectedHead, networkUuid);
    if (!network) throw StorageError(StorageErrorKind::NotFound, "hydraulic network not found");
    const Manifest previous = manifestFor(*network);
    const std::map<Key, std::size_t> owners = collectionOwners(previous);
    std::set<Key> seen;
    const QByteArray before = ModelCodec<NetworkHydraulic>::encode(*network);
    for (const HydraulicEntityChange& change : changes) {
        const Key key = keyFromUuid(change.uuid);
        if (change.uuid.isNull() || change.uuid == networkUuid || !seen.insert(key).second)
            throw StorageError(StorageErrorKind::InvalidInput, "invalid or duplicate edit UUID");
        const std::map<Key, std::size_t>::const_iterator existing = owners.find(key);
        std::size_t collection = 0;
        if (change.value) {
            if (change.value->valueless_by_exception())
                throw StorageError(StorageErrorKind::InvalidInput, "invalid hydraulic entity variant");
            const QUuid payloadId = std::visit([](const auto& entity) { return entity.uuid; }, *change.value);
            if (payloadId != change.uuid)
                throw StorageError(StorageErrorKind::InvalidInput, "hydraulic payload UUID disagrees with edit UUID");
            collection = change.value->index();
            if (existing != owners.end() && existing->second != collection)
                throw StorageError(StorageErrorKind::InvalidInput, "cannot change hydraulic entity type");
            if (existing == owners.end() &&
                repository.getModelAtRevisionV2(project, expectedHead, key))
                throw StorageError(StorageErrorKind::Conflict, "entity UUID owned outside network");
        } else {
            if (existing == owners.end())
                throw StorageError(StorageErrorKind::InvalidInput, "cannot delete an unowned entity");
            collection = existing->second;
        }
        applyEntityChange(*network, change, collection);
    }
    // The save below checks every hydraulic reference and physical rule.
    // No-op edits still verify the pinned HEAD, without creating a revision.
    if (ModelCodec<NetworkHydraulic>::encode(*network) == before) {
        if (repository.headV2(project, branch) != expectedHead)
            throw StorageError(StorageErrorKind::Conflict, "branch head changed during hydraulic edit");
        return expectedHead;
    }
    return saveHydraulicNetwork(repository, project, branch, *network, expectedHead);
}

} // namespace aowis::dvmdb::hydraulic
