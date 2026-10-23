#include <aowis/native/model_codec.h>
#include <aowis/native/model_field_merge.h>

#include <QDataStream>
#include <QIODevice>
#include <QList>
#include <QSet>
#include <QDate>
#include <QStringList>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace aowis::native
{
namespace
{
constexpr quint32 codec_magic = 0x414d4435; // AMD5
constexpr quint32 codec_format = 1;
constexpr quint32 maximum_list_length = 1000000;

void writeStruct(QDataStream &stream, const CoordinateWGS84 &value);
void readStruct(QDataStream &stream, CoordinateWGS84 &value);
void writeStruct(QDataStream &stream, const EntityMetadata &value);
void readStruct(QDataStream &stream, EntityMetadata &value);
void writeStruct(QDataStream &stream, const HydraulicControlLinkSetting &value);
void readStruct(QDataStream &stream, HydraulicControlLinkSetting &value);
void writeStruct(QDataStream &stream, const HydraulicControlRule &value);
void readStruct(QDataStream &stream, HydraulicControlRule &value);
void writeStruct(QDataStream &stream, const HydraulicControlRuleAction &value);
void readStruct(QDataStream &stream, HydraulicControlRuleAction &value);
void writeStruct(QDataStream &stream, const HydraulicControlRulePremise &value);
void readStruct(QDataStream &stream, HydraulicControlRulePremise &value);
void writeStruct(QDataStream &stream, const HydraulicControlSimple &value);
void readStruct(QDataStream &stream, HydraulicControlSimple &value);
void writeStruct(QDataStream &stream, const HydraulicCurveGeneric &value);
void readStruct(QDataStream &stream, HydraulicCurveGeneric &value);
void writeStruct(QDataStream &stream, const HydraulicCurveGenericPoint &value);
void readStruct(QDataStream &stream, HydraulicCurveGenericPoint &value);
void writeStruct(QDataStream &stream, const HydraulicCurvePumpEfficiency &value);
void readStruct(QDataStream &stream, HydraulicCurvePumpEfficiency &value);
void writeStruct(QDataStream &stream, const HydraulicCurvePumpEfficiencyPoint &value);
void readStruct(QDataStream &stream, HydraulicCurvePumpEfficiencyPoint &value);
void writeStruct(QDataStream &stream, const HydraulicCurvePumpHead &value);
void readStruct(QDataStream &stream, HydraulicCurvePumpHead &value);
void writeStruct(QDataStream &stream, const HydraulicCurvePumpHeadPoint &value);
void readStruct(QDataStream &stream, HydraulicCurvePumpHeadPoint &value);
void writeStruct(QDataStream &stream, const HydraulicCurveTankVolume &value);
void readStruct(QDataStream &stream, HydraulicCurveTankVolume &value);
void writeStruct(QDataStream &stream, const HydraulicCurveTankVolumePoint &value);
void readStruct(QDataStream &stream, HydraulicCurveTankVolumePoint &value);
void writeStruct(QDataStream &stream, const HydraulicCurveValveCharacteristic &value);
void readStruct(QDataStream &stream, HydraulicCurveValveCharacteristic &value);
void writeStruct(QDataStream &stream, const HydraulicCurveValveCharacteristicPoint &value);
void readStruct(QDataStream &stream, HydraulicCurveValveCharacteristicPoint &value);
void writeStruct(QDataStream &stream, const HydraulicCurveValveHeadloss &value);
void readStruct(QDataStream &stream, HydraulicCurveValveHeadloss &value);
void writeStruct(QDataStream &stream, const HydraulicCurveValveHeadlossPoint &value);
void readStruct(QDataStream &stream, HydraulicCurveValveHeadlossPoint &value);
void writeStruct(QDataStream &stream, const HydraulicDemand &value);
void readStruct(QDataStream &stream, HydraulicDemand &value);
void writeStruct(QDataStream &stream, const HydraulicDemandPoint &value);
void readStruct(QDataStream &stream, HydraulicDemandPoint &value);
void writeStruct(QDataStream &stream, const HydraulicDemandPointAttachment &value);
void readStruct(QDataStream &stream, HydraulicDemandPointAttachment &value);
void writeStruct(QDataStream &stream, const HydraulicLinkPipe &value);
void readStruct(QDataStream &stream, HydraulicLinkPipe &value);
void writeStruct(QDataStream &stream, const HydraulicLinkPump &value);
void readStruct(QDataStream &stream, HydraulicLinkPump &value);
void writeStruct(QDataStream &stream, const HydraulicLinkValve &value);
void readStruct(QDataStream &stream, HydraulicLinkValve &value);
void writeStruct(QDataStream &stream, const HydraulicLinkVertex &value);
void readStruct(QDataStream &stream, HydraulicLinkVertex &value);
void writeStruct(QDataStream &stream, const HydraulicMapBackdrop &value);
void readStruct(QDataStream &stream, HydraulicMapBackdrop &value);
void writeStruct(QDataStream &stream, const HydraulicMapLabel &value);
void readStruct(QDataStream &stream, HydraulicMapLabel &value);
void writeStruct(QDataStream &stream, const HydraulicNodeJunction &value);
void readStruct(QDataStream &stream, HydraulicNodeJunction &value);
void writeStruct(QDataStream &stream, const HydraulicNodeJunctionEmitter &value);
void readStruct(QDataStream &stream, HydraulicNodeJunctionEmitter &value);
void writeStruct(QDataStream &stream, const HydraulicNodeQualitySource &value);
void readStruct(QDataStream &stream, HydraulicNodeQualitySource &value);
void writeStruct(QDataStream &stream, const HydraulicNodeReservoir &value);
void readStruct(QDataStream &stream, HydraulicNodeReservoir &value);
void writeStruct(QDataStream &stream, const HydraulicNodeTank &value);
void readStruct(QDataStream &stream, HydraulicNodeTank &value);
void writeStruct(QDataStream &stream, const HydraulicPatternTime &value);
void readStruct(QDataStream &stream, HydraulicPatternTime &value);
void writeStruct(QDataStream &stream, const HydraulicPipeMaterial &value);
void readStruct(QDataStream &stream, HydraulicPipeMaterial &value);
void writeStruct(QDataStream &stream, const HydraulicPipeMaterialRoughnessAtAge &value);
void readStruct(QDataStream &stream, HydraulicPipeMaterialRoughnessAtAge &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportField &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportField &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldFrictionFactor &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldFrictionFactor &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldM &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldM &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldM3PerH &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldM3PerH &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMPerKm &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMPerKm &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMPerS &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMPerS &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMm &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMm &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportLinkFields &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportLinkFields &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportNodeFields &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportNodeFields &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportOptions &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportOptions &value);
void writeStruct(QDataStream &stream, const HydraulicSimulationReportSelection &value);
void readStruct(QDataStream &stream, HydraulicSimulationReportSelection &value);
void writeStruct(QDataStream &stream, const HydraulicSolverOptions &value);
void readStruct(QDataStream &stream, HydraulicSolverOptions &value);
void writeStruct(QDataStream &stream, const MultiSpeciesConstant &value);
void readStruct(QDataStream &stream, MultiSpeciesConstant &value);
void writeStruct(QDataStream &stream, const MultiSpeciesGlobalInitialQuality &value);
void readStruct(QDataStream &stream, MultiSpeciesGlobalInitialQuality &value);
void writeStruct(QDataStream &stream, const MultiSpeciesNodeInitialQuality &value);
void readStruct(QDataStream &stream, MultiSpeciesNodeInitialQuality &value);
void writeStruct(QDataStream &stream, const MultiSpeciesNodeSource &value);
void readStruct(QDataStream &stream, MultiSpeciesNodeSource &value);
void writeStruct(QDataStream &stream, const MultiSpeciesOptions &value);
void readStruct(QDataStream &stream, MultiSpeciesOptions &value);
void writeStruct(QDataStream &stream, const MultiSpeciesParameter &value);
void readStruct(QDataStream &stream, MultiSpeciesParameter &value);
void writeStruct(QDataStream &stream, const MultiSpeciesParameterOverridePipe &value);
void readStruct(QDataStream &stream, MultiSpeciesParameterOverridePipe &value);
void writeStruct(QDataStream &stream, const MultiSpeciesParameterOverrideTank &value);
void readStruct(QDataStream &stream, MultiSpeciesParameterOverrideTank &value);
void writeStruct(QDataStream &stream, const MultiSpeciesPattern &value);
void readStruct(QDataStream &stream, MultiSpeciesPattern &value);
void writeStruct(QDataStream &stream, const MultiSpeciesPipeInitialQuality &value);
void readStruct(QDataStream &stream, MultiSpeciesPipeInitialQuality &value);
void writeStruct(QDataStream &stream, const MultiSpeciesReaction &value);
void readStruct(QDataStream &stream, MultiSpeciesReaction &value);
void writeStruct(QDataStream &stream, const MultiSpeciesSpecies &value);
void readStruct(QDataStream &stream, MultiSpeciesSpecies &value);
void writeStruct(QDataStream &stream, const MultiSpeciesTerm &value);
void readStruct(QDataStream &stream, MultiSpeciesTerm &value);
void writeStruct(QDataStream &stream, const NetworkHydraulic &value);
void readStruct(QDataStream &stream, NetworkHydraulic &value);
void writeStruct(QDataStream &stream, const NetworkMultiSpecies &value);
void readStruct(QDataStream &stream, NetworkMultiSpecies &value);
void writeStruct(QDataStream &stream, const PumpEnergyOptions &value);
void readStruct(QDataStream &stream, PumpEnergyOptions &value);
void writeStruct(QDataStream &stream, const WaterMeter &value);
void readStruct(QDataStream &stream, WaterMeter &value);
void writeStruct(QDataStream &stream, const WaterQualityBulkReaction &value);
void readStruct(QDataStream &stream, WaterQualityBulkReaction &value);
void writeStruct(QDataStream &stream, const WaterQualityReactionOptions &value);
void readStruct(QDataStream &stream, WaterQualityReactionOptions &value);
void writeStruct(QDataStream &stream, const WaterQualityWallReaction &value);
void readStruct(QDataStream &stream, WaterQualityWallReaction &value);

// Declared enumerators of every persisted enum, generated at configure time
// from the Model headers by tools/check_model_codec.py. An enum without a
// generated entry fails to compile here instead of being stored unchecked.
template<typename T> struct ModelEnumValues;
#include "model_enum_values.inc"

template<typename T> bool declaredEnumerator(qint32 raw)
{
    for (const T value : ModelEnumValues<T>::values)
        if (static_cast<qint32>(value) == raw)
            return true;
    return false;
}

template<typename T> struct IsOptional : std::false_type {};
template<typename T> struct IsOptional<std::optional<T>> : std::true_type { using Value = T; };
template<typename T> struct IsQList : std::false_type {};
template<typename T> struct IsQList<QList<T>> : std::true_type { using Value = T; };

template<typename T> void writeValue(QDataStream &stream, const T &value)
{
    if constexpr (IsOptional<T>::value)
    {
        stream << static_cast<quint8>(value.has_value() ? 1 : 0);
        if (value.has_value())
            writeValue(stream, *value);
    }
    else if constexpr (std::is_same_v<T, QStringList>)
        stream << value;
    else if constexpr (IsQList<T>::value)
    {
        stream << static_cast<quint32>(value.size());
        for (const typename IsQList<T>::Value &item : value)
            writeValue(stream, item);
    }
    else if constexpr (std::is_enum_v<T>)
    {
        // Never store a value outside the declared enumerators.
        if (!declaredEnumerator<T>(static_cast<qint32>(value)))
            stream.setStatus(QDataStream::WriteFailed);
        else
            stream << static_cast<qint32>(value);
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        // NaN and infinities are never stored.
        if (!std::isfinite(value))
            stream.setStatus(QDataStream::WriteFailed);
        else
            stream << value;
    }
    else if constexpr (std::is_same_v<T, QString> || std::is_same_v<T, QUuid> ||
                       std::is_same_v<T, QDate> || std::is_arithmetic_v<T>)
        stream << value;
    else
        writeStruct(stream, value);
}

template<typename T> void readValue(QDataStream &stream, T &value)
{
    if (stream.status() != QDataStream::Ok)
        return;
    if constexpr (IsOptional<T>::value)
    {
        quint8 flag = 0;
        stream >> flag;
        if (flag > 1)
            stream.setStatus(QDataStream::ReadCorruptData);
        else if (flag == 0)
            value.reset();
        else
        {
            typename IsOptional<T>::Value item{};
            readValue(stream, item);
            if (stream.status() == QDataStream::Ok)
                value = std::move(item);
        }
    }
    else if constexpr (std::is_same_v<T, QStringList>)
        stream >> value;
    else if constexpr (IsQList<T>::value)
    {
        quint32 count = 0;
        stream >> count;
        if (count > maximum_list_length)
        {
            stream.setStatus(QDataStream::ReadCorruptData);
            return;
        }
        T result;
        for (quint32 index = 0; index < count && stream.status() == QDataStream::Ok; ++index)
        {
            typename IsQList<T>::Value item{};
            readValue(stream, item);
            if (stream.status() == QDataStream::Ok)
                result.append(std::move(item));
        }
        if (stream.status() == QDataStream::Ok)
            value = std::move(result);
    }
    else if constexpr (std::is_enum_v<T>)
    {
        qint32 raw = 0;
        stream >> raw;
        if (stream.status() == QDataStream::Ok && !declaredEnumerator<T>(raw))
            stream.setStatus(QDataStream::ReadCorruptData);
        else
            value = static_cast<T>(raw);
    }
    else if constexpr (std::is_floating_point_v<T>)
    {
        stream >> value;
        if (stream.status() == QDataStream::Ok && !std::isfinite(value))
            stream.setStatus(QDataStream::ReadCorruptData);
    }
    else if constexpr (std::is_same_v<T, QString> || std::is_same_v<T, QUuid> ||
                       std::is_same_v<T, QDate> || std::is_arithmetic_v<T>)
        stream >> value;
    else
        readStruct(stream, value);
}

template<typename T> QByteArray encodeTyped(const T &value, const QString &type_name)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    stream << codec_magic << codec_format << type_name;
    writeStruct(stream, value);
    return stream.status() == QDataStream::Ok ? bytes : QByteArray();
}

template<typename T> std::optional<T> decodeTyped(const QByteArray &bytes,
                                                   const QString &type_name, QString *error)
{
    // QDataStream protects against truncated values. Cap external payloads
    // before allocating containers; reject trailing bytes and mismatched tags.
    if (bytes.size() > 256 * 1024 * 1024)
    {
        if (error) *error = QStringLiteral("Model payload exceeds 256 MiB limit");
        return std::nullopt;
    }
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    quint32 magic = 0;
    quint32 format = 0;
    QString stored_type;
    stream >> magic >> format >> stored_type;
    if (stream.status() != QDataStream::Ok || magic != codec_magic ||
        format != codec_format || stored_type != type_name)
    {
        if (error) *error = QStringLiteral("Invalid model type or unsupported codec format");
        return std::nullopt;
    }
    T value{};
    readStruct(stream, value);
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
    {
        if (error) *error = QStringLiteral("Truncated, corrupted or extended model data, or a non-finite number or undeclared enumerator");
        return std::nullopt;
    }
    return value;
}

void writeStruct(QDataStream &stream, const CoordinateWGS84 &value)
{
    writeValue(stream, value.longitude_deg);
    writeValue(stream, value.latitude_deg);
    writeValue(stream, value.altitude_m);
}
void readStruct(QDataStream &stream, CoordinateWGS84 &value)
{
    readValue(stream, value.longitude_deg);
    readValue(stream, value.latitude_deg);
    readValue(stream, value.altitude_m);
}

void writeStruct(QDataStream &stream, const EntityMetadata &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.model_role);
    writeValue(stream, value.date_added);
    writeValue(stream, value.date_installed);
    writeValue(stream, value.description);
    writeValue(stream, value.comment);
    writeValue(stream, value.tags);
}
void readStruct(QDataStream &stream, EntityMetadata &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.model_role);
    readValue(stream, value.date_added);
    readValue(stream, value.date_installed);
    readValue(stream, value.description);
    readValue(stream, value.comment);
    readValue(stream, value.tags);
}

void writeStruct(QDataStream &stream, const HydraulicControlLinkSetting &value)
{
    writeValue(stream, value.pump_speed_ratio);
    writeValue(stream, value.valve_pressure_head_m);
    writeValue(stream, value.valve_flow_m3_per_h);
    writeValue(stream, value.valve_loss_coefficient);
    writeValue(stream, value.valve_position_percent);
}
void readStruct(QDataStream &stream, HydraulicControlLinkSetting &value)
{
    readValue(stream, value.pump_speed_ratio);
    readValue(stream, value.valve_pressure_head_m);
    readValue(stream, value.valve_flow_m3_per_h);
    readValue(stream, value.valve_loss_coefficient);
    readValue(stream, value.valve_position_percent);
}

void writeStruct(QDataStream &stream, const HydraulicControlRule &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.premises);
    writeValue(stream, value.actions_then);
    writeValue(stream, value.actions_else);
    writeValue(stream, value.priority);
    writeValue(stream, value.enabled);
}
void readStruct(QDataStream &stream, HydraulicControlRule &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.premises);
    readValue(stream, value.actions_then);
    readValue(stream, value.actions_else);
    readValue(stream, value.priority);
    readValue(stream, value.enabled);
}

void writeStruct(QDataStream &stream, const HydraulicControlRuleAction &value)
{
    writeValue(stream, value.link_uuid);
    writeValue(stream, value.status);
    writeValue(stream, value.setting);
}
void readStruct(QDataStream &stream, HydraulicControlRuleAction &value)
{
    readValue(stream, value.link_uuid);
    readValue(stream, value.status);
    readValue(stream, value.setting);
}

void writeStruct(QDataStream &stream, const HydraulicControlRulePremise &value)
{
    writeValue(stream, value.logical_operator);
    writeValue(stream, value.object);
    writeValue(stream, value.object_uuid);
    writeValue(stream, value.variable);
    writeValue(stream, value.comparison);
    writeValue(stream, value.status);
    writeValue(stream, value.demand_m3_per_h);
    writeValue(stream, value.hydraulic_head_m);
    writeValue(stream, value.water_level_m);
    writeValue(stream, value.pressure_head_m);
    writeValue(stream, value.flow_m3_per_h);
    writeValue(stream, value.link_setting);
    writeValue(stream, value.power_kw);
    writeValue(stream, value.elapsed_time_s);
    writeValue(stream, value.time_of_day_s);
    writeValue(stream, value.fill_time_s);
    writeValue(stream, value.drain_time_s);
}
void readStruct(QDataStream &stream, HydraulicControlRulePremise &value)
{
    readValue(stream, value.logical_operator);
    readValue(stream, value.object);
    readValue(stream, value.object_uuid);
    readValue(stream, value.variable);
    readValue(stream, value.comparison);
    readValue(stream, value.status);
    readValue(stream, value.demand_m3_per_h);
    readValue(stream, value.hydraulic_head_m);
    readValue(stream, value.water_level_m);
    readValue(stream, value.pressure_head_m);
    readValue(stream, value.flow_m3_per_h);
    readValue(stream, value.link_setting);
    readValue(stream, value.power_kw);
    readValue(stream, value.elapsed_time_s);
    readValue(stream, value.time_of_day_s);
    readValue(stream, value.fill_time_s);
    readValue(stream, value.drain_time_s);
}

void writeStruct(QDataStream &stream, const HydraulicControlSimple &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.type);
    writeValue(stream, value.link_uuid);
    writeValue(stream, value.action);
    writeValue(stream, value.setting);
    writeValue(stream, value.trigger_node_uuid);
    writeValue(stream, value.trigger_water_level_m);
    writeValue(stream, value.trigger_pressure_head_m);
    writeValue(stream, value.trigger_elapsed_time_s);
    writeValue(stream, value.trigger_time_of_day_s);
    writeValue(stream, value.enabled);
}
void readStruct(QDataStream &stream, HydraulicControlSimple &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.type);
    readValue(stream, value.link_uuid);
    readValue(stream, value.action);
    readValue(stream, value.setting);
    readValue(stream, value.trigger_node_uuid);
    readValue(stream, value.trigger_water_level_m);
    readValue(stream, value.trigger_pressure_head_m);
    readValue(stream, value.trigger_elapsed_time_s);
    readValue(stream, value.trigger_time_of_day_s);
    readValue(stream, value.enabled);
}

void writeStruct(QDataStream &stream, const HydraulicCurveGeneric &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurveGeneric &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurveGenericPoint &value)
{
    writeValue(stream, value.x);
    writeValue(stream, value.y);
}
void readStruct(QDataStream &stream, HydraulicCurveGenericPoint &value)
{
    readValue(stream, value.x);
    readValue(stream, value.y);
}

void writeStruct(QDataStream &stream, const HydraulicCurvePumpEfficiency &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurvePumpEfficiency &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurvePumpEfficiencyPoint &value)
{
    writeValue(stream, value.flow_m3_per_h);
    writeValue(stream, value.efficiency_percent);
}
void readStruct(QDataStream &stream, HydraulicCurvePumpEfficiencyPoint &value)
{
    readValue(stream, value.flow_m3_per_h);
    readValue(stream, value.efficiency_percent);
}

void writeStruct(QDataStream &stream, const HydraulicCurvePumpHead &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurvePumpHead &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurvePumpHeadPoint &value)
{
    writeValue(stream, value.flow_m3_per_h);
    writeValue(stream, value.head_gain_m);
}
void readStruct(QDataStream &stream, HydraulicCurvePumpHeadPoint &value)
{
    readValue(stream, value.flow_m3_per_h);
    readValue(stream, value.head_gain_m);
}

void writeStruct(QDataStream &stream, const HydraulicCurveTankVolume &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurveTankVolume &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurveTankVolumePoint &value)
{
    writeValue(stream, value.water_level_m);
    writeValue(stream, value.volume_m3);
}
void readStruct(QDataStream &stream, HydraulicCurveTankVolumePoint &value)
{
    readValue(stream, value.water_level_m);
    readValue(stream, value.volume_m3);
}

void writeStruct(QDataStream &stream, const HydraulicCurveValveCharacteristic &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurveValveCharacteristic &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurveValveCharacteristicPoint &value)
{
    writeValue(stream, value.position_percent);
    writeValue(stream, value.relative_flow_percent);
}
void readStruct(QDataStream &stream, HydraulicCurveValveCharacteristicPoint &value)
{
    readValue(stream, value.position_percent);
    readValue(stream, value.relative_flow_percent);
}

void writeStruct(QDataStream &stream, const HydraulicCurveValveHeadloss &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.points);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicCurveValveHeadloss &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.points);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicCurveValveHeadlossPoint &value)
{
    writeValue(stream, value.flow_m3_per_h);
    writeValue(stream, value.head_loss_m);
}
void readStruct(QDataStream &stream, HydraulicCurveValveHeadlossPoint &value)
{
    readValue(stream, value.flow_m3_per_h);
    readValue(stream, value.head_loss_m);
}

void writeStruct(QDataStream &stream, const HydraulicDemand &value)
{
    writeValue(stream, value.category_name);
    writeValue(stream, value.base_demand_m3_per_h);
    writeValue(stream, value.pattern_mode);
    writeValue(stream, value.pattern_uuid);
    writeValue(stream, value.source_method);
    writeValue(stream, value.note);
}
void readStruct(QDataStream &stream, HydraulicDemand &value)
{
    readValue(stream, value.category_name);
    readValue(stream, value.base_demand_m3_per_h);
    readValue(stream, value.pattern_mode);
    readValue(stream, value.pattern_uuid);
    readValue(stream, value.source_method);
    readValue(stream, value.note);
}

void writeStruct(QDataStream &stream, const HydraulicDemandPoint &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.coordinate_wgs84);
    writeValue(stream, value.attachment);
    writeValue(stream, value.demands);
    writeValue(stream, value.meter);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicDemandPoint &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.coordinate_wgs84);
    readValue(stream, value.attachment);
    readValue(stream, value.demands);
    readValue(stream, value.meter);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicDemandPointAttachment &value)
{
    writeValue(stream, value.type);
    writeValue(stream, value.pipe_uuid);
    writeValue(stream, value.pipe_position);
    writeValue(stream, value.pipe_allocation_mode);
    writeValue(stream, value.pipe_assigned_junction_uuid);
    writeValue(stream, value.junction_uuid);
}
void readStruct(QDataStream &stream, HydraulicDemandPointAttachment &value)
{
    readValue(stream, value.type);
    readValue(stream, value.pipe_uuid);
    readValue(stream, value.pipe_position);
    readValue(stream, value.pipe_allocation_mode);
    readValue(stream, value.pipe_assigned_junction_uuid);
    readValue(stream, value.junction_uuid);
}

void writeStruct(QDataStream &stream, const HydraulicLinkPipe &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.node_uuid_from);
    writeValue(stream, value.node_uuid_to);
    writeValue(stream, value.vertices);
    writeValue(stream, value.length_calculated_m);
    writeValue(stream, value.length_measured_m);
    writeValue(stream, value.initial_status);
    writeValue(stream, value.diameter_mm);
    writeValue(stream, value.material_uuid);
    writeValue(stream, value.roughness_mode);
    writeValue(stream, value.roughness_hazen_williams);
    writeValue(stream, value.roughness_darcy_weisbach_mm);
    writeValue(stream, value.roughness_chezy_manning);
    writeValue(stream, value.minor_loss_coefficient);
    writeValue(stream, value.override_bulk_reaction);
    writeValue(stream, value.bulk_reaction);
    writeValue(stream, value.override_wall_reaction);
    writeValue(stream, value.wall_reaction);
    writeValue(stream, value.leak_area_mm2_per_100m);
    writeValue(stream, value.leak_area_expansion_per_pressure_head_mm2_per_m);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicLinkPipe &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.node_uuid_from);
    readValue(stream, value.node_uuid_to);
    readValue(stream, value.vertices);
    readValue(stream, value.length_calculated_m);
    readValue(stream, value.length_measured_m);
    readValue(stream, value.initial_status);
    readValue(stream, value.diameter_mm);
    readValue(stream, value.material_uuid);
    readValue(stream, value.roughness_mode);
    readValue(stream, value.roughness_hazen_williams);
    readValue(stream, value.roughness_darcy_weisbach_mm);
    readValue(stream, value.roughness_chezy_manning);
    readValue(stream, value.minor_loss_coefficient);
    readValue(stream, value.override_bulk_reaction);
    readValue(stream, value.bulk_reaction);
    readValue(stream, value.override_wall_reaction);
    readValue(stream, value.wall_reaction);
    readValue(stream, value.leak_area_mm2_per_100m);
    readValue(stream, value.leak_area_expansion_per_pressure_head_mm2_per_m);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicLinkPump &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.node_uuid_from);
    writeValue(stream, value.node_uuid_to);
    writeValue(stream, value.vertices);
    writeValue(stream, value.definition_type);
    writeValue(stream, value.constant_power_kw);
    writeValue(stream, value.head_curve_uuid);
    writeValue(stream, value.initial_speed_ratio);
    writeValue(stream, value.initial_status);
    writeValue(stream, value.speed_pattern_uuid);
    writeValue(stream, value.efficiency_input_type);
    writeValue(stream, value.constant_efficiency_percent);
    writeValue(stream, value.efficiency_curve_uuid);
    writeValue(stream, value.energy_price_input_type);
    writeValue(stream, value.energy_price_per_kw_h);
    writeValue(stream, value.price_pattern_uuid);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicLinkPump &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.node_uuid_from);
    readValue(stream, value.node_uuid_to);
    readValue(stream, value.vertices);
    readValue(stream, value.definition_type);
    readValue(stream, value.constant_power_kw);
    readValue(stream, value.head_curve_uuid);
    readValue(stream, value.initial_speed_ratio);
    readValue(stream, value.initial_status);
    readValue(stream, value.speed_pattern_uuid);
    readValue(stream, value.efficiency_input_type);
    readValue(stream, value.constant_efficiency_percent);
    readValue(stream, value.efficiency_curve_uuid);
    readValue(stream, value.energy_price_input_type);
    readValue(stream, value.energy_price_per_kw_h);
    readValue(stream, value.price_pattern_uuid);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicLinkValve &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.node_uuid_from);
    writeValue(stream, value.node_uuid_to);
    writeValue(stream, value.vertices);
    writeValue(stream, value.type);
    writeValue(stream, value.setting_pressure_head_m);
    writeValue(stream, value.setting_flow_m3_per_h);
    writeValue(stream, value.setting_loss_coefficient);
    writeValue(stream, value.setting_position_percent);
    writeValue(stream, value.head_loss_curve_uuid);
    writeValue(stream, value.characteristic_curve_uuid);
    writeValue(stream, value.initial_status);
    writeValue(stream, value.diameter_mm);
    writeValue(stream, value.minor_loss_coefficient);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicLinkValve &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.node_uuid_from);
    readValue(stream, value.node_uuid_to);
    readValue(stream, value.vertices);
    readValue(stream, value.type);
    readValue(stream, value.setting_pressure_head_m);
    readValue(stream, value.setting_flow_m3_per_h);
    readValue(stream, value.setting_loss_coefficient);
    readValue(stream, value.setting_position_percent);
    readValue(stream, value.head_loss_curve_uuid);
    readValue(stream, value.characteristic_curve_uuid);
    readValue(stream, value.initial_status);
    readValue(stream, value.diameter_mm);
    readValue(stream, value.minor_loss_coefficient);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicLinkVertex &value)
{
    writeValue(stream, value.coordinate_wgs84);
}
void readStruct(QDataStream &stream, HydraulicLinkVertex &value)
{
    readValue(stream, value.coordinate_wgs84);
}

void writeStruct(QDataStream &stream, const HydraulicMapBackdrop &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.lower_left_wgs84);
    writeValue(stream, value.upper_right_wgs84);
    writeValue(stream, value.file);
    writeValue(stream, value.offset_longitude_deg);
    writeValue(stream, value.offset_latitude_deg);
}
void readStruct(QDataStream &stream, HydraulicMapBackdrop &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.lower_left_wgs84);
    readValue(stream, value.upper_right_wgs84);
    readValue(stream, value.file);
    readValue(stream, value.offset_longitude_deg);
    readValue(stream, value.offset_latitude_deg);
}

void writeStruct(QDataStream &stream, const HydraulicMapLabel &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.coordinate_wgs84);
    writeValue(stream, value.text);
    writeValue(stream, value.anchor_node_uuid);
}
void readStruct(QDataStream &stream, HydraulicMapLabel &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.coordinate_wgs84);
    readValue(stream, value.text);
    readValue(stream, value.anchor_node_uuid);
}

void writeStruct(QDataStream &stream, const HydraulicNodeJunction &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.coordinate_wgs84);
    writeValue(stream, value.elevation_input_type);
    writeValue(stream, value.elevation_m);
    writeValue(stream, value.terrain_elevation_m);
    writeValue(stream, value.elevation_offset_m);
    writeValue(stream, value.demands);
    writeValue(stream, value.emitter);
    writeValue(stream, value.initial_chemical_concentration_mg_per_l);
    writeValue(stream, value.initial_water_age_h);
    writeValue(stream, value.quality_source);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicNodeJunction &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.coordinate_wgs84);
    readValue(stream, value.elevation_input_type);
    readValue(stream, value.elevation_m);
    readValue(stream, value.terrain_elevation_m);
    readValue(stream, value.elevation_offset_m);
    readValue(stream, value.demands);
    readValue(stream, value.emitter);
    readValue(stream, value.initial_chemical_concentration_mg_per_l);
    readValue(stream, value.initial_water_age_h);
    readValue(stream, value.quality_source);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicNodeJunctionEmitter &value)
{
    writeValue(stream, value.coefficient);
    writeValue(stream, value.pressure_exponent);
}
void readStruct(QDataStream &stream, HydraulicNodeJunctionEmitter &value)
{
    readValue(stream, value.coefficient);
    readValue(stream, value.pressure_exponent);
}

void writeStruct(QDataStream &stream, const HydraulicNodeQualitySource &value)
{
    writeValue(stream, value.type);
    writeValue(stream, value.chemical_concentration_mg_per_l);
    writeValue(stream, value.chemical_mass_flow_mg_per_min);
    writeValue(stream, value.pattern_uuid);
}
void readStruct(QDataStream &stream, HydraulicNodeQualitySource &value)
{
    readValue(stream, value.type);
    readValue(stream, value.chemical_concentration_mg_per_l);
    readValue(stream, value.chemical_mass_flow_mg_per_min);
    readValue(stream, value.pattern_uuid);
}

void writeStruct(QDataStream &stream, const HydraulicNodeReservoir &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.coordinate_wgs84);
    writeValue(stream, value.head_input_type);
    writeValue(stream, value.hydraulic_head_m);
    writeValue(stream, value.terrain_elevation_m);
    writeValue(stream, value.hydraulic_head_offset_m);
    writeValue(stream, value.head_pattern_mode);
    writeValue(stream, value.head_pattern_uuid);
    writeValue(stream, value.initial_chemical_concentration_mg_per_l);
    writeValue(stream, value.initial_water_age_h);
    writeValue(stream, value.quality_source);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicNodeReservoir &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.coordinate_wgs84);
    readValue(stream, value.head_input_type);
    readValue(stream, value.hydraulic_head_m);
    readValue(stream, value.terrain_elevation_m);
    readValue(stream, value.hydraulic_head_offset_m);
    readValue(stream, value.head_pattern_mode);
    readValue(stream, value.head_pattern_uuid);
    readValue(stream, value.initial_chemical_concentration_mg_per_l);
    readValue(stream, value.initial_water_age_h);
    readValue(stream, value.quality_source);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicNodeTank &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.coordinate_wgs84);
    writeValue(stream, value.elevation_input_type);
    writeValue(stream, value.bottom_elevation_m);
    writeValue(stream, value.terrain_elevation_m);
    writeValue(stream, value.bottom_offset_m);
    writeValue(stream, value.water_level_initial_m);
    writeValue(stream, value.water_level_minimum_m);
    writeValue(stream, value.water_level_maximum_m);
    writeValue(stream, value.geometry_input_type);
    writeValue(stream, value.diameter_m);
    writeValue(stream, value.cross_section_area_m2);
    writeValue(stream, value.volume_at_maximum_level_m3);
    writeValue(stream, value.minimum_volume_m3);
    writeValue(stream, value.volume_curve_uuid);
    writeValue(stream, value.can_overflow);
    writeValue(stream, value.initial_chemical_concentration_mg_per_l);
    writeValue(stream, value.initial_water_age_h);
    writeValue(stream, value.quality_source);
    writeValue(stream, value.mixing_model);
    writeValue(stream, value.mixing_fraction);
    writeValue(stream, value.override_bulk_reaction);
    writeValue(stream, value.bulk_reaction);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, HydraulicNodeTank &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.coordinate_wgs84);
    readValue(stream, value.elevation_input_type);
    readValue(stream, value.bottom_elevation_m);
    readValue(stream, value.terrain_elevation_m);
    readValue(stream, value.bottom_offset_m);
    readValue(stream, value.water_level_initial_m);
    readValue(stream, value.water_level_minimum_m);
    readValue(stream, value.water_level_maximum_m);
    readValue(stream, value.geometry_input_type);
    readValue(stream, value.diameter_m);
    readValue(stream, value.cross_section_area_m2);
    readValue(stream, value.volume_at_maximum_level_m3);
    readValue(stream, value.minimum_volume_m3);
    readValue(stream, value.volume_curve_uuid);
    readValue(stream, value.can_overflow);
    readValue(stream, value.initial_chemical_concentration_mg_per_l);
    readValue(stream, value.initial_water_age_h);
    readValue(stream, value.quality_source);
    readValue(stream, value.mixing_model);
    readValue(stream, value.mixing_fraction);
    readValue(stream, value.override_bulk_reaction);
    readValue(stream, value.bulk_reaction);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const HydraulicPatternTime &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.multipliers);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, HydraulicPatternTime &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.multipliers);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const HydraulicPipeMaterial &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.description);
    writeValue(stream, value.roughness_by_age);
}
void readStruct(QDataStream &stream, HydraulicPipeMaterial &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.description);
    readValue(stream, value.roughness_by_age);
}

void writeStruct(QDataStream &stream, const HydraulicPipeMaterialRoughnessAtAge &value)
{
    writeValue(stream, value.age_years);
    writeValue(stream, value.roughness_hazen_williams);
    writeValue(stream, value.roughness_darcy_weisbach_mm);
    writeValue(stream, value.roughness_chezy_manning);
}
void readStruct(QDataStream &stream, HydraulicPipeMaterialRoughnessAtAge &value)
{
    readValue(stream, value.age_years);
    readValue(stream, value.roughness_hazen_williams);
    readValue(stream, value.roughness_darcy_weisbach_mm);
    readValue(stream, value.roughness_chezy_manning);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportField &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportField &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldFrictionFactor &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_friction_factor);
    writeValue(stream, value.above_friction_factor);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldFrictionFactor &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_friction_factor);
    readValue(stream, value.above_friction_factor);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldM &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_m);
    writeValue(stream, value.above_m);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldM &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_m);
    readValue(stream, value.above_m);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldM3PerH &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_m3_per_h);
    writeValue(stream, value.above_m3_per_h);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldM3PerH &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_m3_per_h);
    readValue(stream, value.above_m3_per_h);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMPerKm &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_m_per_km);
    writeValue(stream, value.above_m_per_km);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMPerKm &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_m_per_km);
    readValue(stream, value.above_m_per_km);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMPerS &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_m_per_s);
    writeValue(stream, value.above_m_per_s);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMPerS &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_m_per_s);
    readValue(stream, value.above_m_per_s);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportFieldMm &value)
{
    writeValue(stream, value.enabled);
    writeValue(stream, value.precision);
    writeValue(stream, value.below_mm);
    writeValue(stream, value.above_mm);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportFieldMm &value)
{
    readValue(stream, value.enabled);
    readValue(stream, value.precision);
    readValue(stream, value.below_mm);
    readValue(stream, value.above_mm);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportLinkFields &value)
{
    writeValue(stream, value.length);
    writeValue(stream, value.diameter);
    writeValue(stream, value.flow);
    writeValue(stream, value.velocity);
    writeValue(stream, value.headloss);
    writeValue(stream, value.position);
    writeValue(stream, value.setting);
    writeValue(stream, value.reaction);
    writeValue(stream, value.friction);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportLinkFields &value)
{
    readValue(stream, value.length);
    readValue(stream, value.diameter);
    readValue(stream, value.flow);
    readValue(stream, value.velocity);
    readValue(stream, value.headloss);
    readValue(stream, value.position);
    readValue(stream, value.setting);
    readValue(stream, value.reaction);
    readValue(stream, value.friction);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportNodeFields &value)
{
    writeValue(stream, value.elevation);
    writeValue(stream, value.demand);
    writeValue(stream, value.head);
    writeValue(stream, value.pressure);
    writeValue(stream, value.quality);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportNodeFields &value)
{
    readValue(stream, value.elevation);
    readValue(stream, value.demand);
    readValue(stream, value.head);
    readValue(stream, value.pressure);
    readValue(stream, value.quality);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportOptions &value)
{
    writeValue(stream, value.page_size);
    writeValue(stream, value.status);
    writeValue(stream, value.summary);
    writeValue(stream, value.messages);
    writeValue(stream, value.energy);
    writeValue(stream, value.selection_nodes);
    writeValue(stream, value.selection_links);
    writeValue(stream, value.fields_node);
    writeValue(stream, value.fields_link);
    writeValue(stream, value.backend_commands);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportOptions &value)
{
    readValue(stream, value.page_size);
    readValue(stream, value.status);
    readValue(stream, value.summary);
    readValue(stream, value.messages);
    readValue(stream, value.energy);
    readValue(stream, value.selection_nodes);
    readValue(stream, value.selection_links);
    readValue(stream, value.fields_node);
    readValue(stream, value.fields_link);
    readValue(stream, value.backend_commands);
}

void writeStruct(QDataStream &stream, const HydraulicSimulationReportSelection &value)
{
    writeValue(stream, value.mode);
    writeValue(stream, value.uuids);
}
void readStruct(QDataStream &stream, HydraulicSimulationReportSelection &value)
{
    readValue(stream, value.mode);
    readValue(stream, value.uuids);
}

void writeStruct(QDataStream &stream, const HydraulicSolverOptions &value)
{
    writeValue(stream, value.headloss_formula);
    writeValue(stream, value.demand_model);
    writeValue(stream, value.minimum_pressure_head_m);
    writeValue(stream, value.required_pressure_head_m);
    writeValue(stream, value.pressure_exponent);
    writeValue(stream, value.maximum_trials);
    writeValue(stream, value.accuracy);
    writeValue(stream, value.unbalanced_action);
    writeValue(stream, value.unbalanced_extra_trials);
    writeValue(stream, value.check_frequency);
    writeValue(stream, value.maximum_check);
    writeValue(stream, value.damping_limit);
    writeValue(stream, value.maximum_head_error_m);
    writeValue(stream, value.maximum_flow_change_m3_per_h);
    writeValue(stream, value.demand_multiplier);
    writeValue(stream, value.default_demand_pattern_uuid);
    writeValue(stream, value.emitters_can_backflow);
    writeValue(stream, value.specific_gravity);
    writeValue(stream, value.relative_viscosity);
}
void readStruct(QDataStream &stream, HydraulicSolverOptions &value)
{
    readValue(stream, value.headloss_formula);
    readValue(stream, value.demand_model);
    readValue(stream, value.minimum_pressure_head_m);
    readValue(stream, value.required_pressure_head_m);
    readValue(stream, value.pressure_exponent);
    readValue(stream, value.maximum_trials);
    readValue(stream, value.accuracy);
    readValue(stream, value.unbalanced_action);
    readValue(stream, value.unbalanced_extra_trials);
    readValue(stream, value.check_frequency);
    readValue(stream, value.maximum_check);
    readValue(stream, value.damping_limit);
    readValue(stream, value.maximum_head_error_m);
    readValue(stream, value.maximum_flow_change_m3_per_h);
    readValue(stream, value.demand_multiplier);
    readValue(stream, value.default_demand_pattern_uuid);
    readValue(stream, value.emitters_can_backflow);
    readValue(stream, value.specific_gravity);
    readValue(stream, value.relative_viscosity);
}

void writeStruct(QDataStream &stream, const MultiSpeciesConstant &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesConstant &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesGlobalInitialQuality &value)
{
    writeValue(stream, value.species_uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesGlobalInitialQuality &value)
{
    readValue(stream, value.species_uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesNodeInitialQuality &value)
{
    writeValue(stream, value.node_uuid);
    writeValue(stream, value.species_uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesNodeInitialQuality &value)
{
    readValue(stream, value.node_uuid);
    readValue(stream, value.species_uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesNodeSource &value)
{
    writeValue(stream, value.node_uuid);
    writeValue(stream, value.species_uuid);
    writeValue(stream, value.type);
    writeValue(stream, value.value);
    writeValue(stream, value.pattern_uuid);
}
void readStruct(QDataStream &stream, MultiSpeciesNodeSource &value)
{
    readValue(stream, value.node_uuid);
    readValue(stream, value.species_uuid);
    readValue(stream, value.type);
    readValue(stream, value.value);
    readValue(stream, value.pattern_uuid);
}

void writeStruct(QDataStream &stream, const MultiSpeciesOptions &value)
{
    writeValue(stream, value.area_units);
    writeValue(stream, value.rate_units);
    writeValue(stream, value.solver_method);
    writeValue(stream, value.coupling_method);
    writeValue(stream, value.timestep_s);
    writeValue(stream, value.peclet_number_threshold);
    writeValue(stream, value.maximum_segments);
    writeValue(stream, value.default_absolute_tolerance);
    writeValue(stream, value.default_relative_tolerance);
}
void readStruct(QDataStream &stream, MultiSpeciesOptions &value)
{
    readValue(stream, value.area_units);
    readValue(stream, value.rate_units);
    readValue(stream, value.solver_method);
    readValue(stream, value.coupling_method);
    readValue(stream, value.timestep_s);
    readValue(stream, value.peclet_number_threshold);
    readValue(stream, value.maximum_segments);
    readValue(stream, value.default_absolute_tolerance);
    readValue(stream, value.default_relative_tolerance);
}

void writeStruct(QDataStream &stream, const MultiSpeciesParameter &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.default_value);
}
void readStruct(QDataStream &stream, MultiSpeciesParameter &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.default_value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesParameterOverridePipe &value)
{
    writeValue(stream, value.pipe_uuid);
    writeValue(stream, value.parameter_uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesParameterOverridePipe &value)
{
    readValue(stream, value.pipe_uuid);
    readValue(stream, value.parameter_uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesParameterOverrideTank &value)
{
    writeValue(stream, value.tank_uuid);
    writeValue(stream, value.parameter_uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesParameterOverrideTank &value)
{
    readValue(stream, value.tank_uuid);
    readValue(stream, value.parameter_uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesPattern &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.multipliers);
    writeValue(stream, value.comment);
}
void readStruct(QDataStream &stream, MultiSpeciesPattern &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.multipliers);
    readValue(stream, value.comment);
}

void writeStruct(QDataStream &stream, const MultiSpeciesPipeInitialQuality &value)
{
    writeValue(stream, value.pipe_uuid);
    writeValue(stream, value.species_uuid);
    writeValue(stream, value.value);
}
void readStruct(QDataStream &stream, MultiSpeciesPipeInitialQuality &value)
{
    readValue(stream, value.pipe_uuid);
    readValue(stream, value.species_uuid);
    readValue(stream, value.value);
}

void writeStruct(QDataStream &stream, const MultiSpeciesReaction &value)
{
    writeValue(stream, value.uuid);
    writeValue(stream, value.species_uuid);
    writeValue(stream, value.location);
    writeValue(stream, value.expression_type);
    writeValue(stream, value.expression);
}
void readStruct(QDataStream &stream, MultiSpeciesReaction &value)
{
    readValue(stream, value.uuid);
    readValue(stream, value.species_uuid);
    readValue(stream, value.location);
    readValue(stream, value.expression_type);
    readValue(stream, value.expression);
}

void writeStruct(QDataStream &stream, const MultiSpeciesSpecies &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.type);
    writeValue(stream, value.units);
    writeValue(stream, value.absolute_tolerance);
    writeValue(stream, value.relative_tolerance);
    writeValue(stream, value.molecular_diffusivity_m2_per_s);
    writeValue(stream, value.longitudinal_dispersion_coefficient_m2_per_s);
    writeValue(stream, value.note);
}
void readStruct(QDataStream &stream, MultiSpeciesSpecies &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.type);
    readValue(stream, value.units);
    readValue(stream, value.absolute_tolerance);
    readValue(stream, value.relative_tolerance);
    readValue(stream, value.molecular_diffusivity_m2_per_s);
    readValue(stream, value.longitudinal_dispersion_coefficient_m2_per_s);
    readValue(stream, value.note);
}

void writeStruct(QDataStream &stream, const MultiSpeciesTerm &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.expression);
}
void readStruct(QDataStream &stream, MultiSpeciesTerm &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.expression);
}

void writeStruct(QDataStream &stream, const NetworkHydraulic &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.title_line_1);
    writeValue(stream, value.title_line_2);
    writeValue(stream, value.title_line_3);
    writeValue(stream, value.duration_s);
    writeValue(stream, value.timestep_hydraulic_s);
    writeValue(stream, value.timestep_quality_s);
    writeValue(stream, value.timestep_pattern_s);
    writeValue(stream, value.start_pattern_s);
    writeValue(stream, value.timestep_report_s);
    writeValue(stream, value.start_report_s);
    writeValue(stream, value.timestep_rule_s);
    writeValue(stream, value.start_time_of_day_s);
    writeValue(stream, value.report_statistic);
    writeValue(stream, value.options_hydraulic);
    writeValue(stream, value.options_reaction);
    writeValue(stream, value.options_energy);
    writeValue(stream, value.options_report);
    writeValue(stream, value.multi_species);
    writeValue(stream, value.pipe_materials);
    writeValue(stream, value.patterns_time);
    writeValue(stream, value.curves_tank_volume);
    writeValue(stream, value.curves_pump_head);
    writeValue(stream, value.curves_pump_efficiency);
    writeValue(stream, value.curves_valve_headloss);
    writeValue(stream, value.curves_valve_characteristic);
    writeValue(stream, value.curves_generic);
    writeValue(stream, value.nodes_reservoirs);
    writeValue(stream, value.nodes_junctions);
    writeValue(stream, value.nodes_tanks);
    writeValue(stream, value.demand_points);
    writeValue(stream, value.links_pipes);
    writeValue(stream, value.links_pumps);
    writeValue(stream, value.links_valves);
    writeValue(stream, value.controls_simple);
    writeValue(stream, value.controls_rules);
    writeValue(stream, value.map_labels);
    writeValue(stream, value.map_backdrop);
}
void readStruct(QDataStream &stream, NetworkHydraulic &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.title_line_1);
    readValue(stream, value.title_line_2);
    readValue(stream, value.title_line_3);
    readValue(stream, value.duration_s);
    readValue(stream, value.timestep_hydraulic_s);
    readValue(stream, value.timestep_quality_s);
    readValue(stream, value.timestep_pattern_s);
    readValue(stream, value.start_pattern_s);
    readValue(stream, value.timestep_report_s);
    readValue(stream, value.start_report_s);
    readValue(stream, value.timestep_rule_s);
    readValue(stream, value.start_time_of_day_s);
    readValue(stream, value.report_statistic);
    readValue(stream, value.options_hydraulic);
    readValue(stream, value.options_reaction);
    readValue(stream, value.options_energy);
    readValue(stream, value.options_report);
    readValue(stream, value.multi_species);
    readValue(stream, value.pipe_materials);
    readValue(stream, value.patterns_time);
    readValue(stream, value.curves_tank_volume);
    readValue(stream, value.curves_pump_head);
    readValue(stream, value.curves_pump_efficiency);
    readValue(stream, value.curves_valve_headloss);
    readValue(stream, value.curves_valve_characteristic);
    readValue(stream, value.curves_generic);
    readValue(stream, value.nodes_reservoirs);
    readValue(stream, value.nodes_junctions);
    readValue(stream, value.nodes_tanks);
    readValue(stream, value.demand_points);
    readValue(stream, value.links_pipes);
    readValue(stream, value.links_pumps);
    readValue(stream, value.links_valves);
    readValue(stream, value.controls_simple);
    readValue(stream, value.controls_rules);
    readValue(stream, value.map_labels);
    readValue(stream, value.map_backdrop);
}

void writeStruct(QDataStream &stream, const NetworkMultiSpecies &value)
{
    writeValue(stream, value.options);
    writeValue(stream, value.species);
    writeValue(stream, value.constants);
    writeValue(stream, value.parameters);
    writeValue(stream, value.parameter_overrides_pipes);
    writeValue(stream, value.parameter_overrides_tanks);
    writeValue(stream, value.terms);
    writeValue(stream, value.reactions);
    writeValue(stream, value.patterns);
    writeValue(stream, value.initial_quality_global);
    writeValue(stream, value.initial_quality_nodes);
    writeValue(stream, value.initial_quality_pipes);
    writeValue(stream, value.sources);
}
void readStruct(QDataStream &stream, NetworkMultiSpecies &value)
{
    readValue(stream, value.options);
    readValue(stream, value.species);
    readValue(stream, value.constants);
    readValue(stream, value.parameters);
    readValue(stream, value.parameter_overrides_pipes);
    readValue(stream, value.parameter_overrides_tanks);
    readValue(stream, value.terms);
    readValue(stream, value.reactions);
    readValue(stream, value.patterns);
    readValue(stream, value.initial_quality_global);
    readValue(stream, value.initial_quality_nodes);
    readValue(stream, value.initial_quality_pipes);
    readValue(stream, value.sources);
}

void writeStruct(QDataStream &stream, const PumpEnergyOptions &value)
{
    writeValue(stream, value.currency_iso4217);
    writeValue(stream, value.global_pump_efficiency_percent);
    writeValue(stream, value.global_energy_price_per_kw_h);
    writeValue(stream, value.global_energy_price_pattern_uuid);
    writeValue(stream, value.demand_charge_per_kw);
}
void readStruct(QDataStream &stream, PumpEnergyOptions &value)
{
    readValue(stream, value.currency_iso4217);
    readValue(stream, value.global_pump_efficiency_percent);
    readValue(stream, value.global_energy_price_per_kw_h);
    readValue(stream, value.global_energy_price_pattern_uuid);
    readValue(stream, value.demand_charge_per_kw);
}

void writeStruct(QDataStream &stream, const WaterMeter &value)
{
    writeValue(stream, value.id);
    writeValue(stream, value.uuid);
    writeValue(stream, value.manufacturer);
    writeValue(stream, value.model);
    writeValue(stream, value.serial_number);
    writeValue(stream, value.metadata);
}
void readStruct(QDataStream &stream, WaterMeter &value)
{
    readValue(stream, value.id);
    readValue(stream, value.uuid);
    readValue(stream, value.manufacturer);
    readValue(stream, value.model);
    readValue(stream, value.serial_number);
    readValue(stream, value.metadata);
}

void writeStruct(QDataStream &stream, const WaterQualityBulkReaction &value)
{
    writeValue(stream, value.coefficient);
    writeValue(stream, value.order);
}
void readStruct(QDataStream &stream, WaterQualityBulkReaction &value)
{
    readValue(stream, value.coefficient);
    readValue(stream, value.order);
}

void writeStruct(QDataStream &stream, const WaterQualityReactionOptions &value)
{
    writeValue(stream, value.global_pipe_bulk_reaction);
    writeValue(stream, value.global_pipe_wall_reaction);
    writeValue(stream, value.global_tank_bulk_reaction);
    writeValue(stream, value.limiting_concentration_mg_per_l);
    writeValue(stream, value.roughness_reaction_factor);
}
void readStruct(QDataStream &stream, WaterQualityReactionOptions &value)
{
    readValue(stream, value.global_pipe_bulk_reaction);
    readValue(stream, value.global_pipe_wall_reaction);
    readValue(stream, value.global_tank_bulk_reaction);
    readValue(stream, value.limiting_concentration_mg_per_l);
    readValue(stream, value.roughness_reaction_factor);
}

void writeStruct(QDataStream &stream, const WaterQualityWallReaction &value)
{
    writeValue(stream, value.coefficient);
    writeValue(stream, value.order);
}
void readStruct(QDataStream &stream, WaterQualityWallReaction &value)
{
    readValue(stream, value.coefficient);
    readValue(stream, value.order);
}

} // namespace

QByteArray ModelCodec<CoordinateWGS84>::encode(const CoordinateWGS84 &value)
{
    return encodeTyped(value, QStringLiteral("CoordinateWGS84"));
}
std::optional<CoordinateWGS84> ModelCodec<CoordinateWGS84>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<CoordinateWGS84>(bytes, QStringLiteral("CoordinateWGS84"), error);
}
QByteArray ModelCodec<EntityMetadata>::encode(const EntityMetadata &value)
{
    return encodeTyped(value, QStringLiteral("EntityMetadata"));
}
std::optional<EntityMetadata> ModelCodec<EntityMetadata>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<EntityMetadata>(bytes, QStringLiteral("EntityMetadata"), error);
}
QByteArray ModelCodec<HydraulicControlLinkSetting>::encode(const HydraulicControlLinkSetting &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicControlLinkSetting"));
}
std::optional<HydraulicControlLinkSetting> ModelCodec<HydraulicControlLinkSetting>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicControlLinkSetting>(bytes, QStringLiteral("HydraulicControlLinkSetting"), error);
}
QByteArray ModelCodec<HydraulicControlRule>::encode(const HydraulicControlRule &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicControlRule"));
}
std::optional<HydraulicControlRule> ModelCodec<HydraulicControlRule>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicControlRule>(bytes, QStringLiteral("HydraulicControlRule"), error);
}
QByteArray ModelCodec<HydraulicControlRuleAction>::encode(const HydraulicControlRuleAction &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicControlRuleAction"));
}
std::optional<HydraulicControlRuleAction> ModelCodec<HydraulicControlRuleAction>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicControlRuleAction>(bytes, QStringLiteral("HydraulicControlRuleAction"), error);
}
QByteArray ModelCodec<HydraulicControlRulePremise>::encode(const HydraulicControlRulePremise &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicControlRulePremise"));
}
std::optional<HydraulicControlRulePremise> ModelCodec<HydraulicControlRulePremise>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicControlRulePremise>(bytes, QStringLiteral("HydraulicControlRulePremise"), error);
}
QByteArray ModelCodec<HydraulicControlSimple>::encode(const HydraulicControlSimple &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicControlSimple"));
}
std::optional<HydraulicControlSimple> ModelCodec<HydraulicControlSimple>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicControlSimple>(bytes, QStringLiteral("HydraulicControlSimple"), error);
}
QByteArray ModelCodec<HydraulicCurveGeneric>::encode(const HydraulicCurveGeneric &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveGeneric"));
}
std::optional<HydraulicCurveGeneric> ModelCodec<HydraulicCurveGeneric>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveGeneric>(bytes, QStringLiteral("HydraulicCurveGeneric"), error);
}
QByteArray ModelCodec<HydraulicCurveGenericPoint>::encode(const HydraulicCurveGenericPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveGenericPoint"));
}
std::optional<HydraulicCurveGenericPoint> ModelCodec<HydraulicCurveGenericPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveGenericPoint>(bytes, QStringLiteral("HydraulicCurveGenericPoint"), error);
}
QByteArray ModelCodec<HydraulicCurvePumpEfficiency>::encode(const HydraulicCurvePumpEfficiency &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurvePumpEfficiency"));
}
std::optional<HydraulicCurvePumpEfficiency> ModelCodec<HydraulicCurvePumpEfficiency>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurvePumpEfficiency>(bytes, QStringLiteral("HydraulicCurvePumpEfficiency"), error);
}
QByteArray ModelCodec<HydraulicCurvePumpEfficiencyPoint>::encode(const HydraulicCurvePumpEfficiencyPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurvePumpEfficiencyPoint"));
}
std::optional<HydraulicCurvePumpEfficiencyPoint> ModelCodec<HydraulicCurvePumpEfficiencyPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurvePumpEfficiencyPoint>(bytes, QStringLiteral("HydraulicCurvePumpEfficiencyPoint"), error);
}
QByteArray ModelCodec<HydraulicCurvePumpHead>::encode(const HydraulicCurvePumpHead &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurvePumpHead"));
}
std::optional<HydraulicCurvePumpHead> ModelCodec<HydraulicCurvePumpHead>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurvePumpHead>(bytes, QStringLiteral("HydraulicCurvePumpHead"), error);
}
QByteArray ModelCodec<HydraulicCurvePumpHeadPoint>::encode(const HydraulicCurvePumpHeadPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurvePumpHeadPoint"));
}
std::optional<HydraulicCurvePumpHeadPoint> ModelCodec<HydraulicCurvePumpHeadPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurvePumpHeadPoint>(bytes, QStringLiteral("HydraulicCurvePumpHeadPoint"), error);
}
QByteArray ModelCodec<HydraulicCurveTankVolume>::encode(const HydraulicCurveTankVolume &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveTankVolume"));
}
std::optional<HydraulicCurveTankVolume> ModelCodec<HydraulicCurveTankVolume>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveTankVolume>(bytes, QStringLiteral("HydraulicCurveTankVolume"), error);
}
QByteArray ModelCodec<HydraulicCurveTankVolumePoint>::encode(const HydraulicCurveTankVolumePoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveTankVolumePoint"));
}
std::optional<HydraulicCurveTankVolumePoint> ModelCodec<HydraulicCurveTankVolumePoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveTankVolumePoint>(bytes, QStringLiteral("HydraulicCurveTankVolumePoint"), error);
}
QByteArray ModelCodec<HydraulicCurveValveCharacteristic>::encode(const HydraulicCurveValveCharacteristic &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveValveCharacteristic"));
}
std::optional<HydraulicCurveValveCharacteristic> ModelCodec<HydraulicCurveValveCharacteristic>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveValveCharacteristic>(bytes, QStringLiteral("HydraulicCurveValveCharacteristic"), error);
}
QByteArray ModelCodec<HydraulicCurveValveCharacteristicPoint>::encode(const HydraulicCurveValveCharacteristicPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveValveCharacteristicPoint"));
}
std::optional<HydraulicCurveValveCharacteristicPoint> ModelCodec<HydraulicCurveValveCharacteristicPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveValveCharacteristicPoint>(bytes, QStringLiteral("HydraulicCurveValveCharacteristicPoint"), error);
}
QByteArray ModelCodec<HydraulicCurveValveHeadloss>::encode(const HydraulicCurveValveHeadloss &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveValveHeadloss"));
}
std::optional<HydraulicCurveValveHeadloss> ModelCodec<HydraulicCurveValveHeadloss>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveValveHeadloss>(bytes, QStringLiteral("HydraulicCurveValveHeadloss"), error);
}
QByteArray ModelCodec<HydraulicCurveValveHeadlossPoint>::encode(const HydraulicCurveValveHeadlossPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicCurveValveHeadlossPoint"));
}
std::optional<HydraulicCurveValveHeadlossPoint> ModelCodec<HydraulicCurveValveHeadlossPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicCurveValveHeadlossPoint>(bytes, QStringLiteral("HydraulicCurveValveHeadlossPoint"), error);
}
QByteArray ModelCodec<HydraulicDemand>::encode(const HydraulicDemand &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicDemand"));
}
std::optional<HydraulicDemand> ModelCodec<HydraulicDemand>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicDemand>(bytes, QStringLiteral("HydraulicDemand"), error);
}
QByteArray ModelCodec<HydraulicDemandPoint>::encode(const HydraulicDemandPoint &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicDemandPoint"));
}
std::optional<HydraulicDemandPoint> ModelCodec<HydraulicDemandPoint>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicDemandPoint>(bytes, QStringLiteral("HydraulicDemandPoint"), error);
}
QByteArray ModelCodec<HydraulicDemandPointAttachment>::encode(const HydraulicDemandPointAttachment &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicDemandPointAttachment"));
}
std::optional<HydraulicDemandPointAttachment> ModelCodec<HydraulicDemandPointAttachment>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicDemandPointAttachment>(bytes, QStringLiteral("HydraulicDemandPointAttachment"), error);
}
QByteArray ModelCodec<HydraulicLinkPipe>::encode(const HydraulicLinkPipe &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicLinkPipe"));
}
std::optional<HydraulicLinkPipe> ModelCodec<HydraulicLinkPipe>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicLinkPipe>(bytes, QStringLiteral("HydraulicLinkPipe"), error);
}
QByteArray ModelCodec<HydraulicLinkPump>::encode(const HydraulicLinkPump &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicLinkPump"));
}
std::optional<HydraulicLinkPump> ModelCodec<HydraulicLinkPump>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicLinkPump>(bytes, QStringLiteral("HydraulicLinkPump"), error);
}
QByteArray ModelCodec<HydraulicLinkValve>::encode(const HydraulicLinkValve &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicLinkValve"));
}
std::optional<HydraulicLinkValve> ModelCodec<HydraulicLinkValve>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicLinkValve>(bytes, QStringLiteral("HydraulicLinkValve"), error);
}
QByteArray ModelCodec<HydraulicLinkVertex>::encode(const HydraulicLinkVertex &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicLinkVertex"));
}
std::optional<HydraulicLinkVertex> ModelCodec<HydraulicLinkVertex>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicLinkVertex>(bytes, QStringLiteral("HydraulicLinkVertex"), error);
}
QByteArray ModelCodec<HydraulicMapBackdrop>::encode(const HydraulicMapBackdrop &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicMapBackdrop"));
}
std::optional<HydraulicMapBackdrop> ModelCodec<HydraulicMapBackdrop>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicMapBackdrop>(bytes, QStringLiteral("HydraulicMapBackdrop"), error);
}
QByteArray ModelCodec<HydraulicMapLabel>::encode(const HydraulicMapLabel &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicMapLabel"));
}
std::optional<HydraulicMapLabel> ModelCodec<HydraulicMapLabel>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicMapLabel>(bytes, QStringLiteral("HydraulicMapLabel"), error);
}
QByteArray ModelCodec<HydraulicNodeJunction>::encode(const HydraulicNodeJunction &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicNodeJunction"));
}
std::optional<HydraulicNodeJunction> ModelCodec<HydraulicNodeJunction>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicNodeJunction>(bytes, QStringLiteral("HydraulicNodeJunction"), error);
}
QByteArray ModelCodec<HydraulicNodeJunctionEmitter>::encode(const HydraulicNodeJunctionEmitter &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicNodeJunctionEmitter"));
}
std::optional<HydraulicNodeJunctionEmitter> ModelCodec<HydraulicNodeJunctionEmitter>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicNodeJunctionEmitter>(bytes, QStringLiteral("HydraulicNodeJunctionEmitter"), error);
}
QByteArray ModelCodec<HydraulicNodeQualitySource>::encode(const HydraulicNodeQualitySource &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicNodeQualitySource"));
}
std::optional<HydraulicNodeQualitySource> ModelCodec<HydraulicNodeQualitySource>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicNodeQualitySource>(bytes, QStringLiteral("HydraulicNodeQualitySource"), error);
}
QByteArray ModelCodec<HydraulicNodeReservoir>::encode(const HydraulicNodeReservoir &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicNodeReservoir"));
}
std::optional<HydraulicNodeReservoir> ModelCodec<HydraulicNodeReservoir>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicNodeReservoir>(bytes, QStringLiteral("HydraulicNodeReservoir"), error);
}
QByteArray ModelCodec<HydraulicNodeTank>::encode(const HydraulicNodeTank &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicNodeTank"));
}
std::optional<HydraulicNodeTank> ModelCodec<HydraulicNodeTank>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicNodeTank>(bytes, QStringLiteral("HydraulicNodeTank"), error);
}
QByteArray ModelCodec<HydraulicPatternTime>::encode(const HydraulicPatternTime &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicPatternTime"));
}
std::optional<HydraulicPatternTime> ModelCodec<HydraulicPatternTime>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicPatternTime>(bytes, QStringLiteral("HydraulicPatternTime"), error);
}
QByteArray ModelCodec<HydraulicPipeMaterial>::encode(const HydraulicPipeMaterial &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicPipeMaterial"));
}
std::optional<HydraulicPipeMaterial> ModelCodec<HydraulicPipeMaterial>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicPipeMaterial>(bytes, QStringLiteral("HydraulicPipeMaterial"), error);
}
QByteArray ModelCodec<HydraulicPipeMaterialRoughnessAtAge>::encode(const HydraulicPipeMaterialRoughnessAtAge &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicPipeMaterialRoughnessAtAge"));
}
std::optional<HydraulicPipeMaterialRoughnessAtAge> ModelCodec<HydraulicPipeMaterialRoughnessAtAge>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicPipeMaterialRoughnessAtAge>(bytes, QStringLiteral("HydraulicPipeMaterialRoughnessAtAge"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportField>::encode(const HydraulicSimulationReportField &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportField"));
}
std::optional<HydraulicSimulationReportField> ModelCodec<HydraulicSimulationReportField>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportField>(bytes, QStringLiteral("HydraulicSimulationReportField"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldFrictionFactor>::encode(const HydraulicSimulationReportFieldFrictionFactor &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldFrictionFactor"));
}
std::optional<HydraulicSimulationReportFieldFrictionFactor> ModelCodec<HydraulicSimulationReportFieldFrictionFactor>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldFrictionFactor>(bytes, QStringLiteral("HydraulicSimulationReportFieldFrictionFactor"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldM>::encode(const HydraulicSimulationReportFieldM &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldM"));
}
std::optional<HydraulicSimulationReportFieldM> ModelCodec<HydraulicSimulationReportFieldM>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldM>(bytes, QStringLiteral("HydraulicSimulationReportFieldM"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldM3PerH>::encode(const HydraulicSimulationReportFieldM3PerH &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldM3PerH"));
}
std::optional<HydraulicSimulationReportFieldM3PerH> ModelCodec<HydraulicSimulationReportFieldM3PerH>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldM3PerH>(bytes, QStringLiteral("HydraulicSimulationReportFieldM3PerH"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldMPerKm>::encode(const HydraulicSimulationReportFieldMPerKm &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldMPerKm"));
}
std::optional<HydraulicSimulationReportFieldMPerKm> ModelCodec<HydraulicSimulationReportFieldMPerKm>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldMPerKm>(bytes, QStringLiteral("HydraulicSimulationReportFieldMPerKm"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldMPerS>::encode(const HydraulicSimulationReportFieldMPerS &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldMPerS"));
}
std::optional<HydraulicSimulationReportFieldMPerS> ModelCodec<HydraulicSimulationReportFieldMPerS>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldMPerS>(bytes, QStringLiteral("HydraulicSimulationReportFieldMPerS"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportFieldMm>::encode(const HydraulicSimulationReportFieldMm &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportFieldMm"));
}
std::optional<HydraulicSimulationReportFieldMm> ModelCodec<HydraulicSimulationReportFieldMm>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportFieldMm>(bytes, QStringLiteral("HydraulicSimulationReportFieldMm"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportLinkFields>::encode(const HydraulicSimulationReportLinkFields &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportLinkFields"));
}
std::optional<HydraulicSimulationReportLinkFields> ModelCodec<HydraulicSimulationReportLinkFields>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportLinkFields>(bytes, QStringLiteral("HydraulicSimulationReportLinkFields"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportNodeFields>::encode(const HydraulicSimulationReportNodeFields &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportNodeFields"));
}
std::optional<HydraulicSimulationReportNodeFields> ModelCodec<HydraulicSimulationReportNodeFields>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportNodeFields>(bytes, QStringLiteral("HydraulicSimulationReportNodeFields"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportOptions>::encode(const HydraulicSimulationReportOptions &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportOptions"));
}
std::optional<HydraulicSimulationReportOptions> ModelCodec<HydraulicSimulationReportOptions>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportOptions>(bytes, QStringLiteral("HydraulicSimulationReportOptions"), error);
}
QByteArray ModelCodec<HydraulicSimulationReportSelection>::encode(const HydraulicSimulationReportSelection &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSimulationReportSelection"));
}
std::optional<HydraulicSimulationReportSelection> ModelCodec<HydraulicSimulationReportSelection>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSimulationReportSelection>(bytes, QStringLiteral("HydraulicSimulationReportSelection"), error);
}
QByteArray ModelCodec<HydraulicSolverOptions>::encode(const HydraulicSolverOptions &value)
{
    return encodeTyped(value, QStringLiteral("HydraulicSolverOptions"));
}
std::optional<HydraulicSolverOptions> ModelCodec<HydraulicSolverOptions>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<HydraulicSolverOptions>(bytes, QStringLiteral("HydraulicSolverOptions"), error);
}
QByteArray ModelCodec<MultiSpeciesConstant>::encode(const MultiSpeciesConstant &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesConstant"));
}
std::optional<MultiSpeciesConstant> ModelCodec<MultiSpeciesConstant>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesConstant>(bytes, QStringLiteral("MultiSpeciesConstant"), error);
}
QByteArray ModelCodec<MultiSpeciesGlobalInitialQuality>::encode(const MultiSpeciesGlobalInitialQuality &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesGlobalInitialQuality"));
}
std::optional<MultiSpeciesGlobalInitialQuality> ModelCodec<MultiSpeciesGlobalInitialQuality>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesGlobalInitialQuality>(bytes, QStringLiteral("MultiSpeciesGlobalInitialQuality"), error);
}
QByteArray ModelCodec<MultiSpeciesNodeInitialQuality>::encode(const MultiSpeciesNodeInitialQuality &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesNodeInitialQuality"));
}
std::optional<MultiSpeciesNodeInitialQuality> ModelCodec<MultiSpeciesNodeInitialQuality>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesNodeInitialQuality>(bytes, QStringLiteral("MultiSpeciesNodeInitialQuality"), error);
}
QByteArray ModelCodec<MultiSpeciesNodeSource>::encode(const MultiSpeciesNodeSource &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesNodeSource"));
}
std::optional<MultiSpeciesNodeSource> ModelCodec<MultiSpeciesNodeSource>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesNodeSource>(bytes, QStringLiteral("MultiSpeciesNodeSource"), error);
}
QByteArray ModelCodec<MultiSpeciesOptions>::encode(const MultiSpeciesOptions &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesOptions"));
}
std::optional<MultiSpeciesOptions> ModelCodec<MultiSpeciesOptions>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesOptions>(bytes, QStringLiteral("MultiSpeciesOptions"), error);
}
QByteArray ModelCodec<MultiSpeciesParameter>::encode(const MultiSpeciesParameter &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesParameter"));
}
std::optional<MultiSpeciesParameter> ModelCodec<MultiSpeciesParameter>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesParameter>(bytes, QStringLiteral("MultiSpeciesParameter"), error);
}
QByteArray ModelCodec<MultiSpeciesParameterOverridePipe>::encode(const MultiSpeciesParameterOverridePipe &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesParameterOverridePipe"));
}
std::optional<MultiSpeciesParameterOverridePipe> ModelCodec<MultiSpeciesParameterOverridePipe>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesParameterOverridePipe>(bytes, QStringLiteral("MultiSpeciesParameterOverridePipe"), error);
}
QByteArray ModelCodec<MultiSpeciesParameterOverrideTank>::encode(const MultiSpeciesParameterOverrideTank &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesParameterOverrideTank"));
}
std::optional<MultiSpeciesParameterOverrideTank> ModelCodec<MultiSpeciesParameterOverrideTank>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesParameterOverrideTank>(bytes, QStringLiteral("MultiSpeciesParameterOverrideTank"), error);
}
QByteArray ModelCodec<MultiSpeciesPattern>::encode(const MultiSpeciesPattern &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesPattern"));
}
std::optional<MultiSpeciesPattern> ModelCodec<MultiSpeciesPattern>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesPattern>(bytes, QStringLiteral("MultiSpeciesPattern"), error);
}
QByteArray ModelCodec<MultiSpeciesPipeInitialQuality>::encode(const MultiSpeciesPipeInitialQuality &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesPipeInitialQuality"));
}
std::optional<MultiSpeciesPipeInitialQuality> ModelCodec<MultiSpeciesPipeInitialQuality>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesPipeInitialQuality>(bytes, QStringLiteral("MultiSpeciesPipeInitialQuality"), error);
}
QByteArray ModelCodec<MultiSpeciesReaction>::encode(const MultiSpeciesReaction &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesReaction"));
}
std::optional<MultiSpeciesReaction> ModelCodec<MultiSpeciesReaction>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesReaction>(bytes, QStringLiteral("MultiSpeciesReaction"), error);
}
QByteArray ModelCodec<MultiSpeciesSpecies>::encode(const MultiSpeciesSpecies &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesSpecies"));
}
std::optional<MultiSpeciesSpecies> ModelCodec<MultiSpeciesSpecies>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesSpecies>(bytes, QStringLiteral("MultiSpeciesSpecies"), error);
}
QByteArray ModelCodec<MultiSpeciesTerm>::encode(const MultiSpeciesTerm &value)
{
    return encodeTyped(value, QStringLiteral("MultiSpeciesTerm"));
}
std::optional<MultiSpeciesTerm> ModelCodec<MultiSpeciesTerm>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<MultiSpeciesTerm>(bytes, QStringLiteral("MultiSpeciesTerm"), error);
}
QByteArray ModelCodec<NetworkHydraulic>::encode(const NetworkHydraulic &value)
{
    return encodeTyped(value, QStringLiteral("NetworkHydraulic"));
}
std::optional<NetworkHydraulic> ModelCodec<NetworkHydraulic>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<NetworkHydraulic>(bytes, QStringLiteral("NetworkHydraulic"), error);
}
QByteArray ModelCodec<NetworkMultiSpecies>::encode(const NetworkMultiSpecies &value)
{
    return encodeTyped(value, QStringLiteral("NetworkMultiSpecies"));
}
std::optional<NetworkMultiSpecies> ModelCodec<NetworkMultiSpecies>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<NetworkMultiSpecies>(bytes, QStringLiteral("NetworkMultiSpecies"), error);
}
QByteArray ModelCodec<PumpEnergyOptions>::encode(const PumpEnergyOptions &value)
{
    return encodeTyped(value, QStringLiteral("PumpEnergyOptions"));
}
std::optional<PumpEnergyOptions> ModelCodec<PumpEnergyOptions>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<PumpEnergyOptions>(bytes, QStringLiteral("PumpEnergyOptions"), error);
}
QByteArray ModelCodec<WaterMeter>::encode(const WaterMeter &value)
{
    return encodeTyped(value, QStringLiteral("WaterMeter"));
}
std::optional<WaterMeter> ModelCodec<WaterMeter>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<WaterMeter>(bytes, QStringLiteral("WaterMeter"), error);
}
QByteArray ModelCodec<WaterQualityBulkReaction>::encode(const WaterQualityBulkReaction &value)
{
    return encodeTyped(value, QStringLiteral("WaterQualityBulkReaction"));
}
std::optional<WaterQualityBulkReaction> ModelCodec<WaterQualityBulkReaction>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<WaterQualityBulkReaction>(bytes, QStringLiteral("WaterQualityBulkReaction"), error);
}
QByteArray ModelCodec<WaterQualityReactionOptions>::encode(const WaterQualityReactionOptions &value)
{
    return encodeTyped(value, QStringLiteral("WaterQualityReactionOptions"));
}
std::optional<WaterQualityReactionOptions> ModelCodec<WaterQualityReactionOptions>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<WaterQualityReactionOptions>(bytes, QStringLiteral("WaterQualityReactionOptions"), error);
}
QByteArray ModelCodec<WaterQualityWallReaction>::encode(const WaterQualityWallReaction &value)
{
    return encodeTyped(value, QStringLiteral("WaterQualityWallReaction"));
}
std::optional<WaterQualityWallReaction> ModelCodec<WaterQualityWallReaction>::decode(const QByteArray &bytes, QString *error)
{
    return decodeTyped<WaterQualityWallReaction>(bytes, QStringLiteral("WaterQualityWallReaction"), error);
}

// Field-wise merge comparison uses the same canonical QDataStream serialization
// primitives as ModelCodec. Never compare floats with approximate equality or
// rely on operator== for nested Model structs; compare encoded field bytes.
template<typename T> QByteArray encodedMergeField(const T &field)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::BigEndian);
    stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
    writeValue(stream, field);
    return stream.status() == QDataStream::Ok ? bytes : QByteArray();
}

// QStringList preserves order; metadata.tags is not an unordered set.
// Combine only unambiguous, disjoint *append-only* changes. Deletions,
// reordering, inserting in the middle, repeated additions and changes to
// existing tags deliberately retain the usual explicit field conflict.
std::optional<QStringList> mergeAppendedMetadataTags(const QStringList &base,
    const QStringList &left, const QStringList &right)
{
    if (left.size() <= base.size() || right.size() <= base.size())
        return std::nullopt;
    for (qsizetype i = 0; i < base.size(); ++i)
    {
        if (left.at(i) != base.at(i) || right.at(i) != base.at(i))
            return std::nullopt;
    }
    QSet<QString> observed;
    for (const QString &tag : base)
        observed.insert(tag);
    QStringList merged = left;
    for (qsizetype i = base.size(); i < left.size(); ++i)
    {
        const QString &tag = left.at(i);
        if (tag.isEmpty() || observed.contains(tag))
            return std::nullopt;
        observed.insert(tag);
    }
    for (qsizetype i = base.size(); i < right.size(); ++i)
    {
        const QString &tag = right.at(i);
        if (tag.isEmpty() || observed.contains(tag))
            return std::nullopt;
        observed.insert(tag);
        merged.append(tag);
    }
    return merged;
}

template<typename T, typename Member>
void chooseMergeField(const T &base, const T &left, const T &right, T &merged,
    Member T::*member, const char *name, QStringList &conflicts,
    const QMap<QString, bool> &field_choices)
{
    // EntityMetadata is the only intentionally decomposed nested structure.
    // Its seven codec fields have stable independent meanings. Tags support
    // only disjoint append-only changes; other nested collections are atomic.
    if constexpr (std::is_same_v<Member, EntityMetadata>)
    {
        if (QString::fromLatin1(name) != QStringLiteral("metadata"))
        {
            conflicts.append(QString::fromLatin1(name));
            return;
        }
        const EntityMetadata &base_metadata = base.*member;
        const EntityMetadata &left_metadata = left.*member;
        const EntityMetadata &right_metadata = right.*member;
        EntityMetadata &merged_metadata = merged.*member;
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::enabled, "metadata.enabled", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::model_role, "metadata.model_role", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::date_added, "metadata.date_added", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::date_installed, "metadata.date_installed", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::description, "metadata.description", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::comment, "metadata.comment", conflicts, field_choices);
        chooseMergeField(base_metadata, left_metadata, right_metadata, merged_metadata,
            &EntityMetadata::tags, "metadata.tags", conflicts, field_choices);
    }
    else
    {
        const QByteArray baseline = encodedMergeField(base.*member);
        const QByteArray left_bytes = encodedMergeField(left.*member);
        const QByteArray right_bytes = encodedMergeField(right.*member);
        if (baseline.isEmpty() || left_bytes.isEmpty() || right_bytes.isEmpty())
            conflicts.append(QString::fromLatin1(name));
        else if (left_bytes == right_bytes || right_bytes == baseline)
            merged.*member = left.*member;
        else if (left_bytes == baseline)
            merged.*member = right.*member;
        else
        {
            if constexpr (std::is_same_v<Member, QStringList>)
            {
                if (QString::fromLatin1(name) == QStringLiteral("metadata.tags"))
                {
                    const std::optional<QStringList> appended = mergeAppendedMetadataTags(
                        base.*member, left.*member, right.*member);
                    if (appended)
                    {
                        merged.*member = *appended;
                        return;
                    }
                }
            }
            const QMap<QString, bool>::const_iterator choice = field_choices.constFind(QString::fromLatin1(name));
            if (choice == field_choices.cend())
                conflicts.append(QString::fromLatin1(name));
            else
                merged.*member = choice.value() ? right.*member : left.*member;
        }
    }
}

template<typename T>
std::optional<ModelFieldMerge> mergeFieldsForType(const QUuid &object_uuid,
    const QByteArray &baseline_bytes, const QByteArray &left_bytes,
    const QByteArray &right_bytes, const QMap<QString, bool> &field_choices);

// These field lists match docs/model_codec_fields.json and the ordered
// writeStruct/readStruct declarations above. A configure-time checker ensures
// that a codec or Model schema change cannot silently omit a merge field.
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicPipeMaterial>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicPipeMaterial> baseline = ModelCodec<HydraulicPipeMaterial>::decode(baseline_bytes);
    const std::optional<HydraulicPipeMaterial> left = ModelCodec<HydraulicPipeMaterial>::decode(left_bytes);
    const std::optional<HydraulicPipeMaterial> right = ModelCodec<HydraulicPipeMaterial>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicPipeMaterial>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicPipeMaterial>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicPipeMaterial>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicPipeMaterial merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPipeMaterial::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPipeMaterial::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPipeMaterial::description, "description", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPipeMaterial::roughness_by_age, "roughness_by_age", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicPipeMaterial>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicPatternTime>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicPatternTime> baseline = ModelCodec<HydraulicPatternTime>::decode(baseline_bytes);
    const std::optional<HydraulicPatternTime> left = ModelCodec<HydraulicPatternTime>::decode(left_bytes);
    const std::optional<HydraulicPatternTime> right = ModelCodec<HydraulicPatternTime>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicPatternTime>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicPatternTime>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicPatternTime>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicPatternTime merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPatternTime::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPatternTime::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPatternTime::multipliers, "multipliers", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicPatternTime::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicPatternTime>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurveTankVolume>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurveTankVolume> baseline = ModelCodec<HydraulicCurveTankVolume>::decode(baseline_bytes);
    const std::optional<HydraulicCurveTankVolume> left = ModelCodec<HydraulicCurveTankVolume>::decode(left_bytes);
    const std::optional<HydraulicCurveTankVolume> right = ModelCodec<HydraulicCurveTankVolume>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurveTankVolume>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurveTankVolume>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurveTankVolume>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurveTankVolume merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveTankVolume::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveTankVolume::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveTankVolume::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveTankVolume::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurveTankVolume>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurvePumpHead>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurvePumpHead> baseline = ModelCodec<HydraulicCurvePumpHead>::decode(baseline_bytes);
    const std::optional<HydraulicCurvePumpHead> left = ModelCodec<HydraulicCurvePumpHead>::decode(left_bytes);
    const std::optional<HydraulicCurvePumpHead> right = ModelCodec<HydraulicCurvePumpHead>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurvePumpHead>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurvePumpHead>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurvePumpHead>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurvePumpHead merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpHead::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpHead::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpHead::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpHead::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurvePumpHead>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurvePumpEfficiency>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurvePumpEfficiency> baseline = ModelCodec<HydraulicCurvePumpEfficiency>::decode(baseline_bytes);
    const std::optional<HydraulicCurvePumpEfficiency> left = ModelCodec<HydraulicCurvePumpEfficiency>::decode(left_bytes);
    const std::optional<HydraulicCurvePumpEfficiency> right = ModelCodec<HydraulicCurvePumpEfficiency>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurvePumpEfficiency>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurvePumpEfficiency>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurvePumpEfficiency>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurvePumpEfficiency merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpEfficiency::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpEfficiency::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpEfficiency::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurvePumpEfficiency::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurvePumpEfficiency>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurveValveHeadloss>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurveValveHeadloss> baseline = ModelCodec<HydraulicCurveValveHeadloss>::decode(baseline_bytes);
    const std::optional<HydraulicCurveValveHeadloss> left = ModelCodec<HydraulicCurveValveHeadloss>::decode(left_bytes);
    const std::optional<HydraulicCurveValveHeadloss> right = ModelCodec<HydraulicCurveValveHeadloss>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurveValveHeadloss>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurveValveHeadloss>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurveValveHeadloss>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurveValveHeadloss merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveHeadloss::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveHeadloss::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveHeadloss::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveHeadloss::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurveValveHeadloss>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurveValveCharacteristic>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurveValveCharacteristic> baseline = ModelCodec<HydraulicCurveValveCharacteristic>::decode(baseline_bytes);
    const std::optional<HydraulicCurveValveCharacteristic> left = ModelCodec<HydraulicCurveValveCharacteristic>::decode(left_bytes);
    const std::optional<HydraulicCurveValveCharacteristic> right = ModelCodec<HydraulicCurveValveCharacteristic>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurveValveCharacteristic>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurveValveCharacteristic>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurveValveCharacteristic>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurveValveCharacteristic merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveCharacteristic::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveCharacteristic::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveCharacteristic::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveValveCharacteristic::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurveValveCharacteristic>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicCurveGeneric>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicCurveGeneric> baseline = ModelCodec<HydraulicCurveGeneric>::decode(baseline_bytes);
    const std::optional<HydraulicCurveGeneric> left = ModelCodec<HydraulicCurveGeneric>::decode(left_bytes);
    const std::optional<HydraulicCurveGeneric> right = ModelCodec<HydraulicCurveGeneric>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicCurveGeneric>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicCurveGeneric>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicCurveGeneric>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicCurveGeneric merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveGeneric::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveGeneric::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveGeneric::points, "points", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicCurveGeneric::comment, "comment", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicCurveGeneric>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicNodeReservoir>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicNodeReservoir> baseline = ModelCodec<HydraulicNodeReservoir>::decode(baseline_bytes);
    const std::optional<HydraulicNodeReservoir> left = ModelCodec<HydraulicNodeReservoir>::decode(left_bytes);
    const std::optional<HydraulicNodeReservoir> right = ModelCodec<HydraulicNodeReservoir>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicNodeReservoir>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicNodeReservoir>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicNodeReservoir>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicNodeReservoir merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::coordinate_wgs84, "coordinate_wgs84", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::head_input_type, "head_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::hydraulic_head_m, "hydraulic_head_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::terrain_elevation_m, "terrain_elevation_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::hydraulic_head_offset_m, "hydraulic_head_offset_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::head_pattern_mode, "head_pattern_mode", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::head_pattern_uuid, "head_pattern_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::initial_chemical_concentration_mg_per_l, "initial_chemical_concentration_mg_per_l", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::initial_water_age_h, "initial_water_age_h", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::quality_source, "quality_source", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeReservoir::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicNodeReservoir>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicNodeJunction>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicNodeJunction> baseline = ModelCodec<HydraulicNodeJunction>::decode(baseline_bytes);
    const std::optional<HydraulicNodeJunction> left = ModelCodec<HydraulicNodeJunction>::decode(left_bytes);
    const std::optional<HydraulicNodeJunction> right = ModelCodec<HydraulicNodeJunction>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicNodeJunction>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicNodeJunction>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicNodeJunction>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicNodeJunction merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::coordinate_wgs84, "coordinate_wgs84", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::elevation_input_type, "elevation_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::elevation_m, "elevation_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::terrain_elevation_m, "terrain_elevation_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::elevation_offset_m, "elevation_offset_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::demands, "demands", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::emitter, "emitter", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::initial_chemical_concentration_mg_per_l, "initial_chemical_concentration_mg_per_l", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::initial_water_age_h, "initial_water_age_h", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::quality_source, "quality_source", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeJunction::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicNodeJunction>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicNodeTank>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicNodeTank> baseline = ModelCodec<HydraulicNodeTank>::decode(baseline_bytes);
    const std::optional<HydraulicNodeTank> left = ModelCodec<HydraulicNodeTank>::decode(left_bytes);
    const std::optional<HydraulicNodeTank> right = ModelCodec<HydraulicNodeTank>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicNodeTank>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicNodeTank>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicNodeTank>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicNodeTank merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::coordinate_wgs84, "coordinate_wgs84", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::elevation_input_type, "elevation_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::bottom_elevation_m, "bottom_elevation_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::terrain_elevation_m, "terrain_elevation_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::bottom_offset_m, "bottom_offset_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::water_level_initial_m, "water_level_initial_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::water_level_minimum_m, "water_level_minimum_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::water_level_maximum_m, "water_level_maximum_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::geometry_input_type, "geometry_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::diameter_m, "diameter_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::cross_section_area_m2, "cross_section_area_m2", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::volume_at_maximum_level_m3, "volume_at_maximum_level_m3", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::minimum_volume_m3, "minimum_volume_m3", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::volume_curve_uuid, "volume_curve_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::can_overflow, "can_overflow", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::initial_chemical_concentration_mg_per_l, "initial_chemical_concentration_mg_per_l", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::initial_water_age_h, "initial_water_age_h", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::quality_source, "quality_source", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::mixing_model, "mixing_model", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::mixing_fraction, "mixing_fraction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::override_bulk_reaction, "override_bulk_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::bulk_reaction, "bulk_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicNodeTank::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicNodeTank>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicDemandPoint>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicDemandPoint> baseline = ModelCodec<HydraulicDemandPoint>::decode(baseline_bytes);
    const std::optional<HydraulicDemandPoint> left = ModelCodec<HydraulicDemandPoint>::decode(left_bytes);
    const std::optional<HydraulicDemandPoint> right = ModelCodec<HydraulicDemandPoint>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicDemandPoint>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicDemandPoint>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicDemandPoint>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicDemandPoint merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::coordinate_wgs84, "coordinate_wgs84", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::attachment, "attachment", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::demands, "demands", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::meter, "meter", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicDemandPoint::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicDemandPoint>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicLinkPipe>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicLinkPipe> baseline = ModelCodec<HydraulicLinkPipe>::decode(baseline_bytes);
    const std::optional<HydraulicLinkPipe> left = ModelCodec<HydraulicLinkPipe>::decode(left_bytes);
    const std::optional<HydraulicLinkPipe> right = ModelCodec<HydraulicLinkPipe>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicLinkPipe>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicLinkPipe>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicLinkPipe>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicLinkPipe merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::node_uuid_from, "node_uuid_from", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::node_uuid_to, "node_uuid_to", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::vertices, "vertices", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::length_calculated_m, "length_calculated_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::length_measured_m, "length_measured_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::initial_status, "initial_status", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::diameter_mm, "diameter_mm", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::material_uuid, "material_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::roughness_mode, "roughness_mode", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::roughness_hazen_williams, "roughness_hazen_williams", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::roughness_darcy_weisbach_mm, "roughness_darcy_weisbach_mm", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::roughness_chezy_manning, "roughness_chezy_manning", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::minor_loss_coefficient, "minor_loss_coefficient", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::override_bulk_reaction, "override_bulk_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::bulk_reaction, "bulk_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::override_wall_reaction, "override_wall_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::wall_reaction, "wall_reaction", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::leak_area_mm2_per_100m, "leak_area_mm2_per_100m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::leak_area_expansion_per_pressure_head_mm2_per_m, "leak_area_expansion_per_pressure_head_mm2_per_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPipe::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicLinkPipe>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicLinkPump>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicLinkPump> baseline = ModelCodec<HydraulicLinkPump>::decode(baseline_bytes);
    const std::optional<HydraulicLinkPump> left = ModelCodec<HydraulicLinkPump>::decode(left_bytes);
    const std::optional<HydraulicLinkPump> right = ModelCodec<HydraulicLinkPump>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicLinkPump>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicLinkPump>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicLinkPump>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicLinkPump merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::node_uuid_from, "node_uuid_from", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::node_uuid_to, "node_uuid_to", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::vertices, "vertices", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::definition_type, "definition_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::constant_power_kw, "constant_power_kw", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::head_curve_uuid, "head_curve_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::initial_speed_ratio, "initial_speed_ratio", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::initial_status, "initial_status", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::speed_pattern_uuid, "speed_pattern_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::efficiency_input_type, "efficiency_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::constant_efficiency_percent, "constant_efficiency_percent", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::efficiency_curve_uuid, "efficiency_curve_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::energy_price_input_type, "energy_price_input_type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::energy_price_per_kw_h, "energy_price_per_kw_h", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::price_pattern_uuid, "price_pattern_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkPump::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicLinkPump>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicLinkValve>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicLinkValve> baseline = ModelCodec<HydraulicLinkValve>::decode(baseline_bytes);
    const std::optional<HydraulicLinkValve> left = ModelCodec<HydraulicLinkValve>::decode(left_bytes);
    const std::optional<HydraulicLinkValve> right = ModelCodec<HydraulicLinkValve>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicLinkValve>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicLinkValve>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicLinkValve>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicLinkValve merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::node_uuid_from, "node_uuid_from", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::node_uuid_to, "node_uuid_to", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::vertices, "vertices", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::type, "type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::setting_pressure_head_m, "setting_pressure_head_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::setting_flow_m3_per_h, "setting_flow_m3_per_h", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::setting_loss_coefficient, "setting_loss_coefficient", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::setting_position_percent, "setting_position_percent", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::head_loss_curve_uuid, "head_loss_curve_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::characteristic_curve_uuid, "characteristic_curve_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::initial_status, "initial_status", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::diameter_mm, "diameter_mm", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::minor_loss_coefficient, "minor_loss_coefficient", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicLinkValve::metadata, "metadata", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicLinkValve>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicControlSimple>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicControlSimple> baseline = ModelCodec<HydraulicControlSimple>::decode(baseline_bytes);
    const std::optional<HydraulicControlSimple> left = ModelCodec<HydraulicControlSimple>::decode(left_bytes);
    const std::optional<HydraulicControlSimple> right = ModelCodec<HydraulicControlSimple>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicControlSimple>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicControlSimple>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicControlSimple>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicControlSimple merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::type, "type", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::link_uuid, "link_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::action, "action", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::setting, "setting", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::trigger_node_uuid, "trigger_node_uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::trigger_water_level_m, "trigger_water_level_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::trigger_pressure_head_m, "trigger_pressure_head_m", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::trigger_elapsed_time_s, "trigger_elapsed_time_s", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::trigger_time_of_day_s, "trigger_time_of_day_s", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlSimple::enabled, "enabled", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicControlSimple>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicControlRule>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicControlRule> baseline = ModelCodec<HydraulicControlRule>::decode(baseline_bytes);
    const std::optional<HydraulicControlRule> left = ModelCodec<HydraulicControlRule>::decode(left_bytes);
    const std::optional<HydraulicControlRule> right = ModelCodec<HydraulicControlRule>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicControlRule>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicControlRule>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicControlRule>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicControlRule merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::premises, "premises", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::actions_then, "actions_then", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::actions_else, "actions_else", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::priority, "priority", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicControlRule::enabled, "enabled", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicControlRule>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
template<> std::optional<ModelFieldMerge> mergeFieldsForType<HydraulicMapLabel>(
    const QUuid &object_uuid, const QByteArray &baseline_bytes,
    const QByteArray &left_bytes, const QByteArray &right_bytes,
    const QMap<QString, bool> &field_choices)
{
    const std::optional<HydraulicMapLabel> baseline = ModelCodec<HydraulicMapLabel>::decode(baseline_bytes);
    const std::optional<HydraulicMapLabel> left = ModelCodec<HydraulicMapLabel>::decode(left_bytes);
    const std::optional<HydraulicMapLabel> right = ModelCodec<HydraulicMapLabel>::decode(right_bytes);
    if (!baseline || !left || !right || baseline->uuid != object_uuid ||
        left->uuid != object_uuid || right->uuid != object_uuid ||
        ModelCodec<HydraulicMapLabel>::encode(*baseline) != baseline_bytes ||
        ModelCodec<HydraulicMapLabel>::encode(*left) != left_bytes ||
        ModelCodec<HydraulicMapLabel>::encode(*right) != right_bytes)
        return std::nullopt;
    HydraulicMapLabel merged = *left;
    ModelFieldMerge result;
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicMapLabel::id, "id", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicMapLabel::uuid, "uuid", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicMapLabel::coordinate_wgs84, "coordinate_wgs84", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicMapLabel::text, "text", result.conflicting_fields, field_choices);
    chooseMergeField(*baseline, *left, *right, merged, &HydraulicMapLabel::anchor_node_uuid, "anchor_node_uuid", result.conflicting_fields, field_choices);
    if (result.conflicting_fields.isEmpty())
    {
        result.content = ModelCodec<HydraulicMapLabel>::encode(merged);
        if (result.content.isEmpty())
            return std::nullopt;
    }
    return result;
}
std::optional<ModelFieldMerge> mergeModelFields(const QUuid &object_uuid,
    const QString &kind, const QByteArray &base, const QByteArray &left,
    const QByteArray &right, const QMap<QString, bool> &field_choices)
{
    // Bound discretionary field parsing in merge preview. Large objects are
    // still mergeable through existing explicit entity-level resolutions.
    constexpr qsizetype field_merge_limit = 4 * 1024 * 1024;
    if (object_uuid.isNull() || base.isEmpty() || left.isEmpty() || right.isEmpty() ||
        base.size() > field_merge_limit || left.size() > field_merge_limit ||
        right.size() > field_merge_limit)
        return std::nullopt;
    if (kind == QStringLiteral("hydraulic.pipe_material"))
        return mergeFieldsForType<HydraulicPipeMaterial>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.pattern_time"))
        return mergeFieldsForType<HydraulicPatternTime>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_tank_volume"))
        return mergeFieldsForType<HydraulicCurveTankVolume>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_pump_head"))
        return mergeFieldsForType<HydraulicCurvePumpHead>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_pump_efficiency"))
        return mergeFieldsForType<HydraulicCurvePumpEfficiency>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_valve_headloss"))
        return mergeFieldsForType<HydraulicCurveValveHeadloss>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_valve_characteristic"))
        return mergeFieldsForType<HydraulicCurveValveCharacteristic>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.curve_generic"))
        return mergeFieldsForType<HydraulicCurveGeneric>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.reservoir"))
        return mergeFieldsForType<HydraulicNodeReservoir>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.junction"))
        return mergeFieldsForType<HydraulicNodeJunction>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.tank"))
        return mergeFieldsForType<HydraulicNodeTank>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.demand_point"))
        return mergeFieldsForType<HydraulicDemandPoint>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.pipe"))
        return mergeFieldsForType<HydraulicLinkPipe>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.pump"))
        return mergeFieldsForType<HydraulicLinkPump>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.valve"))
        return mergeFieldsForType<HydraulicLinkValve>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.control_simple"))
        return mergeFieldsForType<HydraulicControlSimple>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.control_rule"))
        return mergeFieldsForType<HydraulicControlRule>(object_uuid, base, left, right, field_choices);
    if (kind == QStringLiteral("hydraulic.map_label"))
        return mergeFieldsForType<HydraulicMapLabel>(object_uuid, base, left, right, field_choices);
    return std::nullopt;
}

} // namespace aowis::native
