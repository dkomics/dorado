#include "poly_tail_calculator.h"

#include "dna_poly_tail_calculator.h"
#include "plasmid_poly_tail_calculator.h"
#include "poly_tail_config.h"
#include "read_pipeline/messages.h"
#include "rna_poly_tail_calculator.h"
#include "utils/sequence_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

namespace dorado::poly_tail {

namespace {
const int kMaxTailLength = PolyTailCalculator::max_tail_length();
}

std::pair<int, int> PolyTailCalculator::signal_range(int signal_anchor,
                                                     int signal_len,
                                                     float samples_per_base,
                                                     bool fwd) const {
    const int kSpread = int(std::round(samples_per_base * max_tail_length()));
    const float start_scale = fwd ? 1.f : 0.1f;
    const float end_scale = fwd ? 0.1f : 1.f;
    return {std::max(0, static_cast<int>(signal_anchor - kSpread * start_scale)),
            std::min(signal_len, static_cast<int>(signal_anchor + kSpread * end_scale))};
}

float PolyTailCalculator::estimate_samples_per_base(const dorado::SimplexRead& read) const {
    const size_t num_bases = read.read_common.seq.length();
    const auto num_samples = read.read_common.get_raw_data_samples();
    const auto stride = read.read_common.model_stride;
    const auto seq_to_sig_map =
            dorado::utils::moves_to_map(read.read_common.moves, stride, num_samples, num_bases + 1);
    // Store the samples per base in float to use the quantile calcuation function.
    std::vector<float> sizes(seq_to_sig_map.size() - 1, 0.f);
    for (int i = 1; i < int(seq_to_sig_map.size()); i++) {
        sizes[i - 1] = static_cast<float>(seq_to_sig_map[i] - seq_to_sig_map[i - 1]);
    }

    return average_samples_per_base(sizes);
}

std::pair<int, int> PolyTailCalculator::determine_signal_bounds(int signal_anchor,
                                                                bool fwd,
                                                                const dorado::SimplexRead& read,
                                                                float num_samples_per_base) const {
    const c10::Half* signal = static_cast<c10::Half*>(read.read_common.raw_data.data_ptr());
    int signal_len = int(read.read_common.get_raw_data_samples());

    auto calc_stats = [&](int s, int e) -> std::pair<float, float> {
        float avg = 0;
        for (int i = s; i < e; i++) {
            avg += signal[i];
        }
        avg = avg / (e - s);
        float var = 0;
        for (int i = s; i < e; i++) {
            var += (signal[i] - avg) * (signal[i] - avg);
        }
        var = var / (e - s);
        return {avg, std::sqrt(var)};
    };

    // Maximum variance between consecutive values to be
    // considered part of the same interval.
    const float kVar = 0.35f;
    // How close the mean values should be for consecutive intervals
    // to be merged.
    const float kMeanValueProximity = 0.25f;
    // Maximum gap between intervals that can be combined.
    const int kMaxSampleGap = int(std::round(num_samples_per_base * 5));
    // Minimum size of intervals considered for merge.
    const int kMinIntervalSizeForMerge = kMaxSampleGap * 2;
    // Floor for average signal value of poly tail.
    const float kMinAvgVal = min_avg_val();

    auto [left_end, right_end] = signal_range(signal_anchor, signal_len, num_samples_per_base, fwd);
    spdlog::trace("Bounds left {}, right {}", left_end, right_end);

    std::vector<std::pair<int, int>> intervals;
    std::pair<float, float> last_interval_stats;
    const int kStride = 3;

    for (int s = left_end; s < (right_end - kMaxSampleGap); s += kStride) {
        const int e = s + kMaxSampleGap;
        auto [avg, stdev] = calc_stats(s, e);
        if (avg > kMinAvgVal && stdev < kVar) {
            if (intervals.empty()) {
                spdlog::trace("Add new interval {}-{} avg {} stdev {}", s, e, avg, stdev);
                intervals.push_back({s, e});
            } else {
                // If new interval overlaps with the previous interval and
                // intervals have a similar mean, just extend the previous interval.
                auto last_interval = intervals.rbegin();
                if (last_interval->second >= s &&
                    std::abs(avg - last_interval_stats.first) < kMeanValueProximity) {
                    // recalc stats for new interval
                    std::tie(avg, stdev) = calc_stats(last_interval->first, e);
                    spdlog::trace("extend interval {}-{} to {}-{} avg {} stdev {}",
                                  last_interval->first, last_interval->second, last_interval->first,
                                  e, avg, stdev);
                    last_interval->second = e;
                } else {
                    spdlog::trace("Add new interval {}-{} avg {} stdev {}", s, e, avg, stdev);
                    intervals.push_back({s, e});
                }
            }
            last_interval_stats = {avg, stdev};
        }
    }

    std::string int_str = "";
    for (const auto& in : intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::trace("found intervals {}", int_str);

    // Cluster intervals if there are interrupted poly tails that should
    // be combined. Interruption length is specified through a config file.
    // In the example below, tail estimation should include both stretches
    // of As along with the small gap in the middle.
    // e.g. -----AAAAAAA--AAAAAA-----
    const int kMaxInterruption =
            static_cast<int>(std::round(num_samples_per_base * m_config.tail_interrupt_length));

    std::vector<std::pair<int, int>> clustered_intervals;
    for (const auto& i : intervals) {
        if (clustered_intervals.empty()) {
            clustered_intervals.push_back(i);
        } else {
            auto& last = clustered_intervals.back();
            auto [avg_1, std_1] = calc_stats(last.first, last.second);
            auto [avg_2, std_2] = calc_stats(i.first, i.second);
            bool mean_proximity_ok = std::abs(avg_2 - avg_1) < kMeanValueProximity;
            auto separation = i.first - last.second;
            bool skip_glitch = std::abs(separation) < kMaxSampleGap &&
                               last.second - last.first > kMinIntervalSizeForMerge &&
                               (i.second - i.first > kMinIntervalSizeForMerge ||
                                i.second >= right_end - kStride);
            bool allow_linker = separation >= 0 && separation < kMaxInterruption;
            if (mean_proximity_ok && (skip_glitch || allow_linker)) {
                last.second = i.second;
            } else {
                clustered_intervals.push_back(i);
            }
        }
    }

    int_str = "";
    for (const auto& in : clustered_intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::trace("clustered intervals {}", int_str);

    // Once the clustered intervals are available, filter them by how
    // close they are to the anchor.
    std::vector<std::pair<int, int>> filtered_intervals;
    std::copy_if(clustered_intervals.begin(), clustered_intervals.end(),
                 std::back_inserter(filtered_intervals), [&](auto& i) {
                     auto buffer = buffer_range(i, num_samples_per_base);
                     // Only keep intervals that are close-ish to the signal anchor.
                     // i.e. the anchor needs to be within the buffer region of
                     // the interval
                     // <----buffer.first---|--- interval ---|---- buffer.second---->
                     bool within_anchor_dist =
                             (signal_anchor >= std::max(0, i.first - buffer.first)) &&
                             (signal_anchor <= (i.second + buffer.second));
                     bool meets_min_base_count =
                             (i.second - i.first) >=
                             std::round(num_samples_per_base * m_config.min_base_count);

                     return within_anchor_dist && meets_min_base_count;
                 });

    int_str = "";
    for (const auto& in : filtered_intervals) {
        int_str += std::to_string(in.first) + "-" + std::to_string(in.second) + ", ";
    }
    spdlog::trace("filtered intervals {}", int_str);

    if (filtered_intervals.empty()) {
        spdlog::trace("Anchor {} No range within anchor proximity found", signal_anchor);
        return {0, 0};
    }

    // Choose the longest interval. If there is a tie for the longest interval,
    // choose the one that is closest to the anchor.
    auto best_interval = std::max_element(filtered_intervals.begin(), filtered_intervals.end(),
                                          [&](auto& l, auto& r) {
                                              auto l_size = l.second - l.first;
                                              auto r_size = r.second - r.first;
                                              if (l_size != r_size) {
                                                  return l_size < r_size;
                                              } else {
                                                  if (fwd) {
                                                      return std::abs(l.second - signal_anchor) <
                                                             std::abs(r.second - signal_anchor);
                                                  } else {
                                                      return std::abs(l.first - signal_anchor) <
                                                             std::abs(r.first - signal_anchor);
                                                  }
                                              }
                                          });

    spdlog::trace("Anchor {} Range {} {}", signal_anchor, best_interval->first,
                  best_interval->second);

    return *best_interval;
}

int PolyTailCalculator::calculate_num_bases(const SimplexRead& read,
                                            const SignalAnchorInfo& signal_info) const {
    spdlog::trace("{} Strand {}; poly A/T signal anchor {}", read.read_common.read_id,
                  signal_info.is_fwd_strand ? '+' : '-', signal_info.signal_anchor);

    auto num_samples_per_base = estimate_samples_per_base(read);

    // Walk through signal. Require a minimum of length 10 poly-A since below that
    // the current algorithm returns a lot of false intervals.
    auto [signal_start, signal_end] = determine_signal_bounds(
            signal_info.signal_anchor, signal_info.is_fwd_strand, read, num_samples_per_base);

    auto signal_len = signal_end - signal_start;
    signal_len -= signal_length_adjustment(read, signal_len);

    int num_bases = int(std::round(static_cast<float>(signal_len) / num_samples_per_base)) -
                    signal_info.trailing_adapter_bases;

    spdlog::trace(
            "{} PolyA bases {}, signal anchor {} Signal range is {} {} Signal length "
            "{}, samples/base {} trim {} read len {}",
            read.read_common.read_id, num_bases, signal_info.signal_anchor, signal_start,
            signal_end, signal_len, num_samples_per_base, read.read_common.num_trimmed_samples,
            read.read_common.seq.length());

    return num_bases;
}

std::shared_ptr<const PolyTailCalculator>
PolyTailCalculatorFactory::create(const PolyTailConfig& config, bool is_rna, bool is_rna_adapter) {
    if (is_rna) {
        return std::make_unique<RNAPolyTailCalculator>(config, is_rna_adapter);
    }
    if (config.is_plasmid) {
        return std::make_unique<PlasmidPolyTailCalculator>(config);
    }
    return std::make_unique<DNAPolyTailCalculator>(config);
}

}  // namespace dorado::poly_tail
