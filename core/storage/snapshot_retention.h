#pragma once

#include "core/storage/capacity.h"

#include <iceberg/result.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace iceberg {
struct TableMetadata;
}

namespace NGlamDB::NStorage {

struct TRetentionPolicy
{
    std::optional<std::int64_t> KeepLast{};
    std::int64_t MinAgeMs = 0;
    bool operator==(const TRetentionPolicy&) const = default;
};

inline constexpr char RetentionVersionKey[] = "glamdb.retention.version";
inline constexpr char RetentionKeepKey[] = "glamdb.retention.keep-last";
inline constexpr char RetentionAgeKey[] = "glamdb.retention.min-age-ms";
inline constexpr char ExpiredSequenceKey[] = "glamdb.retention.expired-through-sequence";

iceberg::Status CheckRetentionPolicy(const TRetentionPolicy& policy);
iceberg::Result<TRetentionPolicy> ReadRetentionPolicy(const iceberg::TableMetadata& metadata);
iceberg::Result<std::int64_t> ExpiredSequence(const iceberg::TableMetadata& metadata);
iceberg::Status
WriteRetentionPolicy(iceberg::TableMetadata& metadata, const TRetentionPolicy& policy, std::int64_t expiredSequence);
iceberg::Result<std::size_t>
ExpirationPrefix(const iceberg::TableMetadata& metadata, const TRetentionPolicy& policy, std::int64_t nowMs);
// Validates and removes the supported extension properties from a profile copy.
iceberg::Status StripRetentionProperties(std::unordered_map<std::string, std::string>& properties);
TStorageLimits RetentionLimits(TStorageLimits limits, const iceberg::TableMetadata& metadata);

}
