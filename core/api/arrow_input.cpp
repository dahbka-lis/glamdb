#include "core/api/canonical.h"

#include "core/api/binary.h"

#include <arrow/api.h>
#include <unordered_map>

namespace glamdb::detail {
namespace {

std::vector<int> MapFields(const SchemaSpec &schema,
                           const arrow::RecordBatch &batch) {
    Require(batch.num_columns() <= static_cast<int>(schema.fields.size()) &&
                batch.ValidateFull().ok(),
            "INVALID_ARGUMENT", "invalid Arrow batch");
    std::unordered_map<std::string, int> names;
    for (int i = 0; i < batch.num_columns(); ++i) {
        const auto &name = batch.schema()->field(i)->name();
        Require(names.emplace(name, i).second &&
                    std::ranges::any_of(
                        schema.fields,
                        [&](const auto &field) { return field.name == name; }),
                "INVALID_ARGUMENT", "unknown or duplicate Arrow field");
    }
    std::vector<int> columns;
    for (const auto &field : schema.fields) {
        const auto found = names.find(field.name);
        if (found == names.end()) {
            Require(field.nullable, "INVALID_ARGUMENT",
                    "required Arrow field is missing");
            columns.push_back(-1);
            continue;
        }
        const auto &type = batch.column(found->second)->type();
        bool matches = false;
        switch (field.type) {
        case FieldType::String:
            matches = type->id() == arrow::Type::STRING;
            break;
        case FieldType::Bool:
            matches = type->id() == arrow::Type::BOOL;
            break;
        case FieldType::Int64:
            matches = type->id() == arrow::Type::INT64;
            break;
        case FieldType::Float64:
            matches = type->id() == arrow::Type::DOUBLE;
            break;
        case FieldType::TimestampUsUtc:
            if (type->id() == arrow::Type::TIMESTAMP) {
                const auto &timestamp =
                    static_cast<const arrow::TimestampType &>(*type);
                matches = timestamp.unit() == arrow::TimeUnit::MICRO &&
                          timestamp.timezone() == "UTC";
            }
            break;
        }
        Require(matches, "INVALID_ARGUMENT",
                "Arrow field type does not match fixed schema");
        columns.push_back(found->second);
    }
    return columns;
}

struct Shape {
    size_t rows = 0;
    size_t decoded_bytes = 0;
};

Shape Inspect(const SchemaSpec &schema,
              const std::vector<std::shared_ptr<arrow::RecordBatch>> &batches) {
    const auto fingerprint = SchemaFingerprint(schema);
    if (!fingerprint) {
        throw Failure(fingerprint.error());
    }
    Require(!batches.empty() && batches.size() <= 10000, "INVALID_ARGUMENT",
            "append requires bounded Arrow batches");
    Shape shape;
    for (const auto &batch : batches) {
        Require(batch != nullptr && batch->num_rows() >= 0 &&
                    batch->num_rows() <= 10000 &&
                    shape.rows + static_cast<size_t>(batch->num_rows()) <=
                        10000,
                "INVALID_ARGUMENT", "append requires at most 10000 rows");
        shape.rows += static_cast<size_t>(batch->num_rows());
        const auto columns = MapFields(schema, *batch);
        for (int64_t row = 0; row < batch->num_rows(); ++row) {
            size_t bytes = schema.fields.size();
            for (size_t field = 0; field < columns.size(); ++field) {
                const int column = columns[field];
                if (column < 0 || batch->column(column)->IsNull(row)) {
                    Require(schema.fields[field].nullable, "INVALID_ARGUMENT",
                            "NULL in required Arrow field");
                    continue;
                }
                if (schema.fields[field].type == FieldType::String) {
                    const auto &strings =
                        static_cast<const arrow::StringArray &>(
                            *batch->column(column));
                    const auto length = strings.value_length(row);
                    Require(length <= (schema.fields[field].name == "key"
                                           ? 1024
                                           : 1024 * 1024),
                            "INVALID_ARGUMENT", "oversized Arrow string");
                    bytes += 4 + static_cast<size_t>(length);
                } else {
                    bytes +=
                        schema.fields[field].type == FieldType::Bool ? 1 : 8;
                }
            }
            shape.decoded_bytes += bytes;
            Require(bytes <= 2ULL * 1024 * 1024 &&
                        shape.decoded_bytes <= 16ULL * 1024 * 1024,
                    "RESOURCE_EXHAUSTED",
                    "Arrow input exceeds decoded byte limit");
        }
    }
    Require(shape.rows != 0, "INVALID_ARGUMENT",
            "empty append is not supported");
    return shape;
}

Cell Read(const arrow::Array &array, int64_t row, FieldType type) {
    if (array.IsNull(row)) {
        return std::monostate{};
    }
    switch (type) {
    case FieldType::String:
        return std::string(
            static_cast<const arrow::StringArray &>(array).GetView(row));
    case FieldType::Bool:
        return static_cast<const arrow::BooleanArray &>(array).Value(row);
    case FieldType::Int64:
        return static_cast<const arrow::Int64Array &>(array).Value(row);
    case FieldType::Float64:
        return static_cast<const arrow::DoubleArray &>(array).Value(row);
    case FieldType::TimestampUsUtc:
        return TimestampUs{
            static_cast<const arrow::TimestampArray &>(array).Value(row)};
    }
    throw Failure(
        {"INVALID_ARGUMENT", "unsupported Arrow field", RetryHint::FixInput});
}

} // namespace

Result<size_t> InputReservation(
    const SchemaSpec &schema,
    const std::vector<std::shared_ptr<arrow::RecordBatch>> &batches) {
    try {
        const auto shape = Inspect(schema, batches);
        // Includes both normalization copies, canonical string growth and STL
        // cells, including tiny/null values whose buffers alone undercount RAM.
        return shape.decoded_bytes * 5 +
               shape.rows *
                   (sizeof(Row) + schema.fields.size() * sizeof(Cell)) * 2 +
               65536;
    } catch (const Failure &error) {
        return std::unexpected(error.detail);
    }
}

Result<NormalizedInput> NormalizeBatches(
    const SchemaSpec &schema, std::string_view collection_id,
    const std::vector<std::shared_ptr<arrow::RecordBatch>> &batches) {
    try {
        const auto shape = Inspect(schema, batches);
        std::vector<Row> rows;
        rows.reserve(shape.rows);
        for (const auto &batch : batches) {
            const auto columns = MapFields(schema, *batch);
            for (int64_t index = 0; index < batch->num_rows(); ++index) {
                Row row;
                row.reserve(schema.fields.size());
                for (size_t field = 0; field < columns.size(); ++field) {
                    row.push_back(columns[field] < 0
                                      ? Cell(std::monostate{})
                                      : Read(*batch->column(columns[field]),
                                             index, schema.fields[field].type));
                }
                rows.push_back(std::move(row));
            }
        }
        return NormalizeRows(schema, collection_id, rows);
    } catch (const Failure &error) {
        return std::unexpected(error.detail);
    }
}

} // namespace glamdb::detail
