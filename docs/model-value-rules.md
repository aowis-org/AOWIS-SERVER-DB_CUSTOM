# Model value rules

Every hydraulic model value is checked before it is stored, and again when it is read.

**Principle.** Impossible values are rejected. Values a draft has not set yet are accepted: zero, empty lists and null references. Whether a model is complete enough to simulate is checked when it is exported to a simulator, not here. For example, a valve with diameter 0 can be stored, but a valve with diameter −1 cannot.

## Where the rules apply

| Layer | What it enforces | Result |
|---|---|---|
| Codec (`src/model_codec.cpp`), encode and decode | Every enum holds a declared enumerator. Every `double` is finite (no NaN, no ±∞), in every field, list element and optional. | Encode returns empty bytes; decode fails ("…non-finite number or undeclared enumerator"). An HTTP payload is 400; a stored record is corruption. |
| Value rules (`src/model_value_validation.cpp`) | The per-field ranges below, with a field path in the message. | `saveHydraulicNetwork`, every `editHydraulicNetwork` path (including all indexed fast paths), merge validation and network loading: `InvalidInput`, HTTP 400. |
| Build (`tools/check_model_codec.py`) | Every numeric Model field, and every struct field containing numbers, is referenced by its rule function. The enumerator table is generated from the Model headers. | CMake configure fails when a new numeric field has no decided range, or when a persisted enum uses conditional enumerators. |

## Ranges

Notation: *finite* means any finite number. *≥ 0* and *> 0* also require the value to be finite. Optional values are checked only when present. Percentages are 0–100.

### Geometry

| Field | Rule |
|---|---|
| `longitude_deg` | −180 … 180 |
| `latitude_deg` | −90 … 90 |
| `altitude_m` | finite |
| Map backdrop: `offset_longitude_deg`, `offset_latitude_deg` | −360 … 360, −180 … 180 |

### Nodes

| Field | Rule |
|---|---|
| Elevations, heads and offsets: junction, reservoir and tank bottom | finite (below sea level is valid) |
| Demand `base_demand_m3_per_h` | finite (negative = inflow) |
| Emitter `coefficient` | ≥ 0 |
| Emitter `pressure_exponent` | > 0 |
| `initial_chemical_concentration_mg_per_l`, `initial_water_age_h` | ≥ 0 |
| Quality source: concentration, mass flow | ≥ 0 |
| Tank levels (minimum, initial, maximum) | ≥ 0, and minimum ≤ initial ≤ maximum |
| Tank `diameter_m`, `cross_section_area_m2`, `volume_at_maximum_level_m3`, `minimum_volume_m3` | ≥ 0 |
| Tank `mixing_fraction` | 0 … 1 |

### Links

| Field | Rule |
|---|---|
| Pipe `length_calculated_m` | ≥ 0 |
| Pipe `length_measured_m` | > 0 |
| Pipe `diameter_mm` | > 0 (default 100) |
| Roughness: Hazen-Williams | > 0 |
| Roughness: Darcy-Weisbach (mm) | ≥ 0 (0 = smooth) |
| Roughness: Chezy-Manning | > 0 |
| `minor_loss_coefficient` (pipes, valves) | ≥ 0 |
| Leak area and its pressure expansion | ≥ 0 |
| Pump `constant_power_kw`, `initial_speed_ratio`, `energy_price_per_kw_h` | ≥ 0 |
| Pump `constant_efficiency_percent` | > 0 … 100 |
| Valve `setting_pressure_head_m` | finite |
| Valve `setting_flow_m3_per_h`, `setting_loss_coefficient`, `diameter_mm` | ≥ 0 (diameter 0 = not set) |
| Valve `setting_position_percent` | 0 … 100 |
| Demand point `attachment.pipe_position` | 0 … 1 |

### Water quality

| Field | Rule |
|---|---|
| Bulk reaction: `coefficient`, `order` | finite (negative coefficient = decay; negative order = Michaelis-Menten) |
| Wall reaction: `coefficient` | finite |
| Wall reaction: `order` | 0 or 1 |
| `limiting_concentration_mg_per_l` | ≥ 0 |
| `roughness_reaction_factor` | finite |

### Curves, patterns and materials

| Field | Rule |
|---|---|
| Every curve | x values strictly increase |
| Pump head: flow, head | ≥ 0 |
| Pump efficiency: flow | ≥ 0 |
| Pump efficiency: efficiency | 0 … 100 |
| Tank volume: level, volume | ≥ 0; volume never decreases with level |
| Valve characteristic: position, relative flow | 0 … 100 |
| Valve headloss: flow, head loss | ≥ 0 |
| Generic curve | finite |
| Pattern multipliers (time and MSX patterns) | finite |
| Material `age_years` | ≥ 0, strictly increasing |
| Material roughness | as for pipes |

### Controls

| Field | Rule |
|---|---|
| Time of day (simple controls, rule premises, network start) | 0 … 86399 s |
| Water levels | ≥ 0 |
| Heads, pressures, demands, flows | finite |
| `power_kw` | ≥ 0 |
| Elapsed, fill and drain times | any |
| Link settings: pump speed, valve flow, valve loss coefficient | ≥ 0 |
| Link settings: valve position | 0 … 100 |
| Link settings: valve pressure | finite |
| Rule `priority` | finite |

### Network options

| Field | Rule |
|---|---|
| Timesteps: hydraulic, quality, pattern, report, rule | > 0 s |
| Duration and start times | any (start of day < 86400 s) |
| `minimum_pressure_head_m`, `required_pressure_head_m` | ≥ 0, required ≥ minimum |
| `pressure_exponent`, `accuracy`, `specific_gravity`, `relative_viscosity` | > 0 |
| `maximum_trials` | ≥ 1 |
| `check_frequency` | ≥ 1 |
| `unbalanced_extra_trials`, `maximum_check` | ≥ 0 |
| `damping_limit`, `maximum_head_error_m`, `maximum_flow_change_m3_per_h`, `demand_multiplier` | ≥ 0 |
| Energy: `currency_iso4217` | empty or three letters A–Z |
| Energy: `global_pump_efficiency_percent` | > 0 … 100 |
| Energy: prices and demand charge | ≥ 0 |
| Report `precision` | 0 … 15 |
| Report `page_size` | ≥ 0 |
| Report thresholds | finite |
| MSX `timestep_s` and default tolerances | > 0 |
| MSX `peclet_number_threshold` | ≥ 0 |
| MSX `maximum_segments` | ≥ 1 |
| MSX species: tolerances, diffusivity, dispersion | ≥ 0 (tolerance 0 = default) |
| MSX initial qualities and source values | ≥ 0 |
| MSX constants, parameters, overrides | finite |

## Changing a rule

1. Edit the field's `check()` in `src/model_value_validation.cpp`.
2. Update this table.
3. Add a valid case and an invalid case to `tests/model_value_rules_tests.cpp`.

A new numeric Model field fails the configure step until the field manifest is updated and the field has a rule (step 1). Steps 2 and 3 are not checked by the build, so do them in the same change.
