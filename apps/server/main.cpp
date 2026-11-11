#include "apps/server/server.h"

#include <CLI/CLI.hpp>

#include <iostream>

int main(int argc, char** argv)
try {
    CLI::App app{"GlamDB native HTTP server"};
    std::string root, storeID, uiRoot;
    std::string address = "127.0.0.1";
    uint16_t port = 8080;
    int timeoutMs = 10000;
    app.add_option("--root", root, "Existing store path")->required();
    app.add_option("--store-id", storeID, "Expected store UUID")->required();
    app.add_option("--listen", address, "HTTP listen address; use TLS proxy outside loopback");
    app.add_option("--port", port, "HTTP port; zero selects an available port");
    app.add_option("--ui-root", uiRoot, "Workbench directory (defaults to installed share/glamdb/ui)");
    app.add_option("--request-timeout-ms", timeoutMs, "Total header/body read timeout")->check(CLI::Range(100, 60000));
    CLI11_PARSE(app, argc, argv);
    const auto token = glamdb::NTransport::ReadToken();
    if (uiRoot.empty()) {
        const auto executable = std::filesystem::canonical("/proc/self/exe");
        const auto installed = executable.parent_path().parent_path() / "share/glamdb/ui";
        if (std::filesystem::is_directory(installed)) {
            uiRoot = installed.string();
        }
    }
    auto engine = glamdb::NTransport::Unwrap(glamdb::OpenExisting({root, storeID}));
    glamdb::NServer::TRoutes routes(engine, uiRoot, token);
    glamdb::NServer::RunServer(routes, address, port, std::chrono::milliseconds(timeoutMs));
    const auto closed = engine->Shutdown(std::chrono::milliseconds(60000));
    if (!closed) {
        std::cerr << closed.error().code << ": " << closed.error().message << '\n';
        return 1;
    }
    return 0;
} catch (const glamdb::NTransport::TFailure& error) {
    std::cerr << error.Detail.code << ": " << error.Detail.message << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << "server failed: " << error.what() << '\n';
    return 1;
}
