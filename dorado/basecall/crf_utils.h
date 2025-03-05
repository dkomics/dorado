#pragma once

#include "basecall/ModelRunnerBase.h"
#include "config/BasecallModelConfig.h"

#include <torch/nn.h>

#include <filesystem>
#include <string>
#include <vector>

namespace dorado::basecall {

std::vector<at::Tensor> load_crf_model_weights(const config::CRFModelConfig& model_config);

torch::nn::ModuleHolder<torch::nn::AnyModule> load_crf_model(
        const config::CRFModelConfig& model_config,
        const torch::TensorOptions& options);

size_t auto_calculate_num_runners(const config::CRFModelConfig& model_config,
                                  float memory_fraction);

}  // namespace dorado::basecall
