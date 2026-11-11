#pragma once

#include "apps/transport/http.h"

#include <memory>

namespace glamdb::NServer {
// Bounded, expiring fixed-view cursors. Tokens are scoped to database, collection
// and representation; only the last consumed page can be replayed.
class TScans
{
public:
    TScans();
    ~TScans();
    NTransport::TResponse Open(const std::shared_ptr<Engine>& engine,
        const std::string& database,
        const std::string& collection,
        const NTransport::TJson& body,
        bool arrow,
        bool openOnly);
    NTransport::TResponse
    Cancel(const std::string& database, const std::string& collection, const NTransport::TJson& body);
    void Clear(const std::string& database);
    void Clear();

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};
} // namespace glamdb::NServer
