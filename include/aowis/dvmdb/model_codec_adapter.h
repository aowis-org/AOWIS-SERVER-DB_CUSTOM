#pragma once

// Hydraulic entity bridge for the native-owned model serializer.
// This header is not included by native_store.h: the core storage engine
// has no Qt dependency.
#include "aowis/dvmdb/native_store.h"
#include <aowis/dvmdb/model_codec.h>

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <optional>
#include <stdexcept>

namespace aowis::dvmdb::hydraulic {

// These local storage-kind values follow the native model-record contract, not
// a finalized global entity catalog. Payloads use ModelCodec<T>.
inline constexpr std::uint16_t junctionKind = 1;
inline constexpr std::uint16_t pipeKind = 4;

Key keyFromUuid(const QUuid& uuid);
QUuid uuidFromKey(const Key& key);
Bytes bytesFromQByteArray(const QByteArray& data);
QByteArray qByteArrayFromBytes(const Bytes& data);
ModelChange changeFor(const HydraulicNodeJunction& junction);
ModelChange changeFor(const HydraulicLinkPipe& pipe);
// Typed reads of one model record at an immutable revision of `project`.
std::optional<HydraulicNodeJunction> readJunction(Repository& store, const ProjectId& project,
                                                 const RevisionId& revision, const Key& key,
                                                 QString* error = nullptr);
std::optional<HydraulicLinkPipe> readPipe(Repository& store, const ProjectId& project,
                                          const RevisionId& revision, const Key& key,
                                          QString* error = nullptr);

}  // namespace aowis::dvmdb::hydraulic
