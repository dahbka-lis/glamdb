#include "core/api/canonical.h"
#include "core/api/codec.h"

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

namespace glamdb {
namespace {

TEST(Canonical, IndependentGoldenWithNullAndDuplicateInt64) {
    const SchemaSpec schema{{{"n", FieldType::Int64, true}}};
    const std::vector<Row> rows{{int64_t{9007199254740993LL}},
                                {std::monostate{}},
                                {int64_t{9007199254740993LL}}};
    const auto hash = detail::SchemaFingerprint(schema);
    ASSERT_TRUE(hash);
    EXPECT_EQ(
        *hash,
        "4078bb95b0753db6f11470b1507afe1936afc16845b96029fbf1105110301685");
    const auto input = detail::NormalizeRows(
        schema, "00112233-4455-6677-8899-aabbccddeeff", rows);
    ASSERT_TRUE(input);
    EXPECT_EQ(
        input->payload_hash,
        "e9cbe7131f25c3f2932c48f755529fd10370a936c5182e2bca621d5f9e3c0fd7");
    EXPECT_EQ(input->rows.size(), 3U);
    EXPECT_EQ(input->decoded_bytes, 19U);
}

TEST(Canonical, AllTypesGoldenAndNegativeZero) {
    const SchemaSpec schema{{{"s", FieldType::String, false},
                             {"b", FieldType::Bool, true},
                             {"i", FieldType::Int64, true},
                             {"f", FieldType::Float64, true},
                             {"t", FieldType::TimestampUsUtc, true}}};
    const std::vector<Row> rows{
        {std::string("é"), true, std::numeric_limits<int64_t>::min(), -0.0,
         TimestampUs{-62135596800000000LL}},
        {std::string("dup"), false, std::numeric_limits<int64_t>::max(), 1.5,
         TimestampUs{253402300799999999LL}},
        {std::string("dup"), std::monostate{}, std::monostate{},
         std::monostate{}, std::monostate{}}};
    const auto input = detail::NormalizeRows(
        schema, "00112233-4455-6677-8899-aabbccddeeff", rows);
    ASSERT_TRUE(input);
    EXPECT_EQ(
        input->payload_hash,
        "72b0d7fbd4d8097a8a848de273fbf13cffa462d20be481e87d8c788f64feef1b");
    EXPECT_FALSE(std::signbit(std::get<double>(input->rows.front()[3])));
}

TEST(Canonical, RejectsTypesNullsUtf8AndTimestampOverflow) {
    const auto normalize = [](FieldType type, Cell cell) {
        return detail::NormalizeRows(SchemaSpec{{{"x", type, false}}},
                                     "00112233-4455-6677-8899-aabbccddeeff",
                                     {{cell}});
    };
    EXPECT_FALSE(normalize(FieldType::Int64, true));
    EXPECT_FALSE(normalize(FieldType::Int64, std::monostate{}));
    EXPECT_FALSE(normalize(FieldType::String, std::string("\xc0\xaf")));
    EXPECT_FALSE(
        normalize(FieldType::Float64, std::numeric_limits<double>::infinity()));
    EXPECT_FALSE(normalize(FieldType::Float64,
                           std::numeric_limits<double>::quiet_NaN()));
    EXPECT_FALSE(normalize(FieldType::TimestampUsUtc,
                           TimestampUs{-62135596800000001LL}));
    EXPECT_FALSE(normalize(FieldType::TimestampUsUtc,
                           TimestampUs{253402300800000000LL}));
    EXPECT_TRUE(normalize(FieldType::TimestampUsUtc, TimestampUs{-1}));
    EXPECT_FALSE(
        detail::NormalizeRows(SchemaSpec{{{"x", FieldType::Int64, true}}},
                              "not-a-uuid", {{int64_t{1}}}));
    EXPECT_FALSE(
        detail::NormalizeRows(SchemaSpec{{{"x", FieldType::Int64, true}}},
                              "00112233-4455-6677-8899-aabbccddeeff", {}));
}

TEST(Canonical, SchemaAndInputLimitsFailBeforeCopy) {
    EXPECT_FALSE(detail::SchemaFingerprint(SchemaSpec{}));
    EXPECT_FALSE(detail::SchemaFingerprint(
        SchemaSpec{{{"key", FieldType::String, true}}}));
    EXPECT_FALSE(detail::SchemaFingerprint(SchemaSpec{
        {{"x", FieldType::Bool, true}, {"x", FieldType::Bool, false}}}));
    const SchemaSpec schema{{{"key", FieldType::String, false}}};
    EXPECT_FALSE(detail::NormalizeRows(schema,
                                       "00112233-4455-6677-8899-aabbccddeeff",
                                       {{std::string(1025, 'a')}}));
    EXPECT_FALSE(
        detail::NormalizeRows(schema, "00112233-4455-6677-8899-aabbccddeeff",
                              std::vector<Row>(10001, Row{std::string("k")})));
}

TEST(OutcomeCodec, RetainsOriginalFailureAndRejectsUnknownFormats) {
    const AppendOutcome failed{
        "00000000-0000-0000-0000-000000000003",
        CommitDisposition::DefinitelyNotCommitted,
        CommitPhase::Data,
        std::nullopt,
        ErrorDetail{"IO_ERROR", "first error", RetryHint::NewOperationId},
        std::nullopt};
    const auto bytes = detail::EncodeOutcome(failed);
    ASSERT_TRUE(bytes);
    const auto decoded = detail::DecodeOutcome(*bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, failed);
    EXPECT_FALSE(detail::DecodeOutcome(bytes->substr(0, bytes->size() - 1)));
    auto corrupt = *bytes;
    corrupt[0] = 99;
    EXPECT_FALSE(detail::DecodeOutcome(corrupt));
    EXPECT_FALSE(detail::EncodeOutcome(AppendOutcome{}));
}

} // namespace
} // namespace glamdb
