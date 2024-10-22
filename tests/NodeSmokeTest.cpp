#include "MessageSinkUtils.h"
#include "TestUtils.h"
#include "api/runner_creation.h"
#include "basecall/CRFModelConfig.h"
#include "demux/adapter_info.h"
#include "demux/barcoding_info.h"
#include "demux/parse_custom_kit.h"
#include "demux/parse_custom_sequences.h"
#include "model_downloader/model_downloader.h"
#include "models/kits.h"
#include "models/models.h"
#include "poly_tail/poly_tail_calculator_selector.h"
#include "read_pipeline/AdapterDetectorNode.h"
#include "read_pipeline/BarcodeClassifierNode.h"
#include "read_pipeline/BasecallerNode.h"
#include "read_pipeline/DefaultClientInfo.h"
#include "read_pipeline/HtsReader.h"
#include "read_pipeline/ModBaseCallerNode.h"
#include "read_pipeline/PolyACalculatorNode.h"
#include "read_pipeline/ReadFilterNode.h"
#include "read_pipeline/ReadToBamTypeNode.h"
#include "read_pipeline/ScalerNode.h"
#include "torch_utils/trim_rapid_adapter.h"
#include "utils/PostCondition.h"
#include "utils/SampleSheet.h"
#include "utils/parameters.h"

#include <torch/cuda.h>

#include <optional>

#if DORADO_CUDA_BUILD
#include "torch_utils/cuda_utils.h"
#endif

#include <ATen/Functions.h>
#include <torch/types.h>
// Catch2 must come after torch since both define CHECK()
#include <catch2/catch.hpp>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <random>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {

// Fixture for smoke testing nodes
template <typename... MessageTs>
class NodeSmokeTestBase {
protected:
    std::minstd_rand m_rng{Catch::rngSeed()};
    std::uniform_real_distribution<> m_dist;

    std::shared_ptr<dorado::DefaultClientInfo> client_info =
            std::make_shared<dorado::DefaultClientInfo>();

    float random_between(float min, float max) {
        typename decltype(m_dist)::param_type range(min, max);
        return float(m_dist(m_rng, range));
    }

    auto make_test_read(std::string read_id) {
        auto read = std::make_unique<dorado::SimplexRead>();
        read->read_common.raw_data = at::rand(size_t(random_between(100, 200)));
        read->read_common.sample_rate = 5000;
        read->read_common.shift = random_between(100, 200);
        read->read_common.scale = random_between(5, 10);
        read->read_common.read_id = std::move(read_id);
        read->read_common.seq = "ACGTACGT";
        read->read_common.qstring = "********";
        read->read_common.num_trimmed_samples = size_t(random_between(10, 100));
        read->read_common.attributes.mux = 2;
        read->read_common.attributes.read_number = 12345;
        read->read_common.attributes.channel_number = 5;
        read->read_common.attributes.start_time = "2017-04-29T09:10:04Z";
        read->read_common.attributes.fast5_filename = "test.fast5";
        read->read_common.client_info = client_info;
        return read;
    }

private:
    std::size_t m_num_reads = 200;
    std::size_t m_num_messages = m_num_reads;
    using ReadMutator = std::function<void(dorado::SimplexReadPtr& read)>;
    ReadMutator m_read_mutator;
    bool m_pipeline_restart = false;

protected:
    void set_num_reads(std::size_t num_reads) { m_num_reads = num_reads; }
    void set_expected_messages(std::size_t num_messages) { m_num_messages = num_messages; }
    void set_read_mutator(ReadMutator mutator) { m_read_mutator = std::move(mutator); }
    void set_pipeline_restart(bool restart) { m_pipeline_restart = restart; }

    template <class NodeType, class... Args>
    void run_smoke_test(Args&&... args) {
        dorado::PipelineDescriptor pipeline_desc;
        std::vector<dorado::Message> messages;
        auto sink = pipeline_desc.add_node<MessageSinkToVector>({}, 100, messages);
        pipeline_desc.add_node<NodeType>({sink}, std::forward<Args>(args)...);
        auto pipeline = dorado::Pipeline::create(std::move(pipeline_desc), nullptr);
        if (m_pipeline_restart) {
            pipeline->terminate(dorado::DefaultFlushOptions());
            pipeline->restart();
        }
        // Throw some reads at it.
        for (std::size_t i = 0; i < m_num_reads; i++) {
            auto read = make_test_read("read_" + std::to_string(i));
            if (m_read_mutator) {
                m_read_mutator(read);
            }
            pipeline->push_message(std::move(read));
        }
        // Wait for them to complete.
        pipeline.reset();
        // Check that we get the expected number of outputs
        CHECK(messages.size() == m_num_messages);
        // Check the message types match
        for (auto& message : messages) {
            CAPTURE(message.index());
            CHECK((std::holds_alternative<MessageTs>(message) || ...));
        }
    }
};

using NodeSmokeTestRead = NodeSmokeTestBase<dorado::SimplexReadPtr>;
using NodeSmokeTestBam = NodeSmokeTestBase<dorado::BamMessage>;

#define DEFINE_TEST(base, name) TEST_CASE_METHOD(base, "SmokeTest: " name, "[SmokeTest]")

// Not introduced until catch2 3.3.0
#ifndef SKIP
#define SKIP(msg)                                      \
    do {                                               \
        std::cerr << "Skipping test: " << msg << '\n'; \
        return;                                        \
    } while (false)
#endif

// Download a model to a temporary directory
TempDir download_model(const std::string& model) {
    // Create a new directory to download the model to
    auto temp_dir = make_temp_dir("model");

    // Download it
    REQUIRE(dorado::model_downloader::download_models(temp_dir.m_path.string(), model));
    return temp_dir;
}

DEFINE_TEST(NodeSmokeTestRead, "ScalerNode") {
    using SampleType = dorado::models::SampleType;
    auto pipeline_restart = GENERATE(false, true);
    auto model_type = GENERATE(SampleType::DNA, SampleType::RNA002, SampleType::RNA004);
    dorado::utils::rapid::Settings trim_rapid_adapter;
    CAPTURE(pipeline_restart);
    CAPTURE(model_type);

    if (model_type == SampleType::RNA002) {
        auto trim_adapter = GENERATE(true, false);
        auto rna_adapter = GENERATE(true, false);
        CAPTURE(trim_adapter);
        CAPTURE(rna_adapter);
        auto adapter_info = std::make_shared<dorado::demux::AdapterInfo>();
        adapter_info->trim_adapters = trim_adapter;
        adapter_info->trim_adapters = rna_adapter;
        client_info->contexts().register_context<const dorado::demux::AdapterInfo>(
                std::move(adapter_info));
    }

    set_pipeline_restart(pipeline_restart);

    // Scaler node expects i16 input
    set_read_mutator([model_type](dorado::SimplexReadPtr& read) {
        read->read_common.raw_data = read->read_common.raw_data.to(torch::kI16);
        read->read_common.is_rna_model = model_type != SampleType::DNA;
    });

    dorado::basecall::SignalNormalisationParams config;
    config.strategy = dorado::basecall::ScalingStrategy::QUANTILE;
    config.quantile.quantile_a = 0.2f;
    config.quantile.quantile_b = 0.9f;
    config.quantile.shift_multiplier = 0.51f;
    config.quantile.scale_multiplier = 0.53f;
    run_smoke_test<dorado::ScalerNode>(config, model_type, trim_rapid_adapter, 2, 1000);
}

DEFINE_TEST(NodeSmokeTestRead, "BasecallerNode") {
    auto gpu = GENERATE(true, false);
    CAPTURE(gpu);
    auto pipeline_restart = GENERATE(false, true);
    CAPTURE(pipeline_restart);
    auto model_name = GENERATE("dna_r10.4.1_e8.2_400bps_fast@v4.2.0", "rna004_130bps_fast@v3.0.1");

    set_pipeline_restart(pipeline_restart);

    // BasecallerNode will skip reads that have already been basecalled.
    set_read_mutator([](dorado::SimplexReadPtr& read) { read->read_common.seq.clear(); });

    const auto& default_params = dorado::utils::default_parameters;
    const auto model_dir = download_model(model_name);
    const auto model_path = (model_dir.m_path / model_name).string();
    auto model_config = dorado::basecall::load_crf_model_config(model_path);

    // Use a fixed batch size otherwise we slow down CI autobatchsizing.
    int batch_size = 128;

    std::string device;
    if (gpu) {
#if DORADO_METAL_BUILD
        device = "metal";
#elif DORADO_CUDA_BUILD
        device = "cuda:all";
        std::vector<std::string> devices;
        std::string error_message;
        if (!dorado::utils::try_parse_cuda_device_string(device, devices, error_message) ||
            devices.empty()) {
            SKIP("No CUDA devices found: " << error_message);
        }
#else
        SKIP("Can't test GPU without DORADO_GPU_BUILD");
#endif
    } else {
        // CPU processing is very slow, so reduce the number of test reads we throw at it.
        set_num_reads(5);
        set_expected_messages(5);
        batch_size = 8;
        device = "cpu";
    }

    // Pass the batchsize into the config
    model_config.basecaller.set_batch_size(batch_size);
    model_config.normalise_basecaller_params();

    // Create runners
    auto [runners, num_devices] = dorado::api::create_basecall_runners(
            {model_config, device, 1.f, dorado::api::PipelineType::simplex, 0.f, false, false},
            default_params.num_runners, 1);
    CHECK(num_devices != 0);
    run_smoke_test<dorado::BasecallerNode>(std::move(runners),
                                           dorado::utils::default_parameters.overlap, model_name,
                                           1000, "BasecallerNode", 0);
}

DEFINE_TEST(NodeSmokeTestRead, "ModBaseCallerNode") {
    auto gpu = GENERATE(true, false);
    CAPTURE(gpu);
    auto pipeline_restart = GENERATE(false, true);
    CAPTURE(pipeline_restart);

    set_pipeline_restart(pipeline_restart);

    const auto& default_params = dorado::utils::default_parameters;
    const char remora_model_name[] = "dna_r10.4.1_e8.2_400bps_fast@v4.2.0_5mCG_5hmCG@v2";
    const auto remora_model_dir = download_model(remora_model_name);
    const auto remora_model = remora_model_dir.m_path / remora_model_name;

    // Add a second model into the mix
    const char remora_model_6mA_name[] = "dna_r10.4.1_e8.2_400bps_sup@v4.2.0_6mA@v3";
    const auto remora_model_6mA_dir = download_model(remora_model_6mA_name);
    const auto remora_model_6mA = remora_model_6mA_dir.m_path / remora_model_6mA_name;

    // The model stride for RemoraCaller isn't in its config so grab it separately.
    // Note: We only look at the stride of one of the models since it's not what we're
    // testing for. In theory we could hardcode the stride to any number here, but to
    // be somewhat realistic we'll use an actual one.
    const char model_name[] = "dna_r10.4.1_e8.2_400bps_fast@v4.2.0";
    const auto model_dir = download_model(model_name);
    const std::size_t model_stride =
            dorado::basecall::load_crf_model_config(model_dir.m_path / model_name).stride;

    // Create runners
    std::string device;
    int batch_size = default_params.remora_batchsize;
    if (gpu) {
#if DORADO_METAL_BUILD
        device = "metal";
#elif DORADO_CUDA_BUILD
        device = "cuda:all";
        std::vector<std::string> devices;
        std::string error_message;
        if (!dorado::utils::try_parse_cuda_device_string(device, devices, error_message) ||
            devices.empty()) {
            SKIP("No CUDA devices found: " << error_message);
        }
#else
        SKIP("Can't test GPU without DORADO_GPU_BUILD");
#endif
    } else {
        // CPU processing is very slow, so reduce the number of test reads we throw at it.
        set_num_reads(5);
        set_expected_messages(5);
        device = "cpu";
        batch_size = 8;  // reduce batch size so we're not doing work on empty entries
    }

    // ModBase node expects half input and needs a move table
    set_read_mutator([this, model_stride](dorado::SimplexReadPtr& read) {
        read->read_common.raw_data = read->read_common.raw_data.to(torch::kHalf);

        read->read_common.model_stride = int(model_stride);
        // The move table size needs rounding up.
        const size_t move_table_size =
                (read->read_common.get_raw_data_samples() + model_stride - 1) / model_stride;
        read->read_common.moves.resize(move_table_size);
        std::fill_n(read->read_common.moves.begin(), read->read_common.seq.size(), 1);
        // First element must be 1, the rest can be shuffled
        std::shuffle(std::next(read->read_common.moves.begin()), read->read_common.moves.end(),
                     m_rng);
    });

    auto remora_runners = dorado::api::create_modbase_runners(
            {remora_model, remora_model_6mA}, device, default_params.mod_base_runners_per_caller,
            batch_size);

    run_smoke_test<dorado::ModBaseCallerNode>(std::move(remora_runners), 2, model_stride, 1000);
}

DEFINE_TEST(NodeSmokeTestBam, "ReadToBamTypeNode") {
    auto emit_moves = GENERATE(true, false);
    auto pipeline_restart = GENERATE(false, true);
    CAPTURE(emit_moves);
    CAPTURE(pipeline_restart);

    set_pipeline_restart(pipeline_restart);

    run_smoke_test<dorado::ReadToBamTypeNode>(
            emit_moves, 2, dorado::utils::default_parameters.methylation_threshold, nullptr, 1000);
}

struct BarcodeKitInputs {
    std::string kit_name;
    std::string custom_kit;
    std::string custom_sequences;
};

DEFINE_TEST(NodeSmokeTestRead, "BarcodeClassifierNode") {
    auto barcode_both_ends = GENERATE(true, false);
    auto no_trim = GENERATE(true, false);
    auto pipeline_restart = GENERATE(false, true);
    auto kit_inputs =
            GENERATE(BarcodeKitInputs{"SQK-RPB004", "", ""},
                     BarcodeKitInputs{"",
                                      (fs::path(get_data_dir("barcode_demux/custom_barcodes")) /
                                       "test_kit_single_ended.toml")
                                              .string(),
                                      std::string{}},
                     BarcodeKitInputs{"",
                                      (fs::path(get_data_dir("barcode_demux/custom_barcodes")) /
                                       "test_kit_single_ended.toml")
                                              .string(),
                                      (fs::path(get_data_dir("barcode_demux/custom_barcodes")) /
                                       "test_sequences.fasta")
                                              .string()});
    CAPTURE(barcode_both_ends);
    CAPTURE(no_trim);
    CAPTURE(kit_inputs);
    CAPTURE(pipeline_restart);

    if (!kit_inputs.custom_kit.empty()) {
        auto kit_info = dorado::demux::parse_custom_arrangement(kit_inputs.custom_kit);
        REQUIRE(kit_info.has_value());
        dorado::barcode_kits::add_custom_barcode_kit(kit_info->first, kit_info->second);
    }
    auto kit_cleanup =
            dorado::utils::PostCondition([] { dorado::barcode_kits::clear_custom_barcode_kits(); });

    if (!kit_inputs.custom_sequences.empty()) {
        auto custom_barcodes = dorado::demux::parse_custom_sequences(kit_inputs.custom_sequences);
        dorado::barcode_kits::add_custom_barcodes(custom_barcodes);
    }
    auto barcode_cleanup =
            dorado::utils::PostCondition([] { dorado::barcode_kits::clear_custom_barcodes(); });

    auto barcoding_info = std::make_shared<dorado::demux::BarcodingInfo>();
    barcoding_info->kit_name = kit_inputs.kit_name;
    barcoding_info->barcode_both_ends = barcode_both_ends;
    barcoding_info->trim = !no_trim;
    client_info->contexts().register_context<const dorado::demux::BarcodingInfo>(
            std::move(barcoding_info));

    set_pipeline_restart(pipeline_restart);

    run_smoke_test<dorado::BarcodeClassifierNode>(2);
}

DEFINE_TEST(NodeSmokeTestRead, "AdapterDetectorNode") {
    auto adapter_info = std::make_shared<dorado::demux::AdapterInfo>();
    adapter_info->custom_seqs = std::nullopt;
    adapter_info->trim_adapters = GENERATE(false, true);
    adapter_info->trim_primers = GENERATE(false, true);
    auto pipeline_restart = GENERATE(false, true);
    CAPTURE(adapter_info->trim_adapters);
    CAPTURE(adapter_info->trim_primers);
    CAPTURE(pipeline_restart);

    client_info->contexts().register_context<const dorado::demux::AdapterInfo>(
            std::move(adapter_info));

    set_pipeline_restart(pipeline_restart);
    run_smoke_test<dorado::AdapterDetectorNode>(2);
}

TEST_CASE("BarcodeClassifierNode: test simple pipeline with fastq and sam files", "[SmokeTest]") {
    dorado::PipelineDescriptor pipeline_desc;
    std::vector<dorado::Message> messages;
    auto sink = pipeline_desc.add_node<MessageSinkToVector>({}, 100, messages);
    std::string kit = {"EXP-PBC096"};
    bool barcode_both_ends = GENERATE(true, false);
    bool no_trim = GENERATE(true, false);
    pipeline_desc.add_node<dorado::BarcodeClassifierNode>({sink}, 8);
    auto pipeline = dorado::Pipeline::create(std::move(pipeline_desc), nullptr);

    fs::path data1 = fs::path(get_data_dir("barcode_demux/double_end_variant")) /
                     "EXP-PBC096_barcode_both_ends_pass.fastq";
    fs::path data2 = fs::path(get_data_dir("bam_utils")) / "test.sam";
    auto client_info = std::make_shared<dorado::DefaultClientInfo>();
    auto barcoding_info = std::make_shared<dorado::demux::BarcodingInfo>();
    barcoding_info->kit_name = kit;
    barcoding_info->barcode_both_ends = barcode_both_ends;
    barcoding_info->trim = !no_trim;
    client_info->contexts().register_context<const dorado::demux::BarcodingInfo>(
            std::move(barcoding_info));
    for (auto& test_file : {data1, data2}) {
        dorado::HtsReader reader(test_file.string(), std::nullopt);
        reader.set_client_info(client_info);
        reader.read(*pipeline, 0);
    }
}

DEFINE_TEST(NodeSmokeTestRead, "PolyACalculatorNode") {
    auto pipeline_restart = GENERATE(false, true);
    auto is_rna = GENERATE(false, true);
    auto is_rna_adapter = false;
    if (is_rna) {
        is_rna_adapter = GENERATE(false, true);
    }
    CAPTURE(pipeline_restart);
    CAPTURE(is_rna);
    CAPTURE(is_rna_adapter);

    set_pipeline_restart(pipeline_restart);

    client_info->contexts().register_context<const dorado::poly_tail::PolyTailCalculatorSelector>(
            std::make_shared<dorado::poly_tail::PolyTailCalculatorSelector>("", is_rna,
                                                                            is_rna_adapter));

    set_read_mutator([](dorado::SimplexReadPtr& read) {
        read->read_common.model_stride = 2;
        read->read_common.moves = {1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0,
                                   0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 1};
    });

    run_smoke_test<dorado::PolyACalculatorNode>(8, 1000);
}

}  // namespace
