#include "aowis/native/model_codec_adapter.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <limits>

namespace aowis::native::hydraulic {

Key keyFromUuid(const QUuid& uuid) {
    const QByteArray raw = uuid.toRfc4122();
    if (raw.size() != 16 || uuid.isNull()) {
        throw std::invalid_argument("AOWIS model record requires a non-null UUID");
    }
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(static_cast<unsigned char>(raw.at(static_cast<qsizetype>(i))));
    }
    return key;
}

QUuid uuidFromKey(const Key& key) {
    const QByteArray raw(reinterpret_cast<const char*>(key.data()),
                         static_cast<qsizetype>(key.size()));
    return QUuid::fromRfc4122(raw);
}

Bytes bytesFromQByteArray(const QByteArray& data) {
    Bytes result;
    result.reserve(static_cast<std::size_t>(data.size()));
    for (const char byte : data) {
        result.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }
    return result;
}

QByteArray qByteArrayFromBytes(const Bytes& data) {
    if (data.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())) {
        throw std::length_error("canonical model payload too large");
    }
    return QByteArray(reinterpret_cast<const char*>(data.data()),
                      static_cast<qsizetype>(data.size()));
}

ModelChange changeFor(const HydraulicNodeJunction& junction) {
    ModelRecord record;
    record.kind = junctionKind;
    record.payload = bytesFromQByteArray(aowis::native::ModelCodec<HydraulicNodeJunction>::encode(junction));
    return ModelChange{keyFromUuid(junction.uuid), std::move(record)};
}

ModelChange changeFor(const HydraulicLinkPipe& pipe) {
    ModelRecord record;
    record.kind = pipeKind;
    record.references.push_back(keyFromUuid(pipe.node_uuid_from));
    record.references.push_back(keyFromUuid(pipe.node_uuid_to));
    record.payload = bytesFromQByteArray(aowis::native::ModelCodec<HydraulicLinkPipe>::encode(pipe));
    return ModelChange{keyFromUuid(pipe.uuid), std::move(record)};
}

std::optional<HydraulicNodeJunction> readJunction(Repository& store,
                                                 std::string_view branch, const Key& key,
                                                 QString* error) {
    const std::optional<ModelRecord> record = store.getModel(branch, key);
    if (!record) return std::nullopt;
    if (record->kind != junctionKind) {
        if (error) *error = QStringLiteral("Model record kind is not a junction");
        return std::nullopt;
    }
    std::optional<HydraulicNodeJunction> result =
        aowis::native::ModelCodec<HydraulicNodeJunction>::decode(qByteArrayFromBytes(record->payload), error);
    if (result && result->uuid != uuidFromKey(key)) {
        if (error) *error = QStringLiteral("Model record UUID differs from its tree key");
        return std::nullopt;
    }
    return result;
}

std::optional<HydraulicLinkPipe> readPipe(Repository& store,
                                          std::string_view branch, const Key& key,
                                          QString* error) {
    const std::optional<ModelRecord> record = store.getModel(branch, key);
    if (!record) return std::nullopt;
    if (record->kind != pipeKind) {
        if (error) *error = QStringLiteral("Model record kind is not a pipe");
        return std::nullopt;
    }
    std::optional<HydraulicLinkPipe> result =
        aowis::native::ModelCodec<HydraulicLinkPipe>::decode(qByteArrayFromBytes(record->payload), error);
    if (result) {
        if (result->uuid != uuidFromKey(key) ||
            result->node_uuid_from.isNull() || result->node_uuid_to.isNull()) {
            if (error) *error = QStringLiteral("Pipe identity or endpoints invalid");
            return std::nullopt;
        }
        std::vector<Key> expected{keyFromUuid(result->node_uuid_from),
                                  keyFromUuid(result->node_uuid_to)};
        std::sort(expected.begin(), expected.end());
        expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        if (record->references != expected) {
            if (error) *error = QStringLiteral("Pipe endpoint references differ from canonical model payload");
            return std::nullopt;
        }
    }
    return result;
}

}  // namespace aowis::native::hydraulic
