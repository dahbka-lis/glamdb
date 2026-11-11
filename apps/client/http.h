#pragma once
#include <map>
#include <string>

namespace glamdb::NClient {
struct TRequest
{
    std::string Url;
    std::string Method = "GET";
    std::string Body;
    std::string Accept = "application/json";
    std::string ContentType = "application/json";
    std::string CaFile;
    unsigned TimeoutSeconds = 65;
};
struct TResponse
{
    int Status = 0;
    std::map<std::string, std::string> Headers;
    std::string Body;
};
TResponse SendRequest(const TRequest& request);
}
