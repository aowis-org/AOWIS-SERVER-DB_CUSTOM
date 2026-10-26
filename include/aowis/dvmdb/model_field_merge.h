#ifndef AOWIS_DVMDB_MODEL_FIELD_MERGE_H
#define AOWIS_DVMDB_MODEL_FIELD_MERGE_H

#include <optional>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace aowis::dvmdb
{
// Internal, schema-aware top-level field merge. A nullopt result means that
// the kind/payload cannot be safely merged and must retain entity-level
// conflict handling. EntityMetadata is merged by its seven subfields; other nested values and collections are atomic fields.
// field_choices maps an explicitly conflicting field name to whether the
// right side wins. Unknown names are rejected by the commit caller.
struct ModelFieldMerge
{
    QByteArray content; // Nonempty only when conflicting_fields is empty.
    QStringList conflicting_fields;
};

std::optional<ModelFieldMerge> mergeModelFields(const QUuid &object_uuid,
    const QString &kind, const QByteArray &base, const QByteArray &left,
    const QByteArray &right, const QMap<QString, bool> &field_choices = {});

} // namespace aowis::dvmdb
#endif // AOWIS_DVMDB_MODEL_FIELD_MERGE_H
