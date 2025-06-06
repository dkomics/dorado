#include "trim.h"

#include "utils/ssize.h"

#include <spdlog/spdlog.h>

#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace dorado::polisher {

namespace {

enum class Relationship {
    DIFFERENT_REF_NAME,
    FORWARD_OVERLAP,
    REVERSE_OVERLAP,
    FORWARD_ABUTTED,
    REVERSE_ABUTTED,
    FORWARD_GAPPED,
    REVERSE_GAPPED,
    S2_WITHIN_S1,
    S1_WITHIN_S2,
    UNKNOWN,
};

// clang-format off
std::ostream& operator<<(std::ostream& os, const Relationship rel) {
    switch (rel) {
        case Relationship::DIFFERENT_REF_NAME: os << "DIFFERENT_REF_NAME"; break;
        case Relationship::FORWARD_OVERLAP: os << "FORWARD_OVERLAP"; break;
        case Relationship::REVERSE_OVERLAP: os << "REVERSE_OVERLAP"; break;
        case Relationship::FORWARD_ABUTTED: os << "FORWARD_ABUTTED"; break;
        case Relationship::REVERSE_ABUTTED: os << "REVERSE_ABUTTED"; break;
        case Relationship::FORWARD_GAPPED: os << "FORWARD_GAPPED"; break;
        case Relationship::REVERSE_GAPPED: os << "REVERSE_GAPPED"; break;
        case Relationship::S2_WITHIN_S1: os << "S2_WITHIN_S1"; break;
        case Relationship::S1_WITHIN_S2: os << "S1_WITHIN_S2"; break;
        case Relationship::UNKNOWN: os << "UNKNOWN"; break;
    }
    return os;
}
// clang-format on

std::string relationship_to_string(const Relationship rel) {
    std::ostringstream oss;
    oss << rel;
    return oss.str();
}

}  // namespace

bool operator==(const TrimInfo& lhs, const TrimInfo& rhs) {
    return std::tie(lhs.start, lhs.end, lhs.heuristic) ==
           std::tie(rhs.start, rhs.end, rhs.heuristic);
}

std::ostream& operator<<(std::ostream& os, const TrimInfo& rhs) {
    os << "start = " << rhs.start << ", end = " << rhs.end << ", heuristic = " << rhs.heuristic;
    return os;
}

namespace {

Relationship relative_position(const Sample& s1, const Sample& s2) {
    // Helper lambdas for comparisons
    const auto ordered_abuts = [](const Sample& s1_, const Sample& s2_) -> bool {
        const auto [s1_end_maj, s1_end_min] = s1_.get_last_position();
        const auto [s2_start_maj, s2_start_min] = s2_.get_position(0);
        if (((s2_start_maj == (s1_end_maj + 1)) && (s2_start_min == 0)) ||
            ((s2_start_maj == s1_end_maj) && (s2_start_min == (s1_end_min + 1)))) {
            return true;
        }
        return false;
    };

    const auto ordered_contained = [](const Sample& s1_, const Sample& s2_) -> bool {
        return (s2_.get_position(0) >= s1_.get_position(0)) &&
               (s2_.get_last_position() <= s1_.get_last_position());
    };

    const auto ordered_overlaps = [](const Sample& s1_, const Sample& s2_) -> bool {
        const auto [s1_end_maj, s1_end_min] = s1_.get_last_position();
        const auto [s2_start_maj, s2_start_min] = s2_.get_position(0);
        if ((s2_start_maj < s1_end_maj) ||
            ((s2_start_maj == s1_end_maj) && (s2_start_min < (s1_end_min + 1)))) {
            return true;
        }
        return false;
    };

    const auto ordered_gapped = [](const Sample& s1_, const Sample& s2_) -> bool {
        const auto [s1_end_maj, s1_end_min] = s1_.get_last_position();
        const auto [s2_start_maj, s2_start_min] = s2_.get_position(0);
        if ((s2_start_maj > (s1_end_maj + 1)) ||
            ((s2_start_maj > s1_end_maj) && (s2_start_min > 0)) ||
            ((s2_start_maj == s1_end_maj) && (s2_start_min > (s1_end_min + 1)))) {
            return true;
        }
        return false;
    };

    // Main logic for relative position determination
    if (s1.seq_id != s2.seq_id) {
        return Relationship::DIFFERENT_REF_NAME;
    }

    // Sort s1 and s2 by first position, and then by size in descending order
    const Sample& s1_ord = (std::pair(s1.get_position(0), -dorado::ssize(s1.positions_major)) <=
                            std::pair(s2.get_position(0), -dorado::ssize(s2.positions_major)))
                                   ? s1
                                   : s2;
    const Sample& s2_ord = (&s1_ord == &s1) ? s2 : s1;
    const bool is_ordered = (&s1_ord == &s1);

    // Determine the relationship based on various conditions
    if (ordered_contained(s1_ord, s2_ord)) {
        return is_ordered ? Relationship::S2_WITHIN_S1 : Relationship::S1_WITHIN_S2;
    } else if (ordered_abuts(s1_ord, s2_ord)) {
        return is_ordered ? Relationship::FORWARD_ABUTTED : Relationship::REVERSE_ABUTTED;
    } else if (ordered_overlaps(s1_ord, s2_ord)) {
        return is_ordered ? Relationship::FORWARD_OVERLAP : Relationship::REVERSE_OVERLAP;
    } else if (ordered_gapped(s1_ord, s2_ord)) {
        return is_ordered ? Relationship::FORWARD_GAPPED : Relationship::REVERSE_GAPPED;
    }
    return Relationship::UNKNOWN;
}

std::tuple<int64_t, int64_t, bool> overlap_indices(const Sample& s1, const Sample& s2) {
    const Relationship rel = relative_position(s1, s2);

    if (rel == Relationship::FORWARD_ABUTTED) {
        return {dorado::ssize(s1.positions_major), 0, false};
    }

    if (rel != Relationship::FORWARD_OVERLAP) {
        throw std::runtime_error(
                "Cannot overlap samples! Relationship is not FORWARD_OVERLAP. rel = " +
                relationship_to_string(rel));
    }

    // Linear search over the pairs.
    const auto find_left = [](const Sample& s, const std::pair<int64_t, int64_t> target) {
        int64_t idx = -1;
        for (idx = 0; idx < dorado::ssize(s.positions_major); ++idx) {
            const std::pair<int64_t, int64_t> pos = s.get_position(idx);
            if (target < pos) {
                --idx;
                break;
            }
        }
        return idx;
    };

    const auto find_right = [](const Sample& s, const std::pair<int64_t, int64_t> target) {
        int64_t idx = 0;
        for (idx = 0; idx < dorado::ssize(s.positions_major); ++idx) {
            const std::pair<int64_t, int64_t> pos = s.get_position(idx);
            if (target <= pos) {
                ++idx;
                break;
            }
        }
        return idx;
    };

    const int64_t ovl_start_ind1 = find_left(s1, s2.get_position(0));
    const int64_t ovl_end_ind2 = find_right(s2, s1.get_last_position());

    if ((ovl_start_ind1 < 0) || (ovl_end_ind2 < 0)) {
        throw std::runtime_error(
                "Samples should be overlapping, but cannot find adequate cooordinate positions!");
    }

    int64_t end_1_ind = dorado::ssize(s1.positions_major);
    int64_t start_2_ind = 0;

    const auto compare_subvectors = [](const std::vector<int64_t>& a, const int64_t a_start,
                                       const int64_t a_end, const std::vector<int64_t>& b,
                                       const int64_t b_start, const int64_t b_end) {
        if ((a_end - a_start) != (b_end - b_start)) {
            return false;
        }
        for (int64_t i = a_start, j = b_start; i < a_end; ++i, ++j) {
            if (a[i] != b[j]) {
                return false;
            }
        }
        return true;
    };

    bool heuristic = false;

    // In this case, overlaps are not equal in structure.
    if (!compare_subvectors(s1.positions_minor, ovl_start_ind1, std::size(s1.positions_minor),
                            s2.positions_minor, 0, ovl_end_ind2)) {
        heuristic = true;
    }

    if (!heuristic) {
        // Take midpoint as the break point.
        const int64_t overlap_len = ovl_end_ind2;  // Both are equal in size.
        const int64_t pad_1 = overlap_len / 2;
        const int64_t pad_2 = overlap_len - pad_1;
        end_1_ind = ovl_start_ind1 + pad_1;
        start_2_ind = ovl_end_ind2 - pad_2;

        if (((end_1_ind - ovl_start_ind1) + (ovl_end_ind2 - start_2_ind)) != overlap_len) {
            end_1_ind = -1;
            start_2_ind = -1;
            heuristic = true;
        }
    }

    if (heuristic) {
        // Some sample producing methods will not create 1-to-1 mappings
        // in their sets of columns, e.g. where chunking has affected the
        // reads used. Here we find a split point near the middle where
        // the two halfs have the same number of minor positions
        // (i.e. look similar).
        // Require seeing a number of major positions.
        // Heuristic: find midpoint with similar major position counts.

        constexpr int64_t UNIQ_MAJ = 3;

        end_1_ind = dorado::ssize(s1.positions_major);
        start_2_ind = 0;

        const auto count_unique = [](const std::vector<int64_t>& a, const int64_t start,
                                     const int64_t end) -> int64_t {
            const int64_t len = dorado::ssize(a);
            if (std::empty(a) || (end <= start) || (start >= len) || (end > len)) {
                return 0;
            }
            int64_t prev = a[start];
            int64_t ret = 1;
            for (int64_t i = (start + 1); i < end; ++i) {
                if (a[i] == prev) {
                    continue;
                }
                prev = a[i];
                ++ret;
            }
            return ret;
        };

        const auto streak_count = [](const std::vector<int64_t>& a,
                                     const int64_t start) -> int64_t {
            const int64_t len = dorado::ssize(a);
            if (std::empty(a) || (start >= len)) {
                return 0;
            }
            int64_t ret = 1;
            for (int64_t i = (start + 1); i < len; ++i, ++ret) {
                if (a[i] != a[start]) {
                    break;
                }
            }
            return ret;
        };

        const int64_t unique_s1 =
                count_unique(s1.positions_major, ovl_start_ind1, std::size(s1.positions_minor));
        const int64_t unique_s2 = count_unique(s2.positions_major, 0, ovl_end_ind2);

        if ((unique_s1 > UNIQ_MAJ) && (unique_s2 > UNIQ_MAJ)) {
            const int64_t start = s1.positions_major[ovl_start_ind1];
            const int64_t end = s1.positions_major.back();
            const int64_t mid = start + (end - start) / 2;
            int64_t offset = 1;

            while (end_1_ind == -1) {
                if (((mid + offset) > end) && ((mid - offset) < start)) {
                    break;
                }

                for (const int64_t test : {+offset, -offset}) {
                    const auto left = std::lower_bound(std::begin(s1.positions_major),
                                                       std::end(s1.positions_major), mid + test);
                    const auto right = std::lower_bound(std::begin(s2.positions_major),
                                                        std::end(s2.positions_major), mid + test);

                    const int64_t left_pos = std::distance(std::begin(s1.positions_major), left);
                    const int64_t right_pos = std::distance(std::begin(s2.positions_major), right);

                    const int64_t left_streak = streak_count(s1.positions_major, left_pos);
                    const int64_t right_streak = streak_count(s2.positions_major, right_pos);

                    if ((left != std::end(s1.positions_major)) &&
                        (right != std::end(s2.positions_major)) && (left_streak == right_streak)) {
                        end_1_ind = left_pos;
                        start_2_ind = right_pos;
                        break;
                    }
                }

                offset += 1;
            }
        }
    }

    // If return coordinates are -1, then a viable junction was not found.
    return {end_1_ind, start_2_ind, heuristic};
}

}  // namespace

std::vector<TrimInfo> trim_samples(const std::vector<Sample>& samples,
                                   const std::optional<const secondary::RegionInt>& region) {
    std::vector<const Sample*> ptrs;
    ptrs.reserve(std::size(samples));
    for (const auto& sample : samples) {
        ptrs.emplace_back(&sample);
    }
    return trim_samples(ptrs, region);
}

std::vector<TrimInfo> trim_samples(const std::vector<const Sample*>& samples,
                                   const std::optional<const secondary::RegionInt>& region) {
    std::vector<TrimInfo> result(std::size(samples));

    if (std::empty(samples)) {
        return result;
    }

    for (size_t i = 0; i < std::size(samples); ++i) {
        if (!samples[i]) {
            throw std::runtime_error{"Nullptr sample provided to trim_samples. Returning empty."};
        }
    }

    size_t idx_s1 = 0;
    int64_t num_heuristic = 0;

    result[0].start = 0;
    result[0].end = dorado::ssize(samples.front()->positions_major);

    for (size_t i = 1; i < std::size(samples); ++i) {
        const Sample& s1 = *samples[idx_s1];
        const Sample& s2 = *samples[i];
        bool heuristic = false;

        const Relationship rel = relative_position(s1, s2);

        TrimInfo& trim1 = result[idx_s1];
        TrimInfo& trim2 = result[i];

        trim2.start = 0;
        trim2.end = dorado::ssize(s2.positions_major);

        if (rel == Relationship::S2_WITHIN_S1) {
            continue;

        } else if (rel == Relationship::FORWARD_GAPPED) {
            // Deprecated: trim2.is_last_in_contig = true;

        } else if (rel == Relationship::DIFFERENT_REF_NAME) {
            // Pass. No trimming should be applied.

        } else {
            try {
                std::tie(trim1.end, trim2.start, heuristic) = overlap_indices(s1, s2);
            } catch (const std::runtime_error& e) {
                throw std::runtime_error(
                        "Unhandled overlap type whilst stitching chunks. Message: " +
                        std::string(e.what()));
            }
        }

        idx_s1 = i;

        num_heuristic += heuristic;
    }

    {
        result.back().end = dorado::ssize(samples.back()->positions_major);
        // Deprecated: result.back().is_last_in_contig = true;
    }

    // Trim each sample to the region.
    if (region) {
        if ((region->seq_id < 0) || (region->start < 0) || (region->end <= 0)) {
            throw std::runtime_error{"Region trimming coordinates are not valid. seq_id = " +
                                     std::to_string(region->seq_id) +
                                     ", start = " + std::to_string(region->start) +
                                     ", end = " + std::to_string(region->end)};
        }

        spdlog::trace("[trim_samples] Trimming to region.");

        for (size_t i = 0; i < std::size(samples); ++i) {
            const Sample& sample = *samples[i];
            TrimInfo& trim = result[i];

            // Sample not on the specified sequence.
            if (sample.seq_id != region->seq_id) {
                trim.start = -1;
                trim.end = -1;
                continue;
            }

            const int64_t num_positions = dorado::ssize(sample.positions_major);

            // Trim left.
            for (; (region->start > 0) && (trim.start < num_positions); ++trim.start) {
                if (sample.positions_major[trim.start] >= region->start) {
                    break;
                }
            }
            trim.start = (trim.start >= num_positions) ? -1 : trim.start;

            // Trim right. End is non-inclusive, so reduce it by 1 first.
            for (--trim.end; (region->end > 0) && (trim.end >= 0); --trim.end) {
                if (sample.positions_major[trim.end] < region->end) {
                    break;
                }
            }
            ++trim.end;  // Change back to non-inclusive.
            trim.end = (trim.end <= 0) ? -1 : trim.end;

            // Sanity check.
            if (trim.start >= trim.end) {
                trim.start = -1;
                trim.end = -1;
            }
        }
    } else {
        spdlog::trace("[trim_samples] Not trimming to region.");
    }

    assert(std::size(result) == std::size(samples));

    spdlog::trace("[trim_samples] num_heuristic = {}", num_heuristic);

    return result;
}

}  // namespace dorado::polisher
