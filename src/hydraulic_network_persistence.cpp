#include "aowis/native/hydraulic_network_persistence.h"

#include <aowis/native/model_validation.h>

#include <QDataStream>
#include <QCryptographicHash>
#include <QHash>
#include <QIODevice>
#include <QSet>

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <map>
#include <type_traits>
#include <stdexcept>
#include <utility>

namespace aowis::native::v5 {
namespace {

// Keep the SQL hydraulic entity-kind catalog's 18 types, but use native numeric
// kind IDs for the experimental engine. The original Stage 4 junction/pipe
// values MUST remain unchanged. These are NOT public stable wire type tags.
#define AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(X) \
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
    // Populated only by manifestFor()/loadMembers(), never serialized inline.
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
    AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(CLEAR)
#undef CLEAR
}

QByteArray encodeManifest(const Manifest& manifest) {
    QByteArray output;
    QDataStream stream(&output, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    const QByteArray metadata = aowis::native::ModelCodec<NetworkHydraulic>::encode(manifest.metadata);
    if (metadata.isEmpty()) throw std::runtime_error("cannot encode hydraulic network metadata");
    stream << manifestMagic << manifestVersion << metadata;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        stream << manifest.nextChunk[index] << manifest.chunks[index];
    }
    if (stream.status() != QDataStream::Ok || output.size() > maxManifestBytes)
        throw std::runtime_error("hydraulic network manifest exceeds encoding limit");
    return output;
}

Manifest decodeManifest(const QByteArray& bytes) {
    if (bytes.size() > maxManifestBytes) throw std::runtime_error("hydraulic network manifest too large");
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0;
    quint32 version = 0;
    QByteArray metadata;
    stream >> magic >> version >> metadata;
    if (stream.status() != QDataStream::Ok || magic != manifestMagic || version != manifestVersion)
        throw std::runtime_error("unsupported hydraulic network manifest (requires fresh v2 repository)");
    QString error;
    std::optional<NetworkHydraulic> decoded = aowis::native::ModelCodec<NetworkHydraulic>::decode(metadata, &error);
    if (!decoded) throw std::runtime_error("invalid hydraulic network metadata: " + error.toStdString());
    Manifest result;
    result.metadata = std::move(*decoded);
#define CHECK_EMPTY(name, type, kind) \
    if (!result.metadata.name.isEmpty()) throw std::runtime_error("network metadata contains inline " #name);
    AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(CHECK_EMPTY)
#undef CHECK_EMPTY
    std::set<Key> seen;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        stream >> result.nextChunk[index] >> result.chunks[index];
        for (const QUuid& id : result.chunks[index]) {
            if (id.isNull() || id == result.metadata.uuid || !seen.insert(keyFromUuid(id)).second)
                throw std::runtime_error("duplicate/invalid hydraulic membership chunk ID");
        }
        if (result.chunks[index].size() > result.nextChunk[index])
            throw std::runtime_error("invalid hydraulic membership chunk serial");
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        throw std::runtime_error("truncated or trailing hydraulic manifest bytes");
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
        throw std::runtime_error("invalid membership chunk");
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << chunkMagic << chunkVersion << chunk.network << quint32(chunk.collection) << chunk.members;
    if (stream.status() != QDataStream::Ok) throw std::runtime_error("membership chunk encoding failed");
    ModelRecord record;
    record.kind = chunkKind;
    record.payload = bytesFromQByteArray(bytes);
    std::set<Key> unique;
    for (const QUuid& id : chunk.members) {
        if (id.isNull() || id == chunk.network || !unique.insert(keyFromUuid(id)).second)
            throw std::runtime_error("invalid or duplicate member in chunk");
    }
    record.references.assign(unique.begin(), unique.end());
    return record;
}

MemberChunk decodeChunk(const ModelRecord& record, const QUuid& network, std::size_t collection) {
    if (record.kind != chunkKind) throw std::runtime_error("wrong membership chunk kind");
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
        throw std::runtime_error("invalid membership chunk payload");
    if (encodeChunk(chunk).references != record.references)
        throw std::runtime_error("membership chunk reference index mismatch");
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
        throw std::runtime_error("hydraulic root/chunk references disagree");
}

void loadMembers(Repository& repository, std::string_view branch, Manifest& manifest,
                 std::optional<std::size_t> onlyCollection = std::nullopt) {
    std::set<Key> seen;
    for (std::size_t index = 0; index < collectionCount; ++index) {
        if (onlyCollection && *onlyCollection != index) continue;
        for (const QUuid& id : manifest.chunks[index]) {
            const std::optional<ModelRecord> record = repository.getModel(branch, keyFromUuid(id));
            if (!record) throw std::runtime_error("missing hydraulic membership chunk");
            const MemberChunk chunk = decodeChunk(*record, manifest.metadata.uuid, index);
            for (const QUuid& member : chunk.members) {
                if (!seen.insert(keyFromUuid(member)).second)
                    throw std::runtime_error("duplicate network member");
                manifest.members[index].append(member);
            }
        }
    }
}

std::set<Key> memberKeys(const Manifest& manifest) {
    std::set<Key> keys;
    for (const QList<QUuid>& group : manifest.members) {
        for (const QUuid& id : group) {
            const Key key = keyFromUuid(id);
            if (id.isNull() || id == manifest.metadata.uuid || !keys.insert(key).second)
                throw std::runtime_error("duplicate or network-owned hydraulic entity UUID");
        }
    }
    return keys;
}

// A root owns a chunk; a chunk owns the entity. Validate both links before
// using the reverse-reference index to establish network ownership.
std::optional<Key> ownerChunk(Repository& repository, std::string_view branch,
                              const QUuid& network, const Key& member) {
    const std::optional<ModelRecord> root = repository.getModel(branch, keyFromUuid(network));
    if (!root || root->kind != hydraulicNetworkKind) return std::nullopt;
    std::optional<Key> found;
    for (const Key& parent : repository.referencingModel(branch, member)) {
        const std::optional<ModelRecord> candidate = repository.getModel(branch, parent);
        if (!candidate || candidate->kind != chunkKind) continue;
        if (!std::binary_search(root->references.begin(), root->references.end(), parent)) continue;
        const Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
        bool declared = false;
        for (std::size_t group = 0; group < collectionCount; ++group) {
            if (manifest.chunks[group].contains(uuidFromKey(parent))) {
                const MemberChunk owned = decodeChunk(*candidate, network, group);
                if (!owned.members.contains(uuidFromKey(member)))
                    throw std::runtime_error("reverse index claims member absent from chunk");
                declared = true;
                break;
            }
        }
        if (!declared) throw std::runtime_error("root references undeclared membership chunk");
        if (found) throw std::runtime_error("entity owned by multiple membership chunks");
        found = parent;
    }
    return found;
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
QHash<QUuid, QSet<QUuid>> outgoingReferences(const NetworkHydraulic& network,
                                            const std::set<Key>& members) {
    QHash<QUuid, QSet<QUuid>> references;
    for (const aowis::native::HydraulicReference& reference : aowis::native::hydraulicReferences(network)) {
        if (reference.source_uuid.isNull() || reference.target_uuid.isNull()) continue;
        if (reference.source_uuid != network.uuid &&
            members.contains(keyFromUuid(reference.source_uuid)) &&
            members.contains(keyFromUuid(reference.target_uuid))) {
            references[reference.source_uuid].insert(reference.target_uuid);
        }
    }
    return references;
}

template<typename T>
void addEntities(const QList<T>& entities, std::uint16_t kind,
                 QList<QUuid>& ids, const QHash<QUuid, QSet<QUuid>>& references,
                 std::vector<ModelChange>& changes) {
    for (const T& entity : entities) {
        ids.append(entity.uuid);
        ModelRecord record;
        record.kind = kind;
        record.payload = bytesFromQByteArray(aowis::native::ModelCodec<T>::encode(entity));
        if (record.payload.empty()) throw std::runtime_error("cannot encode hydraulic model entity");
        const QSet<QUuid> dependencies = references.value(entity.uuid);
        for (const QUuid& dependency : dependencies) record.references.push_back(keyFromUuid(dependency));
        changes.push_back(ModelChange{keyFromUuid(entity.uuid), std::move(record)});
    }
}

template<typename T>
void restoreEntities(Repository& repository, std::string_view branch,
                     const QList<QUuid>& ids, std::uint16_t kind,
                     QList<T>& output, QHash<QUuid, QSet<QUuid>>& storedReferences) {
    for (const QUuid& uuid : ids) {
        const std::optional<ModelRecord> record = repository.getModel(branch, keyFromUuid(uuid));
        if (!record || record->kind != kind) {
            throw std::runtime_error("missing or wrong-type hydraulic entity: " + uuid.toString().toStdString());
        }
        QString error;
        std::optional<T> entity = aowis::native::ModelCodec<T>::decode(qByteArrayFromBytes(record->payload), &error);
        if (!entity || entity->uuid != uuid) {
            throw std::runtime_error("invalid hydraulic entity payload: " + uuid.toString().toStdString() +
                                     " " + error.toStdString());
        }
        for (const Key& key : record->references) storedReferences[uuid].insert(uuidFromKey(key));
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
    AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(LIST_IDS)
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
            if (found < 0) throw std::runtime_error("hydraulic entity not found for deletion"); \
            list.removeAt(found); \
        } else { \
            const type* value = std::get_if<type>(&*change.value); \
            if (value == nullptr) throw std::runtime_error("hydraulic entity kind mismatch"); \
            if (found >= 0) list[found] = *value; \
            else list.append(*value); \
        } \
        return; \
    } \
    ++ordinal;
    AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(APPLY_EDIT)
#undef APPLY_EDIT
    throw std::runtime_error("unknown hydraulic entity collection");
}

// The same canonical codec and model reference extraction as full saves.
ModelChange encodeChangedEntity(const HydraulicEntityChange& change,
                                const QHash<QUuid, QSet<QUuid>>& references) {
    ModelChange result;
    result.key = keyFromUuid(change.uuid);
    if (!change.value) return result;
    ModelRecord record;
    static_assert(std::variant_size_v<HydraulicEntity> == collectionCount);
    const std::array<std::uint16_t, collectionCount> kinds = {
#define MEMBER_KIND(name, type, kind) static_cast<std::uint16_t>(kind),
        AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(MEMBER_KIND)
#undef MEMBER_KIND
    };
    record.kind = kinds[change.value->index()];
    std::visit([&record](const auto& entity) {
        using T = std::decay_t<decltype(entity)>;
        record.payload = bytesFromQByteArray(aowis::native::ModelCodec<T>::encode(entity));
    }, *change.value);
    if (record.payload.empty()) throw std::runtime_error("cannot encode changed hydraulic entity");
    for (const QUuid& dependency : references.value(change.uuid)) {
        record.references.push_back(keyFromUuid(dependency));
    }
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
            throw std::runtime_error("branch head changed during hydraulic topology edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, pending, starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedTopologyFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

// A carefully limited indexed membership path. The canonical validator's
// relevant checks for newly added junctions/pipes/labels are UUID uniqueness
// and typed outgoing references. Map labels have no incoming dependencies in
// the canonical model or in network metadata, so independent labels may also
// be deleted. Deleting nodes/pipes is NOT safe here: the network's embedded
// report and multi-species settings may refer to them without an individual
// ModelRecord reverse-reference edge.
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

    std::set<Key> seen;
    std::map<Key, const HydraulicEntityChange*> insertions;
    std::map<Key, Key> removals; // member -> owning chunk
    for (const HydraulicEntityChange& change : changes) {
        if (change.uuid.isNull() || change.uuid == networkUuid ||
            !seen.insert(keyFromUuid(change.uuid)).second) return std::nullopt;
        const Key key = keyFromUuid(change.uuid);
        if (change.value) {
            if (change.value->valueless_by_exception() ||
                std::visit([](const auto& entity) { return entity.uuid; }, *change.value) != change.uuid)
                return std::nullopt;
            if (!std::holds_alternative<HydraulicNodeJunction>(*change.value) &&
                !std::holds_alternative<HydraulicLinkPipe>(*change.value) &&
                !std::holds_alternative<HydraulicMapLabel>(*change.value)) return std::nullopt;
            if (repository.getModel(branch, key)) return std::nullopt;
            insertions.emplace(key, &change);
        } else {
            const std::optional<ModelRecord> old = repository.getModel(branch, key);
            if (!old || old->kind != 18) return std::nullopt;
            const std::optional<Key> owning = ownerChunk(repository, branch, networkUuid, key);
            if (!owning || !manifest.chunks[static_cast<std::size_t>(HydraulicEntityType::MapLabel)]
                                .contains(uuidFromKey(*owning))) return std::nullopt;
            const std::vector<Key> incoming = repository.referencingModel(branch, key);
            if (incoming.size() != 1 || incoming.front() != *owning) return std::nullopt;
            QString error;
            const std::optional<HydraulicMapLabel> decoded =
                ModelCodec<HydraulicMapLabel>::decode(qByteArrayFromBytes(old->payload), &error);
            if (!decoded || decoded->uuid != change.uuid ||
                old->references != scalarReferences(HydraulicEntity{*decoded})) return std::nullopt;
            removals.emplace(key, *owning);
        }
    }

    const auto validTarget = [&](const QUuid& id, bool material, bool pattern) -> bool {
        if (id.isNull()) return true;
        const Key key = keyFromUuid(id);
        const std::map<Key, const HydraulicEntityChange*>::const_iterator added = insertions.find(key);
        if (added != insertions.end())
            return !material && !pattern &&
                   std::holds_alternative<HydraulicNodeJunction>(*added->second->value);
        const std::optional<ModelRecord> target = repository.getModel(branch, key);
        if (!target) return false;
        if (material ? target->kind != 8 :
            pattern ? target->kind != 9 :
            (target->kind != junctionKind && target->kind != 2 && target->kind != 3)) return false;
        return ownerChunk(repository, branch, networkUuid, key).has_value();
    };
    for (const std::pair<const Key, const HydraulicEntityChange*>& entry : insertions) {
        const HydraulicEntityChange& change = *entry.second;
        if (const HydraulicNodeJunction* node = std::get_if<HydraulicNodeJunction>(&*change.value)) {
            for (const HydraulicDemand& demand : node->demands)
                if (!validTarget(demand.pattern_uuid, false, true)) return std::nullopt;
            if (!validTarget(node->quality_source.pattern_uuid, false, true)) return std::nullopt;
        } else if (const HydraulicLinkPipe* pipe = std::get_if<HydraulicLinkPipe>(&*change.value)) {
            if (!validTarget(pipe->node_uuid_from, false, false) ||
                !validTarget(pipe->node_uuid_to, false, false) ||
                !validTarget(pipe->material_uuid, true, false)) return std::nullopt;
        } else if (const HydraulicMapLabel* label = std::get_if<HydraulicMapLabel>(&*change.value)) {
            if (!validTarget(label->anchor_node_uuid, false, false)) return std::nullopt;
        } else return std::nullopt;
    }

    std::map<Key, std::optional<MemberChunk>> modifiedChunks;
    const auto mutableChunk = [&](const Key& key, std::size_t collection) -> MemberChunk& {
        std::map<Key, std::optional<MemberChunk>>::iterator found = modifiedChunks.find(key);
        if (found == modifiedChunks.end()) {
            const std::optional<ModelRecord> record = repository.getModel(branch, key);
            if (!record) throw std::runtime_error("missing chunk while editing membership");
            found = modifiedChunks.emplace(key, decodeChunk(*record, networkUuid, collection)).first;
        }
        if (!found->second) throw std::runtime_error("membership chunk removed twice");
        return *found->second;
    };

    std::vector<ModelChange> pending;
    pending.reserve(changes.size() + 24);
    static constexpr std::array<std::uint16_t, collectionCount> kinds = {
#define MEMBER_KIND(name, type, kind) static_cast<std::uint16_t>(kind),
        AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(MEMBER_KIND)
#undef MEMBER_KIND
    };
    for (const HydraulicEntityChange& change : changes) {
        const Key key = keyFromUuid(change.uuid);
        if (change.value) {
            const std::size_t collection = change.value->index();
            QList<QUuid>& ids = manifest.chunks[collection];
            if (ids.isEmpty() || mutableChunk(keyFromUuid(ids.last()), collection).members.size() >= chunkCapacity) {
                if (manifest.nextChunk[collection] == std::numeric_limits<quint32>::max())
                    throw std::runtime_error("hydraulic chunk serial exhausted");
                const QUuid id = chunkUuid(networkUuid, collection, manifest.nextChunk[collection]++);
                if (repository.getModel(branch, keyFromUuid(id)) || modifiedChunks.contains(keyFromUuid(id)))
                    throw std::runtime_error("hydraulic membership chunk ID collision");
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
            if (record.payload.empty()) throw std::runtime_error("cannot encode inserted entity");
            record.references = scalarReferences(*change.value);
            pending.push_back(ModelChange{key, std::move(record)});
        } else {
            const std::size_t collection = static_cast<std::size_t>(HydraulicEntityType::MapLabel);
            const Key owner = removals.at(key);
            MemberChunk& chunk = mutableChunk(owner, collection);
            if (chunk.members.removeAll(change.uuid) != 1)
                throw std::runtime_error("membership label missing from declared chunk");
            if (chunk.members.isEmpty()) {
                modifiedChunks[owner] = std::nullopt;
                if (!manifest.chunks[collection].removeOne(uuidFromKey(owner)))
                    throw std::runtime_error("membership chunk not declared by manifest");
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
    const Revision result = repository.commitModel(branch, pending, starting.id);
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
            throw std::runtime_error("branch head changed during hydraulic scalar edit");
        result = starting;
    } else {
        result = repository.commitModel(branch, pending, starting.id);
    }
    if (diagnostics) {
        diagnostics->indexedScalarFastPath = true;
        diagnostics->validatedEntities = changes.size();
    }
    return result;
}

} // namespace

std::optional<QList<QUuid>> listHydraulicEntityIds(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    HydraulicEntityType type, QString* error) {
    try {
        const std::size_t index = static_cast<std::size_t>(type);
        if (index >= collectionCount) throw std::runtime_error("unknown hydraulic entity collection");
        const Revision beginning = repository.head(branch);
        const std::optional<ModelRecord> root = repository.getModel(branch, keyFromUuid(networkUuid));
        if (!root) {
            if (repository.head(branch).id != beginning.id)
                throw std::runtime_error("branch head changed during hydraulic entity listing");
            if (error) error->clear();
            return std::nullopt;
        }
        if (root->kind != hydraulicNetworkKind)
            throw std::runtime_error("requested UUID is not a hydraulic network");
        Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
        if (manifest.metadata.uuid != networkUuid)
            throw std::runtime_error("hydraulic network root UUID mismatch");
        checkRoot(manifest, *root);
        loadMembers(repository, branch, manifest, index);
        if (repository.head(branch).id != beginning.id)
            throw std::runtime_error("branch head changed during hydraulic entity listing");
        if (error) error->clear();
        return manifest.members[index];
    } catch (const std::exception& failure) {
        if (error) *error = QString::fromUtf8(failure.what());
        return std::nullopt;
    }
}

std::optional<HydraulicEntity> loadHydraulicEntity(
    Repository& repository, std::string_view branch, const QUuid& networkUuid,
    const QUuid& entityUuid, QString* error) {
    try {
        if (networkUuid.isNull() || entityUuid.isNull() || networkUuid == entityUuid) {
            if (error) error->clear();
            return std::nullopt;
        }
        const Revision beginning = repository.head(branch);
        // Two-level ownership: network root -> bounded chunk -> entity.
        if (!ownerChunk(repository, branch, networkUuid, keyFromUuid(entityUuid))) {
            if (repository.head(branch).id != beginning.id)
                throw std::runtime_error("branch head changed during hydraulic entity lookup");
            if (error) error->clear();
            return std::nullopt;
        }
        const std::optional<ModelRecord> record = repository.getModel(branch, keyFromUuid(entityUuid));
        if (!record) throw std::runtime_error("owned hydraulic entity payload is missing");
        std::optional<HydraulicEntity> decoded;
        switch (record->kind) {
#define LOAD_ONE(name, type, kind) \
        case static_cast<std::uint16_t>(kind): { \
            QString decodeError; \
            std::optional<type> item = ModelCodec<type>::decode(qByteArrayFromBytes(record->payload), &decodeError); \
            if (!item || item->uuid != entityUuid) \
                throw std::runtime_error("invalid hydraulic entity payload: " + decodeError.toStdString()); \
            decoded.emplace(std::move(*item)); \
            break; \
        }
        AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(LOAD_ONE)
#undef LOAD_ONE
        default:
            throw std::runtime_error("owned hydraulic entity has unknown storage kind");
        }
        if (repository.head(branch).id != beginning.id)
            throw std::runtime_error("branch head changed during hydraulic entity lookup");
        if (error) error->clear();
        return decoded;
    } catch (const std::exception& failure) {
        if (error) *error = QString::fromUtf8(failure.what());
        return std::nullopt;
    }
}

Revision saveHydraulicNetwork(Repository& repository, std::string_view branch,
                              const NetworkHydraulic& network) {
    const aowis::native::ModelValidationReport report = aowis::native::validateHydraulicNetwork(network);
    if (!report.valid()) throw std::runtime_error("invalid hydraulic network: " + report.summary().toStdString());

    // Read a stable logical starting point. commitModel() verifies this head
    // again under its repository lock, preventing lost updates between the
    // membership read and final revision publication.
    const Revision startingRevision = repository.head(branch);
    const Hash expectedHead = startingRevision.id;
    Manifest next = manifestFor(network);
    const std::set<Key> newMembers = memberKeys(next);
    const QHash<QUuid, QSet<QUuid>> references = outgoingReferences(network, newMembers);

    // Discover the old membership first. Updates outside the old membership
    // are only valid when their UUID is not already claimed by another object.
    const Key networkKey = keyFromUuid(network.uuid);
    const std::optional<ModelRecord> currentRoot = repository.getModel(branch, networkKey);
    std::set<Key> oldMembers;
    if (currentRoot) {
        if (currentRoot->kind != hydraulicNetworkKind) {
            throw std::runtime_error("network UUID already used by another model entity");
        }
        Manifest previous = decodeManifest(qByteArrayFromBytes(currentRoot->payload));
        if (previous.metadata.uuid != network.uuid) throw std::runtime_error("network root identity mismatch");
        checkRoot(previous, *currentRoot);
        loadMembers(repository, branch, previous);
        oldMembers = memberKeys(previous);
    }
    for (const Key& key : newMembers) {
        if (startingRevision.root != Hash{} && !oldMembers.contains(key) &&
            repository.getModel(branch, key).has_value()) {
            throw std::runtime_error("hydraulic entity UUID already exists outside this network");
        }
    }

    std::vector<ModelChange> changes;
    changes.reserve(newMembers.size() + oldMembers.size() + 1);
    // IDs are already in the manifest; build canonical payload records only.
#define APPEND(name, type, kind) \
    { QList<QUuid> ignored; addEntities(network.name, static_cast<std::uint16_t>(kind), ignored, references, changes); }
    AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(APPEND)
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
    return repository.commitModel(branch, changes, expectedHead);
}


Revision editHydraulicNetwork(Repository& repository, std::string_view branch,
                              const QUuid& networkUuid,
                              const std::vector<HydraulicEntityChange>& changes,
                              HydraulicEditDiagnostics* diagnostics) {
    if (networkUuid.isNull()) throw std::runtime_error("hydraulic network UUID is null");
    if (diagnostics) *diagnostics = HydraulicEditDiagnostics{};
    const Revision startingRevision = repository.head(branch);
    if (changes.empty()) return startingRevision;
    if (const std::optional<Revision> indexed = tryIndexedScalarEdits(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }
    if (const std::optional<Revision> indexed = tryIndexedPipeRewires(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    if (const std::optional<Revision> indexed = tryIndexedMembershipEdits(
            repository, branch, networkUuid, changes, startingRevision, diagnostics)) {
        return *indexed;
    }

    // The Stage 17 loader checks all stored UUIDs, kinds, reference edges and
    // network semantics. Fail closed if the current aggregate is malformed.
    QString error;
    std::optional<NetworkHydraulic> network = loadHydraulicNetwork(repository, branch, networkUuid, &error);
    if (!network) throw std::runtime_error("cannot edit hydraulic network " +
                                           networkUuid.toString().toStdString() +
                                           ": " + (error.isEmpty() ? std::string("not found") : error.toStdString()));
    const Manifest previous = manifestFor(*network);
    const std::map<Key, std::size_t> oldOwners = collectionOwners(previous);
    std::set<Key> editKeys;

    for (const HydraulicEntityChange& change : changes) {
        const Key key = keyFromUuid(change.uuid);
        if (change.uuid.isNull() || change.uuid == networkUuid || !editKeys.insert(key).second) {
            throw std::runtime_error("null, network-owned, or duplicate hydraulic edit UUID");
        }
        const std::map<Key, std::size_t>::const_iterator existing = oldOwners.find(key);
        std::size_t collection = 0;
        if (change.value) {
            const QUuid payloadId = std::visit([](const auto& entity) { return entity.uuid; }, *change.value);
            if (payloadId != change.uuid) throw std::runtime_error("hydraulic edit UUID disagrees with payload");
            if (change.value->valueless_by_exception()) {
                throw std::runtime_error("invalid hydraulic entity variant");
            }
            collection = change.value->index();
            if (existing != oldOwners.end() && existing->second != collection) {
                throw std::runtime_error("cannot change a hydraulic entity's collection kind");
            }
            if (existing == oldOwners.end() && repository.getModel(branch, key)) {
                throw std::runtime_error("hydraulic UUID already owned outside this network");
            }
        } else {
            if (existing == oldOwners.end()) throw std::runtime_error("cannot delete an unowned hydraulic UUID");
            collection = existing->second;
        }
        applyEntityChange(*network, change, collection);
    }

    // Use the complete canonical model validator until we have a proven
    // incremental validator with identical cross-entity semantics.
    const aowis::native::ModelValidationReport report = aowis::native::validateHydraulicNetwork(*network);
    if (!report.valid()) throw std::runtime_error("invalid hydraulic edit: " + report.summary().toStdString());
    const Manifest next = manifestFor(*network);
    const std::set<Key> nextKeys = memberKeys(next);
    const QHash<QUuid, QSet<QUuid>> references = outgoingReferences(*network, nextKeys);

    std::vector<ModelChange> pending;
    pending.reserve(changes.size() + 1);
    for (const HydraulicEntityChange& change : changes) {
        ModelChange encoded = encodeChangedEntity(change, references);
        const std::optional<ModelRecord> old = repository.getModel(branch, encoded.key);
        // Avoid changing the revision for byte-for-byte unchanged records.
        if (old && encoded.record && encodeModelRecord(*old) == encodeModelRecord(*encoded.record)) continue;
        pending.push_back(std::move(encoded));
    }
    if (next.members != previous.members) {
        Manifest updated = next;
        buildChunks(updated, pending);
        const std::optional<ModelRecord> oldRoot = repository.getModel(branch, keyFromUuid(networkUuid));
        if (!oldRoot) throw std::runtime_error("missing network root during full edit");
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
            throw std::runtime_error("branch head changed during hydraulic edit");
        }
        return startingRevision;
    }
    return repository.commitModel(branch, pending, startingRevision.id);
}

std::optional<NetworkHydraulic> loadHydraulicNetwork(Repository& repository,
                                                      std::string_view branch,
                                                      const QUuid& networkUuid,
                                                      QString* error) {
    try {
        const std::optional<ModelRecord> root = repository.getModel(branch, keyFromUuid(networkUuid));
        if (!root) return std::nullopt;
        if (root->kind != hydraulicNetworkKind) throw std::runtime_error("requested UUID is not a hydraulic network");
        Manifest manifest = decodeManifest(qByteArrayFromBytes(root->payload));
        if (manifest.metadata.uuid != networkUuid) throw std::runtime_error("hydraulic network root UUID mismatch");
        checkRoot(manifest, *root);
        loadMembers(repository, branch, manifest);
        const std::set<Key> members = memberKeys(manifest);

        NetworkHydraulic network = std::move(manifest.metadata);
        QHash<QUuid, QSet<QUuid>> storedReferences;
        std::size_t ordinal = 0;
#define RESTORE(name, type, kind) \
        restoreEntities(repository, branch, manifest.members[ordinal], \
                        static_cast<std::uint16_t>(kind), network.name, storedReferences); \
        ++ordinal;
        AOWIS_NATIVE_HYDRAULIC_COLLECTIONS(RESTORE)
#undef RESTORE
        const aowis::native::ModelValidationReport report = aowis::native::validateHydraulicNetwork(network);
        if (!report.valid()) throw std::runtime_error("loaded network invalid: " + report.summary().toStdString());
        const QHash<QUuid, QSet<QUuid>> canonicalReferences = outgoingReferences(network, members);
        for (const Key& key : members) {
            const QUuid id = uuidFromKey(key);
            if (canonicalReferences.value(id) != storedReferences.value(id)) {
                throw std::runtime_error("stored hydraulic references differ from canonical model payload");
            }
        }
        if (error) error->clear();
        return network;
    } catch (const std::exception& failure) {
        if (error) *error = QString::fromUtf8(failure.what());
        return std::nullopt;
    }
}

} // namespace aowis::native::v5
