#pragma once

#include "apps/transport/http.h"

#include <filesystem>

namespace glamdb::NServer {
class TRoutes
{
public:
    TRoutes(std::shared_ptr<Engine> engine, std::filesystem::path uiRoot, std::string token);
    NTransport::TResponse Handle(const NTransport::TRequest& request) const;
    bool IsAuthorized(const NTransport::TRequest& request) const;

private:
    NTransport::TResponse Route(const NTransport::TRequest& request) const;
    NTransport::TResponse Scan(const std::string& collectionID, const NTransport::TJson& body, bool arrow) const;
    NTransport::TResponse StaticFile(const std::string& path) const;
    std::shared_ptr<Engine> Engine_;
    std::filesystem::path UIRoot_;
    std::string Token_;
};
} // namespace glamdb::NServer
