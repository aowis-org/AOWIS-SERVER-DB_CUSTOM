#ifndef AOWIS_DVMDB_MODEL_VALIDATION_H
#define AOWIS_DVMDB_MODEL_VALIDATION_H

#include <aowis/model/serialization/model_validation.h>

namespace aowis::dvmdb {
using aowis::model::ModelValidationIssue;
using aowis::model::ModelValidationReport;
using aowis::model::HydraulicReference;
using aowis::model::HydraulicDependency;
using aowis::model::HydraulicDependencyValidation;
using aowis::model::hydraulicReferences;
using aowis::model::validateHydraulicNetworkWithDependencies;
using aowis::model::validateHydraulicNetwork;
using aowis::model::validateHydraulicValues;
using aowis::model::validateHydraulicEntityValues;
}

#endif
