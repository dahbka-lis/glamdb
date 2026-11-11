#include "core/resources/metadata_result.h"

#include <gtest/gtest.h>

namespace glamdb::detail {
namespace {
TEST(MetadataResources, ManySmallBoundsRequireAllocationHeadroom)
{
    ResourceLimits limits;
    limits.memory_bytes[1] = 1ULL << 20;
    auto owner = std::make_shared<ResourceOwner>(limits);
    TFileListing listing;
    listing.Files.resize(64);
    for (auto& file : listing.Files) {
        file.FileID = std::string(64, 'a');
        file.FieldMetrics.resize(64);
        for (auto& field : file.FieldMetrics) {
            field.LowerBound = std::vector<uint8_t>(8, 0);
            field.UpperBound = std::vector<uint8_t>(8, 1);
        }
    }
    // Payload capacities fit in 1 MiB; thousands of allocation headers and
    // rounded small bounds need additional capacity before returning this DTO.
    const auto retained = RetainMetadata(owner, listing);
    EXPECT_FALSE(retained);
    if (!retained) {
        EXPECT_EQ(retained.error().code, "RESOURCE_EXHAUSTED");
    }
    EXPECT_EQ(owner->GetUsage().memory_bytes[1], 0);
}
} // namespace
} // namespace glamdb::detail
