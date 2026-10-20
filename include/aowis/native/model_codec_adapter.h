#pragma once

// Optional integration with the existing native V5 codec. This header is not
// included by native_store.h: the core engine has no Qt or SQL dependency.
#include "aowis/native/native_store.h"
#include <aowis/native/model_codec.h>

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <optional>
#include <stdexcept>

namespace aowis::native::v5 {

// These local storage-kind values correspond to the Stage 3 contract, not to
// a finalized global entity catalog. The codec bytes are exactly native V5.
inline constexpr std::uint16_t junctionKind = 1;
inline constexpr std::uint16_t pipeKind = 4;

Key keyFromUuid(const QUuid& uuid);
QUuid uuidFromKey(const Key& key);
Bytes bytesFromQByteArray(const QByteArray& data);
QByteArray qByteArrayFromBytes(const Bytes& data);
ModelChange changeFor(const HydraulicNodeJunction& junction);
ModelChange changeFor(const HydraulicLinkPipe& pipe);
std::optional<HydraulicNodeJunction> readJunction(Repository& store,
                                                 std::string_view branch, const Key& key,
                                                 QString* error = nullptr);
std::optional<HydraulicLinkPipe> readPipe(Repository& store,
                                          std::string_view branch, const Key& key,
                                          QString* error = nullptr);

}  // namespace aowis::native::v5
