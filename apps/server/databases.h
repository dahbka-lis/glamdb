#pragma once

#include "apps/transport/http.h"

#include <filesystem>
#include <functional>

namespace glamdb::NServer {
// Owns the service registry and leases each Engine for a complete HTTP call.
class TDatabases
{
public:
    // Optional VFS is an internal seam for deterministic registry fault tests.
    explicit TDatabases(const std::filesystem::path& directory, const std::string& registryVfs = {});
    ~TDatabases();
    TDatabases(const TDatabases&) = delete;
    TDatabases& operator=(const TDatabases&) = delete;
    NTransport::TJson List() const;
    NTransport::TJson Get(const std::string& id) const;
    NTransport::TJson Create(const NTransport::TJson& body);
    NTransport::TJson Change(const std::string& id, std::string_view action);
    NTransport::TResponse Use(const std::string& id,
        const std::function<NTransport::TResponse(std::shared_ptr<Engine>)>& call) const;
    void Shutdown();

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};
} // namespace glamdb::NServer
