#include <aowis/dvmdb/model_validation.h>

#include <QHash>
#include <QSet>

#include <cmath>
#include <utility>

namespace aowis::dvmdb
{
QString ModelValidationReport::summary() const
{
    if (issues.isEmpty())
        return {};
    return QStringLiteral("%1: %2").arg(issues.first().path, issues.first().description);
}

namespace
{
using UuidSet = QSet<QUuid>;

class NetworkReferenceValidator final
{
public:
    enum class CollectionMode { None, Diagnostic, Compact };

    explicit NetworkReferenceValidator(const NetworkHydraulic &network,
                                       CollectionMode mode = CollectionMode::None)
        : network_(network), mode_(mode)
    {
    }

    QVector<HydraulicReference> takeReferences() { return std::move(this->references_); }
    QVector<HydraulicDependency> takeDependencies() { return std::move(this->dependencies_); }

    ModelValidationReport validate()
    {
        if (this->network_.uuid.isNull())
            issue(QStringLiteral("network.uuid"), QStringLiteral("Network UUID is required"));

        for (const HydraulicNodeJunction &node : this->network_.nodes_junctions)
            registerId(node.uuid, QStringLiteral("nodes_junctions"), this->nodes_);
        for (const HydraulicNodeReservoir &node : this->network_.nodes_reservoirs)
            registerId(node.uuid, QStringLiteral("nodes_reservoirs"), this->nodes_);
        for (const HydraulicNodeTank &node : this->network_.nodes_tanks)
            registerId(node.uuid, QStringLiteral("nodes_tanks"), this->nodes_);
        for (const HydraulicLinkPipe &pipe : this->network_.links_pipes)
            registerId(pipe.uuid, QStringLiteral("links_pipes"), this->pipes_, &this->links_);
        for (const HydraulicLinkPump &pump : this->network_.links_pumps)
            registerId(pump.uuid, QStringLiteral("links_pumps"), this->pumps_, &this->links_);
        for (const HydraulicLinkValve &valve : this->network_.links_valves)
            registerId(valve.uuid, QStringLiteral("links_valves"), this->valves_, &this->links_);
        for (const HydraulicPatternTime &pattern : this->network_.patterns_time)
            registerId(pattern.uuid, QStringLiteral("patterns_time"), this->patterns_);
        for (const HydraulicPipeMaterial &material : this->network_.pipe_materials)
            registerId(material.uuid, QStringLiteral("pipe_materials"), this->materials_);
        for (const HydraulicCurveTankVolume &curve : this->network_.curves_tank_volume)
            registerId(curve.uuid, QStringLiteral("curves_tank_volume"), this->tank_curves_);
        for (const HydraulicCurvePumpHead &curve : this->network_.curves_pump_head)
            registerId(curve.uuid, QStringLiteral("curves_pump_head"), this->pump_curves_);
        for (const HydraulicCurvePumpEfficiency &curve : this->network_.curves_pump_efficiency)
            registerId(curve.uuid, QStringLiteral("curves_pump_efficiency"), this->efficiency_curves_);
        for (const HydraulicCurveValveHeadloss &curve : this->network_.curves_valve_headloss)
            registerId(curve.uuid, QStringLiteral("curves_valve_headloss"), this->headloss_curves_);
        for (const HydraulicCurveValveCharacteristic &curve : this->network_.curves_valve_characteristic)
            registerId(curve.uuid, QStringLiteral("curves_valve_characteristic"), this->characteristic_curves_);
        for (const HydraulicCurveGeneric &curve : this->network_.curves_generic)
            registerId(curve.uuid, QStringLiteral("curves_generic"), this->generic_curves_);

        for (const HydraulicNodeJunction &node : this->network_.nodes_junctions)
        {
            const QString path = context(QStringLiteral("junction"), node.uuid);
            this->current_source_ = node.uuid;
            demands(node.demands, path);
            check(node.quality_source.pattern_uuid, this->patterns_, path, QStringLiteral("quality_source.pattern_uuid"));
        }
        for (const HydraulicNodeReservoir &node : this->network_.nodes_reservoirs)
        {
            const QString path = context(QStringLiteral("reservoir"), node.uuid);
            this->current_source_ = node.uuid;
            check(node.head_pattern_uuid, this->patterns_, path, QStringLiteral("head_pattern_uuid"));
            check(node.quality_source.pattern_uuid, this->patterns_, path, QStringLiteral("quality_source.pattern_uuid"));
        }
        for (const HydraulicNodeTank &node : this->network_.nodes_tanks)
        {
            const QString path = context(QStringLiteral("tank"), node.uuid);
            this->current_source_ = node.uuid;
            check(node.volume_curve_uuid, this->tank_curves_, path, QStringLiteral("volume_curve_uuid"));
            check(node.quality_source.pattern_uuid, this->patterns_, path, QStringLiteral("quality_source.pattern_uuid"));
        }

        for (const HydraulicLinkPipe &pipe : this->network_.links_pipes)
        {
            const QString path = context(QStringLiteral("pipe"), pipe.uuid);
            this->current_source_ = pipe.uuid;
            endpoints(pipe.node_uuid_from, pipe.node_uuid_to, path);
            check(pipe.material_uuid, this->materials_, path, QStringLiteral("material_uuid"));
        }
        for (const HydraulicLinkPump &pump : this->network_.links_pumps)
        {
            const QString path = context(QStringLiteral("pump"), pump.uuid);
            this->current_source_ = pump.uuid;
            endpoints(pump.node_uuid_from, pump.node_uuid_to, path);
            check(pump.head_curve_uuid, this->pump_curves_, path, QStringLiteral("head_curve_uuid"));
            check(pump.speed_pattern_uuid, this->patterns_, path, QStringLiteral("speed_pattern_uuid"));
            check(pump.efficiency_curve_uuid, this->efficiency_curves_, path, QStringLiteral("efficiency_curve_uuid"));
            check(pump.price_pattern_uuid, this->patterns_, path, QStringLiteral("price_pattern_uuid"));
        }
        for (const HydraulicLinkValve &valve : this->network_.links_valves)
        {
            const QString path = context(QStringLiteral("valve"), valve.uuid);
            this->current_source_ = valve.uuid;
            endpoints(valve.node_uuid_from, valve.node_uuid_to, path);
            check(valve.head_loss_curve_uuid, this->headloss_curves_, path, QStringLiteral("head_loss_curve_uuid"));
            check(valve.characteristic_curve_uuid, this->characteristic_curves_, path, QStringLiteral("characteristic_curve_uuid"));
        }
        for (const HydraulicDemandPoint &point : this->network_.demand_points)
        {
            const QString path = context(QStringLiteral("demand_point"), point.uuid);
            this->current_source_ = point.uuid;
            demands(point.demands, path);
            if (point.attachment.type == HydraulicDemandPointAttachmentType::Pipe)
            {
                check(point.attachment.pipe_uuid, this->pipes_, path, QStringLiteral("attachment.pipe_uuid"));
                if (!std::isfinite(point.attachment.pipe_position) || point.attachment.pipe_position < 0.0 || point.attachment.pipe_position > 1.0)
                    issue(path + QStringLiteral(".attachment.pipe_position"), QStringLiteral("Pipe position must be finite and between 0 and 1"));
                if (point.attachment.pipe_allocation_mode == HydraulicDemandPointPipeAllocationMode::AssignedJunction)
                {
                    check(point.attachment.pipe_assigned_junction_uuid, junctions(), path, QStringLiteral("attachment.pipe_assigned_junction_uuid"));
                    if (!point.attachment.pipe_uuid.isNull() && !point.attachment.pipe_assigned_junction_uuid.isNull())
                    {
                        const HydraulicLinkPipe *pipe = findPipe(point.attachment.pipe_uuid);
                        if (pipe != nullptr && pipe->node_uuid_from != point.attachment.pipe_assigned_junction_uuid &&
                            pipe->node_uuid_to != point.attachment.pipe_assigned_junction_uuid)
                            issue(path + QStringLiteral(".attachment.pipe_assigned_junction_uuid"), QStringLiteral("Assigned junction must be an endpoint of the attached pipe"));
                    }
                }
            }
            else if (point.attachment.type == HydraulicDemandPointAttachmentType::Junction)
                check(point.attachment.junction_uuid, junctions(), path, QStringLiteral("attachment.junction_uuid"));
        }
        for (const HydraulicControlSimple &control : this->network_.controls_simple)
        {
            const QString path = context(QStringLiteral("control_simple"), control.uuid);
            this->current_source_ = control.uuid;
            check(control.link_uuid, this->links_, path, QStringLiteral("link_uuid"));
            check(control.trigger_node_uuid, this->nodes_, path, QStringLiteral("trigger_node_uuid"));
        }
        for (const HydraulicControlRule &rule : this->network_.controls_rules)
        {
            const QString path = context(QStringLiteral("control_rule"), rule.uuid);
            this->current_source_ = rule.uuid;
            for (const HydraulicControlRulePremise &premise : rule.premises)
            {
                if (premise.object == HydraulicControlRuleObject::Node)
                    check(premise.object_uuid, this->nodes_, path, QStringLiteral("premises.object_uuid"));
                else if (premise.object == HydraulicControlRuleObject::Link)
                    check(premise.object_uuid, this->links_, path, QStringLiteral("premises.object_uuid"));
            }
            for (const HydraulicControlRuleAction &action : rule.actions_then)
                check(action.link_uuid, this->links_, path, QStringLiteral("actions_then.link_uuid"));
            for (const HydraulicControlRuleAction &action : rule.actions_else)
                check(action.link_uuid, this->links_, path, QStringLiteral("actions_else.link_uuid"));
        }
        for (const HydraulicMapLabel &label : this->network_.map_labels)
        {
            this->current_source_ = label.uuid;
            check(label.anchor_node_uuid, this->nodes_, context(QStringLiteral("map_label"), label.uuid), QStringLiteral("anchor_node_uuid"));
        }
        this->current_source_ = this->network_.uuid;
        check(this->network_.options_hydraulic.default_demand_pattern_uuid, this->patterns_, QStringLiteral("network"), QStringLiteral("options_hydraulic.default_demand_pattern_uuid"));
        check(this->network_.options_energy.global_energy_price_pattern_uuid, this->patterns_, QStringLiteral("network"), QStringLiteral("options_energy.global_energy_price_pattern_uuid"));
        if (this->network_.options_report.selection_nodes.mode == HydraulicSimulationReportSelectionMode::Selected)
            for (const QUuid &uuid : this->network_.options_report.selection_nodes.uuids)
                check(uuid, this->nodes_, QStringLiteral("network"), QStringLiteral("options_report.selection_nodes.uuids"));
        if (this->network_.options_report.selection_links.mode == HydraulicSimulationReportSelectionMode::Selected)
            for (const QUuid &uuid : this->network_.options_report.selection_links.uuids)
                check(uuid, this->links_, QStringLiteral("network"), QStringLiteral("options_report.selection_links.uuids"));
        msx();
        // Physical value rules belong to the same canonical validation.
        this->result_.issues.append(validateHydraulicValues(this->network_).issues);
        return this->result_;
    }

private:
    QString context(const QString &kind, const QUuid &uuid) const
    {
        return kind + QStringLiteral("[") + uuid.toString(QUuid::WithoutBraces) + QStringLiteral("]");
    }
    void issue(const QString &path, const QString &message)
    {
        this->result_.issues.append({path, message});
    }
    void registerId(const QUuid &uuid, const QString &kind, UuidSet &category, UuidSet *other = nullptr)
    {
        if (uuid.isNull())
        {
            issue(kind, QStringLiteral("Entity UUID must not be null"));
            return;
        }
        if (category.contains(uuid))
            issue(kind, QStringLiteral("Duplicate UUID: %1").arg(uuid.toString()));
        category.insert(uuid);
        if (other != nullptr)
            other->insert(uuid);
        if (kind == QStringLiteral("nodes_junctions")) this->junctions_.insert(uuid);
    }
    const UuidSet &junctions() const { return this->junctions_; }
    void check(const QUuid &uuid, const UuidSet &valid, const QString &path, const QString &field)
    {
        if (!uuid.isNull())
        {
            if (this->mode_ == CollectionMode::Diagnostic)
                this->references_.append({this->current_source_, uuid, path + QStringLiteral(".") + field});
            else if (this->mode_ == CollectionMode::Compact)
                this->dependencies_.append({this->current_source_, uuid});
        }
        if (!uuid.isNull() && !valid.contains(uuid))
            issue(path + QStringLiteral(".") + field, QStringLiteral("Dangling or wrong-type reference to %1").arg(uuid.toString()));
    }
    void endpoints(const QUuid &from, const QUuid &to, const QString &path)
    {
        check(from, this->nodes_, path, QStringLiteral("node_uuid_from"));
        check(to, this->nodes_, path, QStringLiteral("node_uuid_to"));
    }
    void demands(const QList<HydraulicDemand> &list, const QString &path)
    {
        for (const HydraulicDemand &demand : list)
            check(demand.pattern_uuid, this->patterns_, path, QStringLiteral("demands.pattern_uuid"));
    }
    const HydraulicLinkPipe *findPipe(const QUuid &uuid) const
    {
        for (const HydraulicLinkPipe &pipe : this->network_.links_pipes)
            if (pipe.uuid == uuid) return &pipe;
        return nullptr;
    }
    void msx()
    {
        this->current_source_ = this->network_.uuid;
        const NetworkMultiSpecies &msx = this->network_.multi_species;
        UuidSet species, parameters, patterns;
        for (const MultiSpeciesSpecies &item : msx.species)
            registerLocal(item.uuid, QStringLiteral("msx.species"), species);
        for (const MultiSpeciesParameter &item : msx.parameters)
            registerLocal(item.uuid, QStringLiteral("msx.parameters"), parameters);
        for (const MultiSpeciesPattern &item : msx.patterns)
            registerLocal(item.uuid, QStringLiteral("msx.patterns"), patterns);
        for (const MultiSpeciesParameterOverridePipe &item : msx.parameter_overrides_pipes)
        {
            check(item.pipe_uuid, this->pipes_, QStringLiteral("msx.parameter_overrides_pipes"), QStringLiteral("pipe_uuid"));
            check(item.parameter_uuid, parameters, QStringLiteral("msx.parameter_overrides_pipes"), QStringLiteral("parameter_uuid"));
        }
        for (const MultiSpeciesParameterOverrideTank &item : msx.parameter_overrides_tanks)
        {
            check(item.tank_uuid, tanks(), QStringLiteral("msx.parameter_overrides_tanks"), QStringLiteral("tank_uuid"));
            check(item.parameter_uuid, parameters, QStringLiteral("msx.parameter_overrides_tanks"), QStringLiteral("parameter_uuid"));
        }
        for (const MultiSpeciesReaction &item : msx.reactions)
            check(item.species_uuid, species, QStringLiteral("msx.reactions"), QStringLiteral("species_uuid"));
        for (const MultiSpeciesGlobalInitialQuality &item : msx.initial_quality_global)
            check(item.species_uuid, species, QStringLiteral("msx.initial_quality_global"), QStringLiteral("species_uuid"));
        for (const MultiSpeciesNodeInitialQuality &item : msx.initial_quality_nodes)
        {
            check(item.node_uuid, this->nodes_, QStringLiteral("msx.initial_quality_nodes"), QStringLiteral("node_uuid"));
            check(item.species_uuid, species, QStringLiteral("msx.initial_quality_nodes"), QStringLiteral("species_uuid"));
        }
        for (const MultiSpeciesPipeInitialQuality &item : msx.initial_quality_pipes)
        {
            check(item.pipe_uuid, this->pipes_, QStringLiteral("msx.initial_quality_pipes"), QStringLiteral("pipe_uuid"));
            check(item.species_uuid, species, QStringLiteral("msx.initial_quality_pipes"), QStringLiteral("species_uuid"));
        }
        for (const MultiSpeciesNodeSource &item : msx.sources)
        {
            check(item.node_uuid, this->nodes_, QStringLiteral("msx.sources"), QStringLiteral("node_uuid"));
            check(item.species_uuid, species, QStringLiteral("msx.sources"), QStringLiteral("species_uuid"));
            check(item.pattern_uuid, patterns, QStringLiteral("msx.sources"), QStringLiteral("pattern_uuid"));
        }
    }
    void registerLocal(const QUuid &uuid, const QString &path, UuidSet &ids)
    {
        if (uuid.isNull() || ids.contains(uuid))
            issue(path, QStringLiteral("Null or duplicate nested UUID"));
        ids.insert(uuid);
    }
    UuidSet tanks() const
    {
        UuidSet result;
        for (const HydraulicNodeTank &tank : this->network_.nodes_tanks)
            result.insert(tank.uuid);
        return result;
    }

    const NetworkHydraulic &network_;
    ModelValidationReport result_;
    QVector<HydraulicReference> references_;
    QVector<HydraulicDependency> dependencies_;
    CollectionMode mode_;
    QUuid current_source_;
    UuidSet nodes_, junctions_, pipes_, pumps_, valves_, links_;
    UuidSet patterns_, materials_, tank_curves_, pump_curves_, efficiency_curves_, headloss_curves_, characteristic_curves_, generic_curves_;
};
} // namespace

ModelValidationReport validateHydraulicNetwork(const NetworkHydraulic &network)
{
    return NetworkReferenceValidator(network).validate();
}

QVector<HydraulicReference> hydraulicReferences(const NetworkHydraulic &network)
{
    NetworkReferenceValidator validator(network, NetworkReferenceValidator::CollectionMode::Diagnostic);
    validator.validate(); // Same traversal as canonical reference validation.
    return validator.takeReferences();
}

HydraulicDependencyValidation validateHydraulicNetworkWithDependencies(
    const NetworkHydraulic &network)
{
    NetworkReferenceValidator validator(network, NetworkReferenceValidator::CollectionMode::Compact);
    HydraulicDependencyValidation result;
    result.report = validator.validate();
    result.dependencies = validator.takeDependencies();
    return result;
}

} // namespace aowis::dvmdb
