#ifndef AOWIS_NATIVE_MODEL_VALIDATION_H
#define AOWIS_NATIVE_MODEL_VALIDATION_H

#include <aowis/model/hydraulic/network_hydraulic.h>

#include <QString>
#include <QUuid>
#include <QVector>

namespace aowis::native
{
struct ModelValidationIssue
{
    QString path;
    QString description;
};

struct ModelValidationReport
{
    QVector<ModelValidationIssue> issues;
    bool valid() const { return issues.isEmpty(); }
    QString summary() const;
};

// Derived, non-authoritative reverse-reference entries. One source is a
// persisted model entity UUID, or the network UUID for root-level options/MSX.
struct HydraulicReference
{
    QUuid source_uuid;
    QUuid target_uuid;
    QString field;
};

QVector<HydraulicReference> hydraulicReferences(const NetworkHydraulic &network);

// Validates present (non-null) UUID references and graph relationships.
// Deliberately permits NULL references while constructing incomplete drafts.
// Does not perform hydraulic solver feasibility or attachment file validation.
ModelValidationReport validateHydraulicNetwork(const NetworkHydraulic &network);

} // namespace aowis::native
#endif // AOWIS_NATIVE_MODEL_VALIDATION_H
