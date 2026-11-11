#pragma once

#include "apps/transport/codec.h"

#include <map>

namespace glamdb::NTransport {
struct TRequest
{
    std::string Method;
    std::string Target;
    std::map<std::string, std::string> Headers;
    std::string Body;
    size_t ContentLength = 0;
};
struct TResponse
{
    int Status = 200;
    std::string Body;
    std::string ContentType = "application/json";
    std::map<std::string, std::string> Headers{};
};
TRequest ParseHeaders(std::string_view bytes);
std::string SerializeResponse(const TResponse& response);
TResponse ErrorResponse(int status, std::string_view code, std::string_view message);
std::string DecodeUrl(std::string_view text);
std::pair<std::string, std::map<std::string, std::string>> ParseTarget(std::string_view target);
std::string ReadToken();
} // namespace glamdb::NTransport
