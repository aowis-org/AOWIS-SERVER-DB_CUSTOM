#ifndef AOWIS_DVMDB_MODEL_VALIDATION_H
#define AOWIS_DVMDB_MODEL_VALIDATION_H

#include <aowis/model/hydraulic/network_hydraulic.h>

#include <QString>
#include <QUuid>
#include <QVector>

namespace aowis::dvmdb
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

// Compact dependency edges for persistence. Unlike HydraulicReference these
// carry no diagnostic field strings, and are produced during the *same*
// canonical validation pass. Keep hydraulicReferences() for diagnostic users.
struct HydraulicDependency
{
    QUuid source_uuid;
    QUuid target_uuid;
};

struct HydraulicDependencyValidation
{
    ModelValidationReport report;
    QVector<HydraulicDependency> dependencies;
};

HydraulicDependencyValidation validateHydraulicNetworkWithDependencies(
    const NetworkHydraulic &network);

// Validates present (non-null) UUID references and graph relationships, and
// the physical value rules below. Deliberately permits NULL references while
// constructing incomplete drafts. Does not perform hydraulic solver
// feasibility or attachment file validation.
ModelValidationReport validateHydraulicNetwork(const NetworkHydraulic &network);

// Physical value rules (docs/model-value-rules.md): every number finite,
// plus per-field ranges. Impossible values are rejected (negative lengths,
// efficiencies above 100 %, inverted tank levels, unordered curves); zero or
// empty "not yet set" values of a draft are accepted. Part of every
// validateHydraulicNetwork* result; the entity overloads check one edited
// entity before any edit path stores it.
ModelValidationReport validateHydraulicValues(const NetworkHydraulic &network);
ModelValidationReport validateHydraulicEntityValues(const HydraulicPipeMaterial &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicPatternTime &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveTankVolume &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurvePumpHead &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurvePumpEfficiency &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveValveHeadloss &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveValveCharacteristic &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveGeneric &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeReservoir &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeJunction &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeTank &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicDemandPoint &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkPipe &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkPump &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkValve &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicControlSimple &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicControlRule &entity);
ModelValidationReport validateHydraulicEntityValues(const HydraulicMapLabel &entity);

} // namespace aowis::dvmdb
#endif // AOWIS_DVMDB_MODEL_VALIDATION_H
