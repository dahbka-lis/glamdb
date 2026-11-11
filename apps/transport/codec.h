#pragma once

#include "storage/engine.h"

#include <arrow/api.h>
#include <nlohmann/json.hpp>

namespace glamdb::NTransport {
using TJson = nlohmann::json;
inline constexpr size_t MaxRequestBytes = 16ULL * 1024 * 1024;
inline constexpr size_t MaxResponseBytes = 16ULL * 1024 * 1024;

struct TFailure : std::exception
{
    ErrorDetail Detail;
    explicit TFailure(ErrorDetail detail)
        : Detail(std::move(detail))
    { }
};
void Require(bool condition, std::string_view message, std::string_view code = "INVALID_ARGUMENT");
template <class T>
T Unwrap(Result<T> result)
{
    if (!result) {
        throw TFailure(result.error());
    }
    return std::move(*result);
}
TJson ParseJson(std::string_view bytes);
void CheckKeys(const TJson& value, std::initializer_list<std::string_view> keys);
int64_t ParseInt64(const TJson& value);
SchemaSpec ParseSchema(const TJson& value);
std::shared_ptr<arrow::RecordBatch> ParseRows(const SchemaSpec& schema, const TJson& rows);
ScanRequest ParseScan(std::string collectionID, const SchemaSpec& schema, const TJson& body);
TJson EncodeSchema(const arrow::Schema& schema);
TJson EncodeRows(const arrow::RecordBatch& batch);
TJson EncodeIdentity(const BaseIdentity& identity);
TJson EncodeError(const ErrorDetail& error);
int ErrorStatus(std::string_view code);
} // namespace glamdb::NTransport
