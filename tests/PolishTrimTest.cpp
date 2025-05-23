#include "polish/trim.h"
#include "secondary/region.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace {

#define TEST_GROUP "[PolishTrimSamples]"

CATCH_TEST_CASE("trim_samples tests", TEST_GROUP) {
    using namespace dorado::polisher;
    using namespace dorado::secondary;

    // The actual Sample struct is complicated, but the only thing needed
    // for trimming are seq_id, positions_major and positions_minor.
    // This would not be needed if C++20 was available because we could simply use
    // designated initializers.
    struct MockSample {
        int32_t seq_id = -1;
        std::vector<int64_t> positions_major;
        std::vector<int64_t> positions_minor;
    };

    struct TestCase {
        std::string name;                       // Name of the test.
        std::vector<MockSample> samples;        // Input samples to trim.
        std::optional<const RegionInt> region;  // Region from which the samples were generated.
        bool expect_throw = false;
        std::vector<TrimInfo> expected;  // Expected results.
    };

    // clang-format off
    auto [test_case] = GENERATE(table<TestCase>({
        TestCase{
            "Empty input == empty output",
            {},             // samples
            std::nullopt,   // region
            false,          // expect_throw
            {},             // expected
        },
        TestCase{
            "Single sample",
            {   // Input samples.
                MockSample{
                    0,                              // seq_id
                    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, // positions_major
                    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // positions_minor
                },
            },
            RegionInt{0, 0, 10},   // region
            false,              // expect_throw
            {   // Expected results.
                TrimInfo{0, 10, false},
            },
        },
        TestCase{
            "Medaka test. Three samples, overlapping. Should trim.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
            },
            std::nullopt,   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{0, 9, false},
                TrimInfo{1, 6, false},
                TrimInfo{1, 4, false},
            },
        },
        TestCase{
            "Medaka test. Two samples with a gap in between.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
            },
            std::nullopt,   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{0, 11, false},
                TrimInfo{0, 4, false},
            },
        },
        TestCase{
            "Medaka test. Input samples are out of order and overlapping. Should throw.",
            {   // Input samples.
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
                MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            std::nullopt,   // region
            true,           // expect_throw
            {   // Expected results.
            },
        },
        TestCase{
            "Input samples are out of order but NOT overlapping. Should throw because only forward oriented relationship is allowed.",
            {   // Input samples.
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            std::nullopt,   // region
            true,           // expect_throw
            {   // Expected results.
            },
        },
        TestCase{
            "Three samples on different reference. No trimming should be applied.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    1,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    2,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
            },
            std::nullopt,   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{0, 11, false},
                TrimInfo{0, 8, false},
                TrimInfo{0, 4, false},
            },
        },
        TestCase{
            " Three samples per reference. Should trim.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
                MockSample{
                    1,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    1,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    1,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
            },
            std::nullopt,   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{0, 9, false},
                TrimInfo{1, 6, false},
                TrimInfo{1, 4, false},
                TrimInfo{0, 9, false},
                TrimInfo{1, 6, false},
                TrimInfo{1, 4, false},
            },
        },
        TestCase{
            "Use a region, but the region is larger than what samples span. Region trimming won't influence the results.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
            },
            RegionInt{0, 0, 1000},   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{0, 9, false},
                TrimInfo{1, 6, false},
                TrimInfo{1, 4, false},
            },
        },
        TestCase{
            "Trim to a region which is shorter than the span of windows. Three samples overlapping, and one on a different reference. Trimming should be applied between samples, and on region start/end.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
                MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
                },
                MockSample{
                    0,              // seq_id
                    {6, 6, 7, 7},   // positions_major
                    {0, 1, 0, 1},   // positions_minor
                },
                MockSample{
                    1,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            RegionInt{0, 1, 5},   // region
            false,          // expect_throw
            {   // Expected results.
                TrimInfo{2, 9, false},
                TrimInfo{1, 4, false},
                TrimInfo{-1, -1, false},    // Filtered out.
                TrimInfo{-1, -1, false},    // Filtered out.
            },
        },
        TestCase{
            "Use a region, but the start coordinate is not valid. Should throw.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            RegionInt{0, -1, 1000},   // region
            true,          // expect_throw
            {   // Expected results.
            },
        },
        TestCase{
            "Use a region, but the end coordinate is not valid. Should throw.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            RegionInt{0, 1000, -1},   // region
            true,          // expect_throw
            {   // Expected results.
            },
        },
        TestCase{
            "Use a region, but the seq_id is not valid. Should throw.",
            {   // Input samples.
                MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
                },
            },
            RegionInt{-1, 1000, 2000},   // region
            true,          // expect_throw
            {   // Expected results.
            },
        },
    }));
    // clang-format on

    CATCH_INFO(TEST_GROUP << " Test name: " << test_case.name);
    std::vector<Sample> samples;
    for (const auto& mock_sample : test_case.samples) {
        Sample new_sample;
        new_sample.seq_id = mock_sample.seq_id;
        new_sample.positions_major = mock_sample.positions_major;
        new_sample.positions_minor = mock_sample.positions_minor;
        samples.emplace_back(std::move(new_sample));
    }

    if (test_case.expect_throw) {
        CATCH_CHECK_THROWS(trim_samples(samples, test_case.region));
    } else {
        const std::vector<TrimInfo> result = trim_samples(samples, test_case.region);
        CATCH_CHECK(test_case.expected == result);
    }
}

/**
 * \brief This tests the alternative overload if trim_samples which takes a vector of pointers instead
 *          of a vector of actual contiguous data. This allows for the const data to be permuted or sorted
 *          and still trimmed without making copies.
 *
 *          This test creates one const vector of mocked samples, then uses numeric IDs to create
 *          the std::vector<const Sample*> used for input.
 *          There is also a test for a nullptr pointer.
 *
 *          Since this function is essentially a backbone of the other overload, more detailed tests from the
 *          previous fixture will also test all the other edge cases here.
 */
CATCH_TEST_CASE("trim_samples via pointers", TEST_GROUP) {
    using namespace dorado::polisher;

    // The actual Sample struct is complicated, but the only thing needed
    // for trimming are seq_id, positions_major and positions_minor.
    // This would not be needed if C++20 was available because we could simply use
    // designated initializers.
    struct MockSample {
        int32_t seq_id = -1;
        std::vector<int64_t> positions_major;
        std::vector<int64_t> positions_minor;
    };

    // Create all input samples, in a permuted order.
    const std::vector<MockSample> all_mock_samples{
            MockSample{
                    0,             // seq_id
                    {6, 6, 7, 7},  // positions_major
                    {0, 1, 0, 1},  // positions_minor
            },
            MockSample{
                    0,                                  // seq_id
                    {0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 4},  // positions_major
                    {0, 1, 0, 0, 1, 2, 0, 0, 1, 2, 3},  // positions_minor
            },
            MockSample{
                    0,                         // seq_id
                    {4, 4, 4, 4, 5, 6, 6, 7},  // positions_major
                    {1, 2, 3, 5, 0, 0, 1, 0},  // positions_minor
            },
    };

    // clang-format off
    struct TestCase {
        std::string name;                       // Name of the test.
        std::vector<int32_t> sample_ids;        // IDs of samples to create a permutation, -1 means nullptr.
        bool expect_throw = false;
        std::vector<TrimInfo> expected;         // Expected results.
    };
    // clang-format on

    // clang-format off
    auto [test_case] = GENERATE(table<TestCase>({
        TestCase{
            "Empty input == empty output",
            {},             // sample_ids
            false,          // expect_throw
            {},             // expected
        },
        TestCase{
            "Three samples, overlapping. Should trim.",
            {1, 2, 0},      // sample_ids
            false,          // expect_throw
            {               // expected
                TrimInfo{0, 9, false},
                TrimInfo{1, 6, false},
                TrimInfo{1, 4, false},
            },
        },
        TestCase{
            "One of the samples is a nullptr, this should throw.",
            {1, -1, 0},     // sample_ids
            true,           // expect_throw
            {               // expected
            },
        },
    }));
    // clang-format on

    // Create the actual Sample objects. This would not be needed if we had C++20 designated initializers.
    CATCH_INFO(TEST_GROUP << " Test name: " << test_case.name);
    std::vector<Sample> samples;
    for (const auto& mock_sample : all_mock_samples) {
        Sample new_sample;
        new_sample.seq_id = mock_sample.seq_id;
        new_sample.positions_major = mock_sample.positions_major;
        new_sample.positions_minor = mock_sample.positions_minor;
        samples.emplace_back(std::move(new_sample));
    }

    // Create the input data as a vector of pointers.
    std::vector<const Sample*> input;
    for (const int32_t id : test_case.sample_ids) {
        if (id < 0) {
            input.emplace_back(nullptr);
        } else if (id >= static_cast<int32_t>(std::size(samples))) {
            throw std::runtime_error("Bad test case! Sample ID out of bounds!");
        } else {
            input.emplace_back(&samples[id]);
        }
    }

    if (test_case.expect_throw) {
        CATCH_CHECK_THROWS(trim_samples(input, std::nullopt));
    } else {
        const std::vector<TrimInfo> result = trim_samples(input, std::nullopt);
        CATCH_CHECK(test_case.expected == result);
    }
}

}  // namespace