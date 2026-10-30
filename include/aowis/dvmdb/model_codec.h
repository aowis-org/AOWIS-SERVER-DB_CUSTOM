#ifndef AOWIS_DVMDB_MODEL_CODEC_H
#define AOWIS_DVMDB_MODEL_CODEC_H

#include <aowis/model/serialization/model_codec.h>

// DVMDB's native storage adapters use the same MODEL codec directly.
// This is a namespace convenience, not a second codec or a format reader.
namespace aowis::dvmdb {
using aowis::model::ModelCodec;
}

#endif
