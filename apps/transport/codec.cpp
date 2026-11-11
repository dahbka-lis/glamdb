#include "apps/transport/codec.h"
#include "core/api/canonical.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <regex>
#include <set>

namespace glamdb::NTransport {
namespace {
const std::vector<std::string> TypeNames{"string", "bool", "int64", "float64", "timestamp_us_utc"};

// Validate the stream without constructing or filtering a DOM. The pinned
// callback DOM parser rescans the parent array after each object, making even
// an always-true callback quadratic for append batches. A separate SAX pass
// preserves the original event/depth limits and decoded-key checks.
class TJsonValidator final : public nlohmann::json_sax<TJson>
{
public:
    bool null() override
    {
        return Visit();
    }
    bool boolean(bool) override
    {
        return Visit();
    }
    bool number_integer(number_integer_t) override
    {
        return Visit();
    }
    bool number_unsigned(number_unsigned_t) override
    {
        return Visit();
    }
    bool number_float(number_float_t, const string_t&) override
    {
        return Visit();
    }
    bool string(string_t&) override
    {
        return Visit();
    }
    bool binary(binary_t&) override
    {
        return Visit();
    }
    bool start_object(size_t) override
    {
        Visit();
        ++Depth_;
        Objects_.emplace_back();
        return true;
    }
    bool key(string_t& value) override
    {
        Visit();
        Require(Objects_.back().insert(value).second, "duplicate JSON key");
        return true;
    }
    bool end_object() override
    {
        --Depth_;
        Objects_.pop_back();
        return Visit();
    }
    bool start_array(size_t) override
    {
        Visit();
        ++Depth_;
        return true;
    }
    bool end_array() override
    {
        --Depth_;
        return Visit();
    }
    bool parse_error(size_t, const std::string&, const TJson::exception&) override
    {
        return false;
    }

private:
    bool Visit()
    {
        Require(Depth_ <= 32 && ++Nodes_ <= 1000000, "JSON complexity limit exceeded");
        return true;
    }
    size_t Depth_ = 0;
    size_t Nodes_ = 0;
    std::vector<std::set<std::string>> Objects_;
};

int64_t ParseTimestamp(const TJson& value)
{
    Require(value.is_string(), "timestamp must be an RFC3339 string");
    const auto text = value.get<std::string>();
    static const std::regex Pattern(
        R"((\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?(Z|[+-]\d{2}:\d{2}))");
    std::smatch match;
    Require(text.size() <= 32 && std::regex_match(text, match, Pattern), "invalid UTC microsecond timestamp");
    const auto number = [&](size_t index) {
        return std::stoi(match[index].str());
    };
    const auto date = std::chrono::year(number(1)) / number(2) / number(3);
    Require(date.ok() && number(1) >= 1 && number(4) < 24 && number(5) < 60 && number(6) < 60,
        "invalid timestamp date/time");
    auto fraction = match[7].str();
    fraction.resize(6, '0');
    const auto zone = match[8].str();
    int offset = 0;
    if (zone != "Z") {
        const int hours = std::stoi(zone.substr(1, 2));
        const int minutes = std::stoi(zone.substr(4, 2));
        Require(hours < 24 && minutes < 60, "invalid timestamp offset");
        offset = (hours * 60 + minutes) * (zone[0] == '+' ? 1 : -1);
    }
    const auto instant = std::chrono::sys_days(date) + std::chrono::hours(number(4)) +
        std::chrono::minutes(number(5) - offset) + std::chrono::seconds(number(6)) +
        std::chrono::microseconds(std::stoi(fraction));
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(instant.time_since_epoch()).count();
    Require(micros >= -62135596800000000LL && micros <= 253402300799999999LL, "timestamp outside years 0001..9999");
    return micros;
}

std::string FormatTimestamp(int64_t micros)
{
    const std::chrono::sys_time<std::chrono::microseconds> instant{std::chrono::microseconds(micros)};
    const auto day = std::chrono::floor<std::chrono::days>(instant);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{instant - day};
    char buffer[40]{};
    std::snprintf(buffer,
        sizeof(buffer),
        "%04d-%02u-%02uT%02lld:%02lld:%02lld.%06lldZ",
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()),
        static_cast<long long>(time.hours().count()),
        static_cast<long long>(time.minutes().count()),
        static_cast<long long>(time.seconds().count()),
        static_cast<long long>(time.subseconds().count()));
    return buffer;
}

std::shared_ptr<arrow::DataType> ArrowType(FieldType type)
{
    switch (type) {
        case FieldType::String:
            return arrow::utf8();
        case FieldType::Bool:
            return arrow::boolean();
        case FieldType::Int64:
            return arrow::int64();
        case FieldType::Float64:
            return arrow::float64();
        case FieldType::TimestampUsUtc:
            return arrow::timestamp(arrow::TimeUnit::MICRO, "UTC");
    }
    throw TFailure({"INVALID_ARGUMENT", "unsupported field type"});
}

std::shared_ptr<arrow::Scalar> ParseScalar(FieldType type, const TJson& value)
{
    switch (type) {
        case FieldType::String:
            Require(value.is_string(), "expected string");
            return std::make_shared<arrow::StringScalar>(value.get<std::string>());
        case FieldType::Bool:
            Require(value.is_boolean(), "expected boolean");
            return std::make_shared<arrow::BooleanScalar>(value.get<bool>());
        case FieldType::Int64:
            return std::make_shared<arrow::Int64Scalar>(ParseInt64(value));
        case FieldType::Float64:
            Require(value.is_number() && std::isfinite(value.get<double>()), "expected finite number");
            return std::make_shared<arrow::DoubleScalar>(value.get<double>());
        case FieldType::TimestampUsUtc:
            return std::make_shared<arrow::TimestampScalar>(ParseTimestamp(value), ArrowType(type));
    }
    throw TFailure({"INVALID_ARGUMENT", "unsupported scalar type"});
}

void CheckArrow(const arrow::Status& status)
{
    Require(status.ok(), status.ToString(), "RESOURCE_EXHAUSTED");
}
} // namespace

void Require(bool condition, std::string_view message, std::string_view code)
{
    if (!condition) {
        throw TFailure({std::string(code), detail::BoundedDiagnostic(message)});
    }
}

TJson ParseJson(std::string_view bytes)
{
    Require(bytes.size() <= MaxRequestBytes, "request is too large", "RESOURCE_EXHAUSTED");
    TJsonValidator validator;
    Require(TJson::sax_parse(bytes, &validator), "invalid JSON request");
    return TJson::parse(bytes);
}

void CheckKeys(const TJson& value, std::initializer_list<std::string_view> keys)
{
    Require(value.is_object(), "expected JSON object");
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        Require(std::find(keys.begin(), keys.end(), key) != keys.end(),
            "unknown request field: " + detail::BoundedDiagnostic(key));
    }
}

int64_t ParseInt64(const TJson& value)
{
    Require(value.is_string(), "int64 and snapshot IDs require decimal strings");
    const auto text = value.get<std::string>();
    int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    Require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid int64");
    return result;
}

SchemaSpec ParseSchema(const TJson& value)
{
    CheckKeys(value, {"fields"});
    const auto& fields = value.at("fields");
    Require(fields.is_array() && !fields.empty() && fields.size() <= 256, "invalid schema fields");
    SchemaSpec result;
    for (const auto& field : fields) {
        CheckKeys(field, {"name", "type", "nullable", "field_id"});
        const auto name = field.at("type").get<std::string>();
        const auto typeIt = std::find(TypeNames.begin(), TypeNames.end(), name);
        Require(typeIt != TypeNames.end(), "unsupported field type");
        if (field.contains("field_id")) {
            Require(field["field_id"].is_number_integer() && field["field_id"] == result.fields.size() + 1,
                "field IDs must follow declaration order");
        }
        result.fields.push_back({field.at("name").get<std::string>(),
            static_cast<FieldType>(1 + typeIt - TypeNames.begin()),
            field.value("nullable", false)});
    }
    return result;
}

std::shared_ptr<arrow::RecordBatch> ParseRows(const SchemaSpec& schema, const TJson& rows)
{
    Require(rows.is_array() && !rows.empty() && rows.size() <= 10000, "append requires 1..10000 rows");
    std::vector<std::unique_ptr<arrow::ArrayBuilder>> builders;
    arrow::FieldVector fields;
    for (const auto& field : schema.fields) {
        const auto type = ArrowType(field.type);
        fields.push_back(arrow::field(field.name, type, field.nullable));
        std::unique_ptr<arrow::ArrayBuilder> builder;
        CheckArrow(arrow::MakeBuilder(arrow::default_memory_pool(), type, &builder));
        builders.push_back(std::move(builder));
    }
    for (const auto& row : rows) {
        Require(row.is_object(), "row must be an object");
        for (auto item = row.begin(); item != row.end(); ++item) {
            const auto& name = item.key();
            Require(std::ranges::any_of(schema.fields,
                        [&](const auto& field) {
                            return field.name == name;
                        }),
                "unknown row field");
        }
        for (size_t i = 0; i < schema.fields.size(); ++i) {
            const auto& field = schema.fields[i];
            const auto value = row.find(field.name);
            if (value == row.end() || value->is_null()) {
                Require(field.nullable, "missing or null required field");
                CheckArrow(builders[i]->AppendNull());
            } else {
                CheckArrow(builders[i]->AppendScalar(*ParseScalar(field.type, *value)));
            }
        }
    }
    arrow::ArrayVector arrays;
    for (auto& builder : builders) {
        std::shared_ptr<arrow::Array> array;
        CheckArrow(builder->Finish(&array));
        arrays.push_back(std::move(array));
    }
    return arrow::RecordBatch::Make(arrow::schema(fields), static_cast<int64_t>(rows.size()), arrays);
}

ScanRequest ParseScan(std::string collectionID, const SchemaSpec& schema, const TJson& body)
{
    CheckKeys(body, {"snapshot_id", "projection", "predicate", "limit", "batch_rows"});
    ScanRequest result;
    result.collection_id = std::move(collectionID);
    result.batch_rows = 1024;
    if (body.contains("snapshot_id")) {
        const auto& value = body["snapshot_id"];
        result.selector = value.is_null() || value == "initial" ? SnapshotSelector::InitialEmpty()
                                                                : SnapshotSelector::Id(ParseInt64(value));
    }
    if (body.contains("projection") && !body["projection"].is_null()) {
        result.projection = body["projection"].get<std::vector<std::string>>();
        Require(!result.projection->empty(), "zero-column projection is unavailable", "UNSUPPORTED_OPERATION");
    }
    for (const auto key : {"limit", "batch_rows"}) {
        if (!body.contains(key)) {
            continue;
        }
        Require(body[key].is_number_integer() && body[key] >= 0 && body[key] <= std::numeric_limits<int64_t>::max(),
            "invalid scan bound");
        const auto value = body[key].get<int64_t>();
        if (std::string_view(key) == "limit") {
            result.limit = value;
        } else {
            Require(value >= 1 && value <= 65536, "batch_rows must be 1..65536");
            result.batch_rows = value;
        }
    }
    if (!body.contains("predicate") || body["predicate"].is_null()) {
        return result;
    }
    const auto& predicate = body["predicate"];
    Require(predicate.is_object(), "predicate must be an object");
    const auto op = predicate.at("op").get<std::string>();
    if (op == "true") {
        CheckKeys(predicate, {"op"});
        return result;
    }
    const std::vector<std::string> operators{"eq", "ne", "lt", "le", "gt", "ge"};
    const auto opIt = std::find(operators.begin(), operators.end(), op);
    Require(opIt != operators.end(), "only one typed comparison is supported", "UNSUPPORTED_OPERATION");
    CheckKeys(predicate, {"op", "field", "value"});
    const auto name = predicate.at("field").get<std::string>();
    const auto fieldIt = std::ranges::find_if(schema.fields, [&](const auto& field) {
        return field.name == name;
    });
    Require(fieldIt != schema.fields.end(), "unknown predicate field");
    const auto scalar = ParseScalar(fieldIt->type, predicate.at("value"));
    Literal literal;
    switch (fieldIt->type) {
        case FieldType::String:
            literal = scalar->ToString();
            break;
        case FieldType::Bool:
            literal = static_cast<const arrow::BooleanScalar&>(*scalar).value;
            break;
        case FieldType::Int64:
            literal = static_cast<const arrow::Int64Scalar&>(*scalar).value;
            break;
        case FieldType::Float64:
            literal = static_cast<const arrow::DoubleScalar&>(*scalar).value;
            break;
        case FieldType::TimestampUsUtc:
            literal = TimestampUs{static_cast<const arrow::TimestampScalar&>(*scalar).value};
            break;
    }
    result.predicate = Predicate::Compare(name, static_cast<CompareOp>(opIt - operators.begin()), std::move(literal));
    return result;
}

TJson EncodeSchema(const arrow::Schema& schema)
{
    auto fields = TJson::array();
    for (const auto& field : schema.fields()) {
        std::string type;
        switch (field->type()->id()) {
            case arrow::Type::STRING:
                type = "string";
                break;
            case arrow::Type::BOOL:
                type = "bool";
                break;
            case arrow::Type::INT64:
                type = "int64";
                break;
            case arrow::Type::DOUBLE:
                type = "float64";
                break;
            case arrow::Type::TIMESTAMP:
                type = "timestamp_us_utc";
                break;
            default:
                throw TFailure({"INTERNAL", "unsupported output type"});
        }
        TJson item{{"name", field->name()}, {"type", type}, {"nullable", field->nullable()}};
        if (field->metadata()) {
            const auto id = field->metadata()->Get("PARQUET:field_id");
            if (id.ok()) {
                item["field_id"] = std::stoi(*id);
            }
        }
        fields.push_back(std::move(item));
    }
    return {{"fields", fields}};
}

TJson EncodeRows(const arrow::RecordBatch& batch)
{
    auto rows = TJson::array();
    size_t bytes = 0;
    for (int64_t row = 0; row < batch.num_rows(); ++row) {
        auto item = TJson::object();
        for (int column = 0; column < batch.num_columns(); ++column) {
            const auto& array = *batch.column(column);
            auto& value = item[batch.schema()->field(column)->name()];
            if (array.IsNull(row)) {
                value = nullptr;
                continue;
            }
            switch (array.type_id()) {
                case arrow::Type::STRING:
                    value = static_cast<const arrow::StringArray&>(array).GetString(row);
                    break;
                case arrow::Type::BOOL:
                    value = static_cast<const arrow::BooleanArray&>(array).Value(row);
                    break;
                case arrow::Type::INT64:
                    value = std::to_string(static_cast<const arrow::Int64Array&>(array).Value(row));
                    break;
                case arrow::Type::DOUBLE:
                    value = static_cast<const arrow::DoubleArray&>(array).Value(row);
                    break;
                case arrow::Type::TIMESTAMP:
                    value = FormatTimestamp(static_cast<const arrow::TimestampArray&>(array).Value(row));
                    break;
                default:
                    throw TFailure({"INTERNAL", "unsupported output type"});
            }
        }
        bytes += item.dump().size() + 1;
        Require(bytes <= MaxResponseBytes, "JSON response exceeds transport limit", "RESOURCE_EXHAUSTED");
        rows.push_back(std::move(item));
    }
    return rows;
}

TJson EncodeIdentity(const BaseIdentity& identity)
{
    return {{"store_id", identity.store_id},
        {"collection_id", identity.collection_id},
        {"table_uuid", identity.table_uuid},
        {"metadata_location", identity.metadata_location},
        {"current_snapshot_id",
            identity.current_snapshot_id ? TJson(std::to_string(*identity.current_snapshot_id)) : TJson(nullptr)},
        {"schema_id", identity.schema_id},
        {"spec_id", identity.spec_id}};
}

TJson EncodeError(const ErrorDetail& error)
{
    const std::vector<std::string> hints{"SameIdAfterReconcile", "NewOperationId", "FixInput", "OperatorAction"};
    return {{"error",
        {{"code", error.code},
            {"message", detail::BoundedDiagnostic(error.message)},
            {"retry_hint", hints.at(static_cast<size_t>(error.retry_hint))}}}};
}

int ErrorStatus(std::string_view code)
{
    if (code == "INVALID_ARGUMENT") {
        return 400;
    }
    if (code == "UNAUTHORIZED") {
        return 401;
    }
    if (code == "NOT_FOUND" || code == "COLLECTION_NOT_FOUND" || code == "SNAPSHOT_NOT_FOUND") {
        return 404;
    }
    if (code == "DATABASE_CLOSED" || code == "DATABASE_BUSY" || code == "DATABASE_REQUIRED" || code == "STATE_IN_USE" ||
        code == "ALREADY_EXISTS" || code == "OPERATION_CONFLICT" || code == "SCHEMA_MISMATCH")
    {
        return 409;
    }
    if (code == "PRECONDITION_FAILED") {
        return 412;
    }
    if (code == "RESOURCE_EXHAUSTED" || code == "CAPACITY_EXCEEDED") {
        return 429;
    }
    if (code == "UNSUPPORTED_OPERATION") {
        return 501;
    }
    if (code == "RECOVERY_REQUIRED" || code == "DEPENDENCY_UNAVAILABLE" || code == "OPERATION_IN_PROGRESS") {
        return 503;
    }
    return 500;
}
} // namespace glamdb::NTransport
