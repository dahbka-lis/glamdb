#include "apps/transport/http.h"

#include <gtest/gtest.h>

namespace glamdb::NTransport {
TEST(Transport, RejectsAmbiguousJsonAndFraming)
{
    EXPECT_THROW(ParseJson(R"({"operation_id":"a","operation_id":"b"})"), TFailure);
    EXPECT_THROW(ParseJson(std::string(34, '[') + "0" + std::string(34, ']')), TFailure);
    for (const auto* header : {"Content-Length: 1\r\nContent-Length: 1\r\n",
             "Transfer-Encoding: chunked\r\n",
             " Content-Length: 1\r\n",
             "Content-Length : 1\r\n",
             "Content-Length: +1\r\n",
             "Content-Length: 1,1\r\n"})
    {
        EXPECT_THROW(ParseHeaders(std::string("POST /collections HTTP/1.1\r\nHost: local\r\n") + header + "\r\n"),
            TFailure);
    }
}
TEST(Transport, PreservesExactScalarsAndRejectsInvalidValues)
{
    const SchemaSpec schema{
        {{"key", FieldType::String, false}, {"n", FieldType::Int64, true}, {"t", FieldType::TimestampUsUtc, true}}};
    const auto batch = ParseRows(schema,
        ParseJson(
            R"([{"key":"x","n":"-9223372036854775808","t":"0001-01-01T00:00:00Z"},{"key":"x","n":"9223372036854775807","t":"9999-12-31T23:59:59.999999Z"}])"));
    const auto rows = EncodeRows(*batch);
    EXPECT_EQ(rows[0]["n"], "-9223372036854775808");
    EXPECT_EQ(rows[0]["t"], "0001-01-01T00:00:00.000000Z");
    EXPECT_EQ(rows[1]["n"], "9223372036854775807");
    EXPECT_EQ(rows[1]["t"], "9999-12-31T23:59:59.999999Z");
    for (const auto* timestamp : {"0000-01-01T00:00:00Z",
             "2026-02-30T00:00:00Z",
             "2026-01-01T24:00:00Z",
             "2026-01-01T00:00:60Z",
             "2026-01-01T00:00:00.0000001Z",
             "0001-01-01T00:00:00+01:00"})
    {
        EXPECT_THROW(ParseRows(schema, TJson::array({{{"key", "x"}, {"t", timestamp}}})), TFailure);
    }
    EXPECT_THROW(ParseInt64(TJson(true)), TFailure);
    EXPECT_THROW(ParseInt64(TJson(9007199254740993LL)), TFailure);
    EXPECT_THROW(ParseInt64(TJson("9223372036854775808")), TFailure);
}
TEST(Transport, RejectsUnknownFieldsAndUnsupportedQueries)
{
    const SchemaSpec schema{{{"key", FieldType::String, false}}};
    EXPECT_FALSE(ParseScan("id", schema, ParseJson(R"({"projection":null})")).projection.has_value());
    EXPECT_THROW(ParseRows(schema, ParseJson(R"([{"key":"x","extra":1}])")), TFailure);
    EXPECT_THROW(ParseScan("id", schema, ParseJson(R"({"projection":[]})")), TFailure);
    EXPECT_THROW(ParseScan("id", schema, ParseJson(R"({"predicate":{"op":"or","args":[]}})")), TFailure);
    EXPECT_THROW(ParseScan("id", schema, ParseJson(R"({"limit":true})")), TFailure);
    EXPECT_THROW(ParseScan("id", schema, ParseJson(R"({"limit":-1})")), TFailure);
    EXPECT_THROW(ParseTarget("/collections?name=a&name=b"), TFailure);
    EXPECT_THROW(ParseTarget("/%0a"), TFailure);
}
} // namespace glamdb::NTransport
