// Physical value rules for persisted hydraulic models (review finding F8).
// The table is documented in docs/model-value-rules.md; keep both in sync.
//
// Principle: reject values that are impossible (NaN, negative lengths,
// efficiencies above 100 %, inverted tank levels, unordered curves), accept
// "not yet set" draft values (0, empty lists, null references). Solver
// readiness is checked when a model is exported to a simulator, not here.
//
// tools/check_model_codec.py fails the build when a numeric Model field (or a
// struct field containing numbers) is not referenced in its check() below,
// so a new number can never be stored without a decided range.
#include <aowis/dvmdb/model_validation.h>

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

namespace aowis::dvmdb
{
namespace
{
constexpr std::uint64_t secondsPerDay = 86400;

class Rules final
{
public:
    explicit Rules(ModelValidationReport &report) : report_(report) {}

    void issue(const QString &path, const QString &message)
    {
        this->report_.issues.append({path, message});
    }
    void finite(const QString &path, double value)
    {
        if (!std::isfinite(value))
            issue(path, QStringLiteral("must be a finite number"));
    }
    void atLeast(const QString &path, double value, double minimum)
    {
        if (!std::isfinite(value) || value < minimum)
            issue(path, QStringLiteral("must be a finite number >= %1").arg(minimum));
    }
    void positive(const QString &path, double value)
    {
        if (!std::isfinite(value) || value <= 0.0)
            issue(path, QStringLiteral("must be a finite number > 0"));
    }
    void between(const QString &path, double value, double minimum, double maximum)
    {
        if (!std::isfinite(value) || value < minimum || value > maximum)
            issue(path, QStringLiteral("must be between %1 and %2").arg(minimum).arg(maximum));
    }
    // 0 < value <= maximum, e.g. efficiencies in percent.
    void positiveAtMost(const QString &path, double value, double maximum)
    {
        if (!std::isfinite(value) || value <= 0.0 || value > maximum)
            issue(path, QStringLiteral("must be greater than 0 and at most %1").arg(maximum));
    }
    void finite(const QString &path, const std::optional<double> &value)
    {
        if (value) finite(path, *value);
    }
    void atLeast(const QString &path, const std::optional<double> &value, double minimum)
    {
        if (value) atLeast(path, *value, minimum);
    }
    void positive(const QString &path, const std::optional<double> &value)
    {
        if (value) positive(path, *value);
    }
    void between(const QString &path, const std::optional<double> &value, double minimum, double maximum)
    {
        if (value) between(path, *value, minimum, maximum);
    }
    void integerAtLeast(const QString &path, long long value, long long minimum)
    {
        if (value < minimum)
            issue(path, QStringLiteral("must be an integer >= %1").arg(minimum));
    }
    void integerBetween(const QString &path, long long value, long long minimum, long long maximum)
    {
        if (value < minimum || value > maximum)
            issue(path, QStringLiteral("must be an integer between %1 and %2").arg(minimum).arg(maximum));
    }
    void timeOfDay(const QString &path, std::uint64_t seconds)
    {
        if (seconds >= secondsPerDay)
            issue(path, QStringLiteral("must be a time of day below 86400 s"));
    }
    void timeOfDay(const QString &path, const std::optional<quint64> &seconds)
    {
        if (seconds) timeOfDay(path, *seconds);
    }
    void positiveDuration(const QString &path, std::uint64_t seconds)
    {
        if (seconds == 0)
            issue(path, QStringLiteral("must be greater than 0 s"));
    }
    // Explicitly decided: every value of the type is valid.
    template<typename T> void unbounded(const QString &, const T &) {}

private:
    ModelValidationReport &report_;
};

QString item(const QString &path, const char *field, qsizetype index)
{
    return path + QLatin1Char('.') + QLatin1String(field) + QLatin1Char('[') +
           QString::number(index) + QLatin1Char(']');
}

QString field(const QString &path, const char *name)
{
    return path + QLatin1Char('.') + QLatin1String(name);
}

// Curves are functions of x: abscissas must strictly increase.
template<typename Point>
void increasing(Rules &rules, const QString &path, const QList<Point> &points,
                const std::function<double(const Point &)> &abscissa)
{
    for (qsizetype index = 1; index < points.size(); ++index)
    {
        const double previous = abscissa(points[index - 1]);
        const double current = abscissa(points[index]);
        if (std::isfinite(previous) && std::isfinite(current) && !(current > previous))
            rules.issue(item(path, "points", index), QStringLiteral("curve x values must strictly increase"));
    }
}

void check(Rules &rules, const QString &path, const CoordinateWGS84 &value)
{
    rules.between(field(path, "longitude_deg"), value.longitude_deg, -180.0, 180.0);
    rules.between(field(path, "latitude_deg"), value.latitude_deg, -90.0, 90.0);
    rules.finite(field(path, "altitude_m"), value.altitude_m);
}

void check(Rules &rules, const QString &path, const WaterQualityBulkReaction &value)
{
    // Negative coefficients are decay; negative orders select
    // Michaelis-Menten kinetics. Both are valid.
    rules.finite(field(path, "coefficient"), value.coefficient);
    rules.finite(field(path, "order"), value.order);
}

void check(Rules &rules, const QString &path, const WaterQualityWallReaction &value)
{
    rules.finite(field(path, "coefficient"), value.coefficient);
    if (!(value.order == 0.0 || value.order == 1.0))
        rules.issue(field(path, "order"), QStringLiteral("wall reaction order must be 0 or 1"));
}

void check(Rules &rules, const QString &path, const HydraulicControlLinkSetting &value)
{
    rules.atLeast(field(path, "pump_speed_ratio"), value.pump_speed_ratio, 0.0);
    rules.finite(field(path, "valve_pressure_head_m"), value.valve_pressure_head_m);
    rules.atLeast(field(path, "valve_flow_m3_per_h"), value.valve_flow_m3_per_h, 0.0);
    rules.atLeast(field(path, "valve_loss_coefficient"), value.valve_loss_coefficient, 0.0);
    rules.between(field(path, "valve_position_percent"), value.valve_position_percent, 0.0, 100.0);
}

void check(Rules &rules, const QString &path, const HydraulicControlRulePremise &value)
{
    // Demands, heads, pressures and flows are signed quantities.
    rules.finite(field(path, "demand_m3_per_h"), value.demand_m3_per_h);
    rules.finite(field(path, "hydraulic_head_m"), value.hydraulic_head_m);
    rules.atLeast(field(path, "water_level_m"), value.water_level_m, 0.0);
    rules.finite(field(path, "pressure_head_m"), value.pressure_head_m);
    rules.finite(field(path, "flow_m3_per_h"), value.flow_m3_per_h);
    check(rules, field(path, "link_setting"), value.link_setting);
    rules.atLeast(field(path, "power_kw"), value.power_kw, 0.0);
    rules.unbounded(field(path, "elapsed_time_s"), value.elapsed_time_s);
    rules.timeOfDay(field(path, "time_of_day_s"), value.time_of_day_s);
    rules.unbounded(field(path, "fill_time_s"), value.fill_time_s);
    rules.unbounded(field(path, "drain_time_s"), value.drain_time_s);
}

void check(Rules &rules, const QString &path, const HydraulicControlRuleAction &value)
{
    check(rules, field(path, "setting"), value.setting);
}

void check(Rules &rules, const QString &path, const HydraulicControlRule &value)
{
    for (qsizetype index = 0; index < value.premises.size(); ++index)
        check(rules, item(path, "premises", index), value.premises[index]);
    for (qsizetype index = 0; index < value.actions_then.size(); ++index)
        check(rules, item(path, "actions_then", index), value.actions_then[index]);
    for (qsizetype index = 0; index < value.actions_else.size(); ++index)
        check(rules, item(path, "actions_else", index), value.actions_else[index]);
    rules.finite(field(path, "priority"), value.priority);
}

void check(Rules &rules, const QString &path, const HydraulicControlSimple &value)
{
    check(rules, field(path, "setting"), value.setting);
    rules.atLeast(field(path, "trigger_water_level_m"), value.trigger_water_level_m, 0.0);
    rules.finite(field(path, "trigger_pressure_head_m"), value.trigger_pressure_head_m);
    rules.unbounded(field(path, "trigger_elapsed_time_s"), value.trigger_elapsed_time_s);
    rules.timeOfDay(field(path, "trigger_time_of_day_s"), value.trigger_time_of_day_s);
}

void check(Rules &rules, const QString &path, const HydraulicCurveGenericPoint &value)
{
    rules.finite(field(path, "x"), value.x);
    rules.finite(field(path, "y"), value.y);
}

void check(Rules &rules, const QString &path, const HydraulicCurveGeneric &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurveGenericPoint>(rules, path, value.points,
        [](const HydraulicCurveGenericPoint &point) { return point.x; });
}

void check(Rules &rules, const QString &path, const HydraulicCurvePumpEfficiencyPoint &value)
{
    rules.atLeast(field(path, "flow_m3_per_h"), value.flow_m3_per_h, 0.0);
    rules.between(field(path, "efficiency_percent"), value.efficiency_percent, 0.0, 100.0);
}

void check(Rules &rules, const QString &path, const HydraulicCurvePumpEfficiency &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurvePumpEfficiencyPoint>(rules, path, value.points,
        [](const HydraulicCurvePumpEfficiencyPoint &point) { return point.flow_m3_per_h; });
}

void check(Rules &rules, const QString &path, const HydraulicCurvePumpHeadPoint &value)
{
    rules.atLeast(field(path, "flow_m3_per_h"), value.flow_m3_per_h, 0.0);
    rules.atLeast(field(path, "head_gain_m"), value.head_gain_m, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicCurvePumpHead &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurvePumpHeadPoint>(rules, path, value.points,
        [](const HydraulicCurvePumpHeadPoint &point) { return point.flow_m3_per_h; });
}

void check(Rules &rules, const QString &path, const HydraulicCurveTankVolumePoint &value)
{
    rules.atLeast(field(path, "water_level_m"), value.water_level_m, 0.0);
    rules.atLeast(field(path, "volume_m3"), value.volume_m3, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicCurveTankVolume &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurveTankVolumePoint>(rules, path, value.points,
        [](const HydraulicCurveTankVolumePoint &point) { return point.water_level_m; });
    // More water never takes less volume.
    for (qsizetype index = 1; index < value.points.size(); ++index)
        if (value.points[index].volume_m3 < value.points[index - 1].volume_m3)
            rules.issue(item(path, "points", index), QStringLiteral("tank volume must not decrease with level"));
}

void check(Rules &rules, const QString &path, const HydraulicCurveValveCharacteristicPoint &value)
{
    rules.between(field(path, "position_percent"), value.position_percent, 0.0, 100.0);
    rules.between(field(path, "relative_flow_percent"), value.relative_flow_percent, 0.0, 100.0);
}

void check(Rules &rules, const QString &path, const HydraulicCurveValveCharacteristic &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurveValveCharacteristicPoint>(rules, path, value.points,
        [](const HydraulicCurveValveCharacteristicPoint &point) { return point.position_percent; });
}

void check(Rules &rules, const QString &path, const HydraulicCurveValveHeadlossPoint &value)
{
    rules.atLeast(field(path, "flow_m3_per_h"), value.flow_m3_per_h, 0.0);
    rules.atLeast(field(path, "head_loss_m"), value.head_loss_m, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicCurveValveHeadloss &value)
{
    for (qsizetype index = 0; index < value.points.size(); ++index)
        check(rules, item(path, "points", index), value.points[index]);
    increasing<HydraulicCurveValveHeadlossPoint>(rules, path, value.points,
        [](const HydraulicCurveValveHeadlossPoint &point) { return point.flow_m3_per_h; });
}

void check(Rules &rules, const QString &path, const HydraulicPatternTime &value)
{
    // Multipliers may be negative (a negative demand is an inflow).
    for (qsizetype index = 0; index < value.multipliers.size(); ++index)
        rules.finite(item(path, "multipliers", index), value.multipliers[index]);
}

void check(Rules &rules, const QString &path, const HydraulicDemand &value)
{
    // A negative base demand is an inflow.
    rules.finite(field(path, "base_demand_m3_per_h"), value.base_demand_m3_per_h);
}

void check(Rules &rules, const QString &path, const HydraulicDemandPointAttachment &value)
{
    rules.between(field(path, "pipe_position"), value.pipe_position, 0.0, 1.0);
}

void check(Rules &rules, const QString &path, const HydraulicDemandPoint &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
    check(rules, field(path, "attachment"), value.attachment);
    for (qsizetype index = 0; index < value.demands.size(); ++index)
        check(rules, item(path, "demands", index), value.demands[index]);
}

void check(Rules &rules, const QString &path, const HydraulicLinkVertex &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
}

void check(Rules &rules, const QString &path, const HydraulicLinkPipe &value)
{
    for (qsizetype index = 0; index < value.vertices.size(); ++index)
        check(rules, item(path, "vertices", index), value.vertices[index]);
    rules.atLeast(field(path, "length_calculated_m"), value.length_calculated_m, 0.0);
    rules.positive(field(path, "length_measured_m"), value.length_measured_m);
    rules.positive(field(path, "diameter_mm"), value.diameter_mm);
    rules.positive(field(path, "roughness_hazen_williams"), value.roughness_hazen_williams);
    rules.atLeast(field(path, "roughness_darcy_weisbach_mm"), value.roughness_darcy_weisbach_mm, 0.0);
    rules.positive(field(path, "roughness_chezy_manning"), value.roughness_chezy_manning);
    rules.atLeast(field(path, "minor_loss_coefficient"), value.minor_loss_coefficient, 0.0);
    check(rules, field(path, "bulk_reaction"), value.bulk_reaction);
    check(rules, field(path, "wall_reaction"), value.wall_reaction);
    rules.atLeast(field(path, "leak_area_mm2_per_100m"), value.leak_area_mm2_per_100m, 0.0);
    rules.atLeast(field(path, "leak_area_expansion_per_pressure_head_mm2_per_m"),
                  value.leak_area_expansion_per_pressure_head_mm2_per_m, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicLinkPump &value)
{
    for (qsizetype index = 0; index < value.vertices.size(); ++index)
        check(rules, item(path, "vertices", index), value.vertices[index]);
    rules.atLeast(field(path, "constant_power_kw"), value.constant_power_kw, 0.0);
    rules.atLeast(field(path, "initial_speed_ratio"), value.initial_speed_ratio, 0.0);
    rules.positiveAtMost(field(path, "constant_efficiency_percent"), value.constant_efficiency_percent, 100.0);
    rules.atLeast(field(path, "energy_price_per_kw_h"), value.energy_price_per_kw_h, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicLinkValve &value)
{
    for (qsizetype index = 0; index < value.vertices.size(); ++index)
        check(rules, item(path, "vertices", index), value.vertices[index]);
    rules.finite(field(path, "setting_pressure_head_m"), value.setting_pressure_head_m);
    rules.atLeast(field(path, "setting_flow_m3_per_h"), value.setting_flow_m3_per_h, 0.0);
    rules.atLeast(field(path, "setting_loss_coefficient"), value.setting_loss_coefficient, 0.0);
    rules.between(field(path, "setting_position_percent"), value.setting_position_percent, 0.0, 100.0);
    rules.atLeast(field(path, "diameter_mm"), value.diameter_mm, 0.0);
    rules.atLeast(field(path, "minor_loss_coefficient"), value.minor_loss_coefficient, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicMapBackdrop &value)
{
    check(rules, field(path, "lower_left_wgs84"), value.lower_left_wgs84);
    check(rules, field(path, "upper_right_wgs84"), value.upper_right_wgs84);
    rules.between(field(path, "offset_longitude_deg"), value.offset_longitude_deg, -360.0, 360.0);
    rules.between(field(path, "offset_latitude_deg"), value.offset_latitude_deg, -180.0, 180.0);
}

void check(Rules &rules, const QString &path, const HydraulicMapLabel &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
}

void check(Rules &rules, const QString &path, const HydraulicNodeJunctionEmitter &value)
{
    rules.atLeast(field(path, "coefficient"), value.coefficient, 0.0);
    rules.positive(field(path, "pressure_exponent"), value.pressure_exponent);
}

void check(Rules &rules, const QString &path, const HydraulicNodeQualitySource &value)
{
    rules.atLeast(field(path, "chemical_concentration_mg_per_l"), value.chemical_concentration_mg_per_l, 0.0);
    rules.atLeast(field(path, "chemical_mass_flow_mg_per_min"), value.chemical_mass_flow_mg_per_min, 0.0);
}

void check(Rules &rules, const QString &path, const HydraulicNodeJunction &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
    // Elevations are signed (below sea level is valid).
    rules.finite(field(path, "elevation_m"), value.elevation_m);
    rules.finite(field(path, "terrain_elevation_m"), value.terrain_elevation_m);
    rules.finite(field(path, "elevation_offset_m"), value.elevation_offset_m);
    for (qsizetype index = 0; index < value.demands.size(); ++index)
        check(rules, item(path, "demands", index), value.demands[index]);
    check(rules, field(path, "emitter"), value.emitter);
    rules.atLeast(field(path, "initial_chemical_concentration_mg_per_l"),
                  value.initial_chemical_concentration_mg_per_l, 0.0);
    rules.atLeast(field(path, "initial_water_age_h"), value.initial_water_age_h, 0.0);
    check(rules, field(path, "quality_source"), value.quality_source);
}

void check(Rules &rules, const QString &path, const HydraulicNodeReservoir &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
    rules.finite(field(path, "hydraulic_head_m"), value.hydraulic_head_m);
    rules.finite(field(path, "terrain_elevation_m"), value.terrain_elevation_m);
    rules.finite(field(path, "hydraulic_head_offset_m"), value.hydraulic_head_offset_m);
    rules.atLeast(field(path, "initial_chemical_concentration_mg_per_l"),
                  value.initial_chemical_concentration_mg_per_l, 0.0);
    rules.atLeast(field(path, "initial_water_age_h"), value.initial_water_age_h, 0.0);
    check(rules, field(path, "quality_source"), value.quality_source);
}

void check(Rules &rules, const QString &path, const HydraulicNodeTank &value)
{
    check(rules, field(path, "coordinate_wgs84"), value.coordinate_wgs84);
    rules.finite(field(path, "bottom_elevation_m"), value.bottom_elevation_m);
    rules.finite(field(path, "terrain_elevation_m"), value.terrain_elevation_m);
    rules.finite(field(path, "bottom_offset_m"), value.bottom_offset_m);
    // Levels are measured from the tank bottom.
    rules.atLeast(field(path, "water_level_initial_m"), value.water_level_initial_m, 0.0);
    rules.atLeast(field(path, "water_level_minimum_m"), value.water_level_minimum_m, 0.0);
    rules.atLeast(field(path, "water_level_maximum_m"), value.water_level_maximum_m, 0.0);
    if (value.water_level_minimum_m > value.water_level_initial_m ||
        value.water_level_initial_m > value.water_level_maximum_m)
        rules.issue(field(path, "water_level_initial_m"),
                    QStringLiteral("tank levels must satisfy minimum <= initial <= maximum"));
    rules.atLeast(field(path, "diameter_m"), value.diameter_m, 0.0);
    rules.atLeast(field(path, "cross_section_area_m2"), value.cross_section_area_m2, 0.0);
    rules.atLeast(field(path, "volume_at_maximum_level_m3"), value.volume_at_maximum_level_m3, 0.0);
    rules.atLeast(field(path, "minimum_volume_m3"), value.minimum_volume_m3, 0.0);
    rules.atLeast(field(path, "initial_chemical_concentration_mg_per_l"),
                  value.initial_chemical_concentration_mg_per_l, 0.0);
    rules.atLeast(field(path, "initial_water_age_h"), value.initial_water_age_h, 0.0);
    check(rules, field(path, "quality_source"), value.quality_source);
    rules.between(field(path, "mixing_fraction"), value.mixing_fraction, 0.0, 1.0);
    check(rules, field(path, "bulk_reaction"), value.bulk_reaction);
}

void check(Rules &rules, const QString &path, const HydraulicPipeMaterialRoughnessAtAge &value)
{
    rules.integerAtLeast(field(path, "age_years"), value.age_years, 0);
    rules.positive(field(path, "roughness_hazen_williams"), value.roughness_hazen_williams);
    rules.atLeast(field(path, "roughness_darcy_weisbach_mm"), value.roughness_darcy_weisbach_mm, 0.0);
    rules.positive(field(path, "roughness_chezy_manning"), value.roughness_chezy_manning);
}

void check(Rules &rules, const QString &path, const HydraulicPipeMaterial &value)
{
    for (qsizetype index = 0; index < value.roughness_by_age.size(); ++index)
    {
        check(rules, item(path, "roughness_by_age", index), value.roughness_by_age[index]);
        if (index > 0 && value.roughness_by_age[index].age_years <= value.roughness_by_age[index - 1].age_years)
            rules.issue(item(path, "roughness_by_age", index), QStringLiteral("ages must strictly increase"));
    }
}

void precision(Rules &rules, const QString &path, const std::optional<int> &value)
{
    if (value) rules.integerBetween(path, *value, 0, 15);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportField &value)
{
    precision(rules, field(path, "precision"), value.precision);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldFrictionFactor &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_friction_factor"), value.below_friction_factor);
    rules.finite(field(path, "above_friction_factor"), value.above_friction_factor);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldM &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_m"), value.below_m);
    rules.finite(field(path, "above_m"), value.above_m);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldM3PerH &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_m3_per_h"), value.below_m3_per_h);
    rules.finite(field(path, "above_m3_per_h"), value.above_m3_per_h);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldMPerKm &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_m_per_km"), value.below_m_per_km);
    rules.finite(field(path, "above_m_per_km"), value.above_m_per_km);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldMPerS &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_m_per_s"), value.below_m_per_s);
    rules.finite(field(path, "above_m_per_s"), value.above_m_per_s);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportFieldMm &value)
{
    precision(rules, field(path, "precision"), value.precision);
    rules.finite(field(path, "below_mm"), value.below_mm);
    rules.finite(field(path, "above_mm"), value.above_mm);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportLinkFields &value)
{
    check(rules, field(path, "length"), value.length);
    check(rules, field(path, "diameter"), value.diameter);
    check(rules, field(path, "flow"), value.flow);
    check(rules, field(path, "velocity"), value.velocity);
    check(rules, field(path, "headloss"), value.headloss);
    check(rules, field(path, "position"), value.position);
    check(rules, field(path, "setting"), value.setting);
    check(rules, field(path, "reaction"), value.reaction);
    check(rules, field(path, "friction"), value.friction);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportNodeFields &value)
{
    check(rules, field(path, "elevation"), value.elevation);
    check(rules, field(path, "demand"), value.demand);
    check(rules, field(path, "head"), value.head);
    check(rules, field(path, "pressure"), value.pressure);
    check(rules, field(path, "quality"), value.quality);
}

void check(Rules &rules, const QString &path, const HydraulicSimulationReportOptions &value)
{
    rules.integerAtLeast(field(path, "page_size"), value.page_size, 0);
    check(rules, field(path, "fields_node"), value.fields_node);
    check(rules, field(path, "fields_link"), value.fields_link);
}

void check(Rules &rules, const QString &path, const HydraulicSolverOptions &value)
{
    rules.atLeast(field(path, "minimum_pressure_head_m"), value.minimum_pressure_head_m, 0.0);
    rules.atLeast(field(path, "required_pressure_head_m"), value.required_pressure_head_m, 0.0);
    if (value.required_pressure_head_m < value.minimum_pressure_head_m)
        rules.issue(field(path, "required_pressure_head_m"),
                    QStringLiteral("must not be below minimum_pressure_head_m"));
    rules.positive(field(path, "pressure_exponent"), value.pressure_exponent);
    rules.integerAtLeast(field(path, "maximum_trials"), value.maximum_trials, 1);
    rules.positive(field(path, "accuracy"), value.accuracy);
    rules.integerAtLeast(field(path, "unbalanced_extra_trials"), value.unbalanced_extra_trials, 0);
    rules.integerAtLeast(field(path, "check_frequency"), value.check_frequency, 1);
    rules.integerAtLeast(field(path, "maximum_check"), value.maximum_check, 0);
    rules.atLeast(field(path, "damping_limit"), value.damping_limit, 0.0);
    rules.atLeast(field(path, "maximum_head_error_m"), value.maximum_head_error_m, 0.0);
    rules.atLeast(field(path, "maximum_flow_change_m3_per_h"), value.maximum_flow_change_m3_per_h, 0.0);
    rules.atLeast(field(path, "demand_multiplier"), value.demand_multiplier, 0.0);
    rules.positive(field(path, "specific_gravity"), value.specific_gravity);
    rules.positive(field(path, "relative_viscosity"), value.relative_viscosity);
}

void check(Rules &rules, const QString &path, const WaterQualityReactionOptions &value)
{
    check(rules, field(path, "global_pipe_bulk_reaction"), value.global_pipe_bulk_reaction);
    check(rules, field(path, "global_pipe_wall_reaction"), value.global_pipe_wall_reaction);
    check(rules, field(path, "global_tank_bulk_reaction"), value.global_tank_bulk_reaction);
    rules.atLeast(field(path, "limiting_concentration_mg_per_l"), value.limiting_concentration_mg_per_l, 0.0);
    rules.finite(field(path, "roughness_reaction_factor"), value.roughness_reaction_factor);
}

void check(Rules &rules, const QString &path, const PumpEnergyOptions &value)
{
    const QString &currency = value.currency_iso4217;
    const bool currencyValid = currency.isEmpty() ||
        (currency.size() == 3 && std::all_of(currency.begin(), currency.end(),
                                             [](QChar character) { return character >= QLatin1Char('A') &&
                                                                          character <= QLatin1Char('Z'); }));
    if (!currencyValid)
        rules.issue(field(path, "currency_iso4217"), QStringLiteral("must be empty or three letters A-Z"));
    rules.positiveAtMost(field(path, "global_pump_efficiency_percent"), value.global_pump_efficiency_percent, 100.0);
    rules.atLeast(field(path, "global_energy_price_per_kw_h"), value.global_energy_price_per_kw_h, 0.0);
    rules.atLeast(field(path, "demand_charge_per_kw"), value.demand_charge_per_kw, 0.0);
}

void check(Rules &rules, const QString &path, const MultiSpeciesConstant &value)
{
    rules.finite(field(path, "value"), value.value);
}

void check(Rules &rules, const QString &path, const MultiSpeciesGlobalInitialQuality &value)
{
    rules.atLeast(field(path, "value"), value.value, 0.0);
}

void check(Rules &rules, const QString &path, const MultiSpeciesNodeInitialQuality &value)
{
    rules.atLeast(field(path, "value"), value.value, 0.0);
}

void check(Rules &rules, const QString &path, const MultiSpeciesPipeInitialQuality &value)
{
    rules.atLeast(field(path, "value"), value.value, 0.0);
}

void check(Rules &rules, const QString &path, const MultiSpeciesNodeSource &value)
{
    rules.atLeast(field(path, "value"), value.value, 0.0);
}

void check(Rules &rules, const QString &path, const MultiSpeciesOptions &value)
{
    rules.positive(field(path, "timestep_s"), value.timestep_s);
    rules.atLeast(field(path, "peclet_number_threshold"), value.peclet_number_threshold, 0.0);
    rules.integerAtLeast(field(path, "maximum_segments"), value.maximum_segments, 1);
    rules.positive(field(path, "default_absolute_tolerance"), value.default_absolute_tolerance);
    rules.positive(field(path, "default_relative_tolerance"), value.default_relative_tolerance);
}

void check(Rules &rules, const QString &path, const MultiSpeciesParameter &value)
{
    rules.finite(field(path, "default_value"), value.default_value);
}

void check(Rules &rules, const QString &path, const MultiSpeciesParameterOverridePipe &value)
{
    rules.finite(field(path, "value"), value.value);
}

void check(Rules &rules, const QString &path, const MultiSpeciesParameterOverrideTank &value)
{
    rules.finite(field(path, "value"), value.value);
}

void check(Rules &rules, const QString &path, const MultiSpeciesPattern &value)
{
    for (qsizetype index = 0; index < value.multipliers.size(); ++index)
        rules.finite(item(path, "multipliers", index), value.multipliers[index]);
}

void check(Rules &rules, const QString &path, const MultiSpeciesSpecies &value)
{
    // 0 selects the default tolerances of MultiSpeciesOptions.
    rules.atLeast(field(path, "absolute_tolerance"), value.absolute_tolerance, 0.0);
    rules.atLeast(field(path, "relative_tolerance"), value.relative_tolerance, 0.0);
    rules.atLeast(field(path, "molecular_diffusivity_m2_per_s"), value.molecular_diffusivity_m2_per_s, 0.0);
    rules.atLeast(field(path, "longitudinal_dispersion_coefficient_m2_per_s"),
                  value.longitudinal_dispersion_coefficient_m2_per_s, 0.0);
}

template<typename T>
void checkList(Rules &rules, const QString &path, const char *name, const QList<T> &list)
{
    for (qsizetype index = 0; index < list.size(); ++index)
        check(rules, item(path, name, index), list[index]);
}

void check(Rules &rules, const QString &path, const NetworkMultiSpecies &value)
{
    check(rules, field(path, "options"), value.options);
    checkList(rules, path, "species", value.species);
    checkList(rules, path, "constants", value.constants);
    checkList(rules, path, "parameters", value.parameters);
    checkList(rules, path, "parameter_overrides_pipes", value.parameter_overrides_pipes);
    checkList(rules, path, "parameter_overrides_tanks", value.parameter_overrides_tanks);
    checkList(rules, path, "patterns", value.patterns);
    checkList(rules, path, "initial_quality_global", value.initial_quality_global);
    checkList(rules, path, "initial_quality_nodes", value.initial_quality_nodes);
    checkList(rules, path, "initial_quality_pipes", value.initial_quality_pipes);
    checkList(rules, path, "sources", value.sources);
}

QString entityPath(const char *kind, const QUuid &uuid)
{
    return QLatin1String(kind) + QLatin1Char('[') + uuid.toString(QUuid::WithoutBraces) + QLatin1Char(']');
}

template<typename T>
void checkEntities(Rules &rules, const char *kind, const QList<T> &list)
{
    for (const T &entity : list)
        check(rules, entityPath(kind, entity.uuid), entity);
}

void check(Rules &rules, const QString &path, const NetworkHydraulic &value)
{
    rules.unbounded(field(path, "duration_s"), value.duration_s);
    rules.positiveDuration(field(path, "timestep_hydraulic_s"), value.timestep_hydraulic_s);
    rules.positiveDuration(field(path, "timestep_quality_s"), value.timestep_quality_s);
    rules.positiveDuration(field(path, "timestep_pattern_s"), value.timestep_pattern_s);
    rules.unbounded(field(path, "start_pattern_s"), value.start_pattern_s);
    rules.positiveDuration(field(path, "timestep_report_s"), value.timestep_report_s);
    rules.unbounded(field(path, "start_report_s"), value.start_report_s);
    rules.positiveDuration(field(path, "timestep_rule_s"), value.timestep_rule_s);
    rules.timeOfDay(field(path, "start_time_of_day_s"), value.start_time_of_day_s);
    check(rules, field(path, "options_hydraulic"), value.options_hydraulic);
    check(rules, field(path, "options_reaction"), value.options_reaction);
    check(rules, field(path, "options_energy"), value.options_energy);
    check(rules, field(path, "options_report"), value.options_report);
    check(rules, field(path, "multi_species"), value.multi_species);
    checkEntities(rules, "pipe_material", value.pipe_materials);
    checkEntities(rules, "pattern_time", value.patterns_time);
    checkEntities(rules, "curve_tank_volume", value.curves_tank_volume);
    checkEntities(rules, "curve_pump_head", value.curves_pump_head);
    checkEntities(rules, "curve_pump_efficiency", value.curves_pump_efficiency);
    checkEntities(rules, "curve_valve_headloss", value.curves_valve_headloss);
    checkEntities(rules, "curve_valve_characteristic", value.curves_valve_characteristic);
    checkEntities(rules, "curve_generic", value.curves_generic);
    checkEntities(rules, "reservoir", value.nodes_reservoirs);
    checkEntities(rules, "junction", value.nodes_junctions);
    checkEntities(rules, "tank", value.nodes_tanks);
    checkEntities(rules, "demand_point", value.demand_points);
    checkEntities(rules, "pipe", value.links_pipes);
    checkEntities(rules, "pump", value.links_pumps);
    checkEntities(rules, "valve", value.links_valves);
    checkEntities(rules, "control_simple", value.controls_simple);
    checkEntities(rules, "control_rule", value.controls_rules);
    checkEntities(rules, "map_label", value.map_labels);
    check(rules, field(path, "map_backdrop"), value.map_backdrop);
}

template<typename T>
ModelValidationReport entityReport(const char *kind, const T &entity)
{
    ModelValidationReport report;
    Rules rules(report);
    check(rules, entityPath(kind, entity.uuid), entity);
    return report;
}
} // namespace

ModelValidationReport validateHydraulicValues(const NetworkHydraulic &network)
{
    ModelValidationReport report;
    Rules rules(report);
    check(rules, QStringLiteral("network"), network);
    return report;
}

ModelValidationReport validateHydraulicEntityValues(const HydraulicPipeMaterial &entity)
{ return entityReport("pipe_material", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicPatternTime &entity)
{ return entityReport("pattern_time", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveTankVolume &entity)
{ return entityReport("curve_tank_volume", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurvePumpHead &entity)
{ return entityReport("curve_pump_head", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurvePumpEfficiency &entity)
{ return entityReport("curve_pump_efficiency", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveValveHeadloss &entity)
{ return entityReport("curve_valve_headloss", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveValveCharacteristic &entity)
{ return entityReport("curve_valve_characteristic", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicCurveGeneric &entity)
{ return entityReport("curve_generic", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeReservoir &entity)
{ return entityReport("reservoir", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeJunction &entity)
{ return entityReport("junction", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicNodeTank &entity)
{ return entityReport("tank", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicDemandPoint &entity)
{ return entityReport("demand_point", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkPipe &entity)
{ return entityReport("pipe", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkPump &entity)
{ return entityReport("pump", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicLinkValve &entity)
{ return entityReport("valve", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicControlSimple &entity)
{ return entityReport("control_simple", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicControlRule &entity)
{ return entityReport("control_rule", entity); }
ModelValidationReport validateHydraulicEntityValues(const HydraulicMapLabel &entity)
{ return entityReport("map_label", entity); }

} // namespace aowis::dvmdb
