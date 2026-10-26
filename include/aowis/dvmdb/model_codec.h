#ifndef AOWIS_DVMDB_MODEL_CODEC_H
#define AOWIS_DVMDB_MODEL_CODEC_H

#include <optional>
#include <QByteArray>
#include <QString>
#include <aowis/model/hydraulic/network_hydraulic.h>

namespace aowis::dvmdb
{
// Stable, schema-versioned, typed encoding of AOWIS-SERVER-MODEL values.
// All fields are explicitly listed in src/model_codec.cpp; a new Model field
// requires an intentional, compatible codec update, never silent omission.
template<typename T> struct ModelCodec;
template<> struct ModelCodec<CoordinateWGS84> final
{
    static QByteArray encode(const CoordinateWGS84 &value);
    static std::optional<CoordinateWGS84> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<EntityMetadata> final
{
    static QByteArray encode(const EntityMetadata &value);
    static std::optional<EntityMetadata> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicControlLinkSetting> final
{
    static QByteArray encode(const HydraulicControlLinkSetting &value);
    static std::optional<HydraulicControlLinkSetting> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicControlRule> final
{
    static QByteArray encode(const HydraulicControlRule &value);
    static std::optional<HydraulicControlRule> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicControlRuleAction> final
{
    static QByteArray encode(const HydraulicControlRuleAction &value);
    static std::optional<HydraulicControlRuleAction> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicControlRulePremise> final
{
    static QByteArray encode(const HydraulicControlRulePremise &value);
    static std::optional<HydraulicControlRulePremise> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicControlSimple> final
{
    static QByteArray encode(const HydraulicControlSimple &value);
    static std::optional<HydraulicControlSimple> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveGeneric> final
{
    static QByteArray encode(const HydraulicCurveGeneric &value);
    static std::optional<HydraulicCurveGeneric> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveGenericPoint> final
{
    static QByteArray encode(const HydraulicCurveGenericPoint &value);
    static std::optional<HydraulicCurveGenericPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurvePumpEfficiency> final
{
    static QByteArray encode(const HydraulicCurvePumpEfficiency &value);
    static std::optional<HydraulicCurvePumpEfficiency> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurvePumpEfficiencyPoint> final
{
    static QByteArray encode(const HydraulicCurvePumpEfficiencyPoint &value);
    static std::optional<HydraulicCurvePumpEfficiencyPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurvePumpHead> final
{
    static QByteArray encode(const HydraulicCurvePumpHead &value);
    static std::optional<HydraulicCurvePumpHead> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurvePumpHeadPoint> final
{
    static QByteArray encode(const HydraulicCurvePumpHeadPoint &value);
    static std::optional<HydraulicCurvePumpHeadPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveTankVolume> final
{
    static QByteArray encode(const HydraulicCurveTankVolume &value);
    static std::optional<HydraulicCurveTankVolume> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveTankVolumePoint> final
{
    static QByteArray encode(const HydraulicCurveTankVolumePoint &value);
    static std::optional<HydraulicCurveTankVolumePoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveValveCharacteristic> final
{
    static QByteArray encode(const HydraulicCurveValveCharacteristic &value);
    static std::optional<HydraulicCurveValveCharacteristic> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveValveCharacteristicPoint> final
{
    static QByteArray encode(const HydraulicCurveValveCharacteristicPoint &value);
    static std::optional<HydraulicCurveValveCharacteristicPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveValveHeadloss> final
{
    static QByteArray encode(const HydraulicCurveValveHeadloss &value);
    static std::optional<HydraulicCurveValveHeadloss> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicCurveValveHeadlossPoint> final
{
    static QByteArray encode(const HydraulicCurveValveHeadlossPoint &value);
    static std::optional<HydraulicCurveValveHeadlossPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicDemand> final
{
    static QByteArray encode(const HydraulicDemand &value);
    static std::optional<HydraulicDemand> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicDemandPoint> final
{
    static QByteArray encode(const HydraulicDemandPoint &value);
    static std::optional<HydraulicDemandPoint> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicDemandPointAttachment> final
{
    static QByteArray encode(const HydraulicDemandPointAttachment &value);
    static std::optional<HydraulicDemandPointAttachment> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicLinkPipe> final
{
    static QByteArray encode(const HydraulicLinkPipe &value);
    static std::optional<HydraulicLinkPipe> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicLinkPump> final
{
    static QByteArray encode(const HydraulicLinkPump &value);
    static std::optional<HydraulicLinkPump> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicLinkValve> final
{
    static QByteArray encode(const HydraulicLinkValve &value);
    static std::optional<HydraulicLinkValve> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicLinkVertex> final
{
    static QByteArray encode(const HydraulicLinkVertex &value);
    static std::optional<HydraulicLinkVertex> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicMapBackdrop> final
{
    static QByteArray encode(const HydraulicMapBackdrop &value);
    static std::optional<HydraulicMapBackdrop> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicMapLabel> final
{
    static QByteArray encode(const HydraulicMapLabel &value);
    static std::optional<HydraulicMapLabel> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicNodeJunction> final
{
    static QByteArray encode(const HydraulicNodeJunction &value);
    static std::optional<HydraulicNodeJunction> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicNodeJunctionEmitter> final
{
    static QByteArray encode(const HydraulicNodeJunctionEmitter &value);
    static std::optional<HydraulicNodeJunctionEmitter> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicNodeQualitySource> final
{
    static QByteArray encode(const HydraulicNodeQualitySource &value);
    static std::optional<HydraulicNodeQualitySource> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicNodeReservoir> final
{
    static QByteArray encode(const HydraulicNodeReservoir &value);
    static std::optional<HydraulicNodeReservoir> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicNodeTank> final
{
    static QByteArray encode(const HydraulicNodeTank &value);
    static std::optional<HydraulicNodeTank> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicPatternTime> final
{
    static QByteArray encode(const HydraulicPatternTime &value);
    static std::optional<HydraulicPatternTime> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicPipeMaterial> final
{
    static QByteArray encode(const HydraulicPipeMaterial &value);
    static std::optional<HydraulicPipeMaterial> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicPipeMaterialRoughnessAtAge> final
{
    static QByteArray encode(const HydraulicPipeMaterialRoughnessAtAge &value);
    static std::optional<HydraulicPipeMaterialRoughnessAtAge> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportField> final
{
    static QByteArray encode(const HydraulicSimulationReportField &value);
    static std::optional<HydraulicSimulationReportField> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldFrictionFactor> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldFrictionFactor &value);
    static std::optional<HydraulicSimulationReportFieldFrictionFactor> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldM> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldM &value);
    static std::optional<HydraulicSimulationReportFieldM> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldM3PerH> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldM3PerH &value);
    static std::optional<HydraulicSimulationReportFieldM3PerH> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldMPerKm> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldMPerKm &value);
    static std::optional<HydraulicSimulationReportFieldMPerKm> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldMPerS> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldMPerS &value);
    static std::optional<HydraulicSimulationReportFieldMPerS> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportFieldMm> final
{
    static QByteArray encode(const HydraulicSimulationReportFieldMm &value);
    static std::optional<HydraulicSimulationReportFieldMm> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportLinkFields> final
{
    static QByteArray encode(const HydraulicSimulationReportLinkFields &value);
    static std::optional<HydraulicSimulationReportLinkFields> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportNodeFields> final
{
    static QByteArray encode(const HydraulicSimulationReportNodeFields &value);
    static std::optional<HydraulicSimulationReportNodeFields> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportOptions> final
{
    static QByteArray encode(const HydraulicSimulationReportOptions &value);
    static std::optional<HydraulicSimulationReportOptions> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSimulationReportSelection> final
{
    static QByteArray encode(const HydraulicSimulationReportSelection &value);
    static std::optional<HydraulicSimulationReportSelection> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<HydraulicSolverOptions> final
{
    static QByteArray encode(const HydraulicSolverOptions &value);
    static std::optional<HydraulicSolverOptions> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesConstant> final
{
    static QByteArray encode(const MultiSpeciesConstant &value);
    static std::optional<MultiSpeciesConstant> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesGlobalInitialQuality> final
{
    static QByteArray encode(const MultiSpeciesGlobalInitialQuality &value);
    static std::optional<MultiSpeciesGlobalInitialQuality> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesNodeInitialQuality> final
{
    static QByteArray encode(const MultiSpeciesNodeInitialQuality &value);
    static std::optional<MultiSpeciesNodeInitialQuality> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesNodeSource> final
{
    static QByteArray encode(const MultiSpeciesNodeSource &value);
    static std::optional<MultiSpeciesNodeSource> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesOptions> final
{
    static QByteArray encode(const MultiSpeciesOptions &value);
    static std::optional<MultiSpeciesOptions> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesParameter> final
{
    static QByteArray encode(const MultiSpeciesParameter &value);
    static std::optional<MultiSpeciesParameter> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesParameterOverridePipe> final
{
    static QByteArray encode(const MultiSpeciesParameterOverridePipe &value);
    static std::optional<MultiSpeciesParameterOverridePipe> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesParameterOverrideTank> final
{
    static QByteArray encode(const MultiSpeciesParameterOverrideTank &value);
    static std::optional<MultiSpeciesParameterOverrideTank> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesPattern> final
{
    static QByteArray encode(const MultiSpeciesPattern &value);
    static std::optional<MultiSpeciesPattern> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesPipeInitialQuality> final
{
    static QByteArray encode(const MultiSpeciesPipeInitialQuality &value);
    static std::optional<MultiSpeciesPipeInitialQuality> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesReaction> final
{
    static QByteArray encode(const MultiSpeciesReaction &value);
    static std::optional<MultiSpeciesReaction> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesSpecies> final
{
    static QByteArray encode(const MultiSpeciesSpecies &value);
    static std::optional<MultiSpeciesSpecies> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<MultiSpeciesTerm> final
{
    static QByteArray encode(const MultiSpeciesTerm &value);
    static std::optional<MultiSpeciesTerm> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<NetworkHydraulic> final
{
    static QByteArray encode(const NetworkHydraulic &value);
    static std::optional<NetworkHydraulic> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<NetworkMultiSpecies> final
{
    static QByteArray encode(const NetworkMultiSpecies &value);
    static std::optional<NetworkMultiSpecies> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<PumpEnergyOptions> final
{
    static QByteArray encode(const PumpEnergyOptions &value);
    static std::optional<PumpEnergyOptions> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<WaterMeter> final
{
    static QByteArray encode(const WaterMeter &value);
    static std::optional<WaterMeter> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<WaterQualityBulkReaction> final
{
    static QByteArray encode(const WaterQualityBulkReaction &value);
    static std::optional<WaterQualityBulkReaction> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<WaterQualityReactionOptions> final
{
    static QByteArray encode(const WaterQualityReactionOptions &value);
    static std::optional<WaterQualityReactionOptions> decode(const QByteArray &bytes, QString *error = nullptr);
};
template<> struct ModelCodec<WaterQualityWallReaction> final
{
    static QByteArray encode(const WaterQualityWallReaction &value);
    static std::optional<WaterQualityWallReaction> decode(const QByteArray &bytes, QString *error = nullptr);
};

} // namespace aowis::dvmdb
#endif // AOWIS_DVMDB_MODEL_CODEC_H
