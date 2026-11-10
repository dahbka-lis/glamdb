#include "core/api/canonical.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

namespace glamdb::detail {
namespace {

std::shared_ptr<arrow::RecordBatch>
Integers(const std::vector<std::optional<int64_t>> &values) {
    arrow::Int64Builder builder;
    for (const auto value : values) {
        const auto status =
            value ? builder.Append(*value) : builder.AppendNull();
        EXPECT_TRUE(status.ok());
    }
    const auto array = builder.Finish();
    EXPECT_TRUE(array.ok());
    return arrow::RecordBatch::Make(
        arrow::schema({arrow::field("n", arrow::int64())}),
        static_cast<int64_t>(values.size()), {*array});
}

TEST(ArrowInput, ChunkBoundariesDoNotChangeCanonicalIdentity) {
    const SchemaSpec schema{{{"n", FieldType::Int64, true}}};
    const auto single = NormalizeBatches(
        schema, "00112233-4455-6677-8899-aabbccddeeff",
        {Integers({9007199254740993LL, std::nullopt, 9007199254740993LL})});
    const auto chunked = NormalizeBatches(
        schema, "00112233-4455-6677-8899-aabbccddeeff",
        {Integers({9007199254740993LL}), Integers({std::nullopt}),
         Integers({9007199254740993LL})});
    ASSERT_TRUE(single);
    ASSERT_TRUE(chunked);
    EXPECT_EQ(
        single->payload_hash,
        "e9cbe7131f25c3f2932c48f755529fd10370a936c5182e2bca621d5f9e3c0fd7");
    EXPECT_EQ(chunked->payload_hash, single->payload_hash);
    EXPECT_EQ(chunked->rows, single->rows);
}

TEST(ArrowInput, MissingNullableFieldsBecomeNullButUnknownAndNullBatchReject) {
    const SchemaSpec schema{
        {{"n", FieldType::Int64, true}, {"extra", FieldType::Bool, true}}};
    const auto result = NormalizeBatches(
        schema, "00112233-4455-6677-8899-aabbccddeeff", {Integers({1})});
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(result->rows[0][1]));
    EXPECT_FALSE(NormalizeBatches(
        SchemaSpec{{{"other", FieldType::Int64, true}}},
        "00112233-4455-6677-8899-aabbccddeeff", {Integers({1})}));
    EXPECT_FALSE(NormalizeBatches(
        schema, "00112233-4455-6677-8899-aabbccddeeff", {nullptr}));
}

} // namespace
} // namespace glamdb::detail
