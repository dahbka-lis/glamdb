#pragma once

#include "storage/types.h"

#include <string_view>

namespace glamdb::detail {

struct NormalizedInput {
    std::vector<Row> rows;
    std::string payload_hash;
    size_t decoded_bytes = 0;
};

bool ValidUuid(std::string_view value);
bool ValidUtf8(std::string_view value);
bool ValidDigest(std::string_view value);
std::string Sha256(std::string_view input);
Result<std::string> EncodeSchema(const SchemaSpec &schema);
Result<SchemaSpec> DecodeSchema(std::string_view bytes);
Result<std::string> SchemaFingerprint(const SchemaSpec &schema);
Result<NormalizedInput> NormalizeRows(const SchemaSpec &schema,
                                      std::string_view collection_id,
                                      const std::vector<Row> &rows);
Result<NormalizedInput> NormalizeBatches(
    const SchemaSpec &schema, std::string_view collection_id,
    const std::vector<std::shared_ptr<arrow::RecordBatch>> &batches);

} // namespace glamdb::detail
