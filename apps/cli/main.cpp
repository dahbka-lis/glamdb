#include "apps/transport/http.h"
#include "storage/engine.h"
#include <fstream>

#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv)
try {
    CLI::App app{"GlamDB store management and HTTP client"};
    app.require_subcommand(1);
    std::string root;
    std::string id;
    auto* create = app.add_subcommand("create-store", "Create an empty store");
    auto* inspect = app.add_subcommand("inspect-store", "Inspect an existing store");
    for (auto* command : {create, inspect}) {
        command->add_option("--root", root, "Absolute store path")->required();
        command->add_option("--store-id", id, "Store UUID")->required();
    }
    auto* request = app.add_subcommand("request", "Call the running server once; print the JSON response");
    std::string host = "127.0.0.1", path, method = "GET", file;
    uint16_t port = 8080;
    request->add_option("--host", host, "Loopback HTTP address");
    request->add_option("--port", port, "Server port");
    request->add_option("--path", path, "API path, for example /databases")->required();
    request->add_option("--method", method, "GET or POST");
    request->add_option("--body", file, "JSON file, or - for stdin");
    if (argc == 1) {
        std::cout << app.help(argv[0]);
        return 0;
    }
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const auto code = app.exit(error);
        return code == 0 ? 0 : 2;
    }
    if (request->parsed()) {
        glamdb::NTransport::TRequest input;
        input.Method = method;
        input.Target = path;
        if (!file.empty()) {
            std::ifstream source;
            if (file != "-") {
                source.open(file, std::ios::binary);
                glamdb::NTransport::Require(source.is_open(), "cannot open JSON input file");
            }
            auto& stream = file == "-" ? std::cin : source;
            char block[4096];
            while (stream.read(block, sizeof(block)) || stream.gcount() > 0) {
                input.Body.append(block, static_cast<size_t>(stream.gcount()));
                glamdb::NTransport::Require(input.Body.size() <= glamdb::NTransport::MaxRequestBytes,
                    "input body too large");
            }
            glamdb::NTransport::Require(!stream.bad(), "input read failed");
            if (request->count("--method") == 0) {
                input.Method = "POST";
            }
        }
        const auto response = glamdb::NTransport::SendRequest(host, port, input);
        std::cout << response.Body << '\n';
        return response.Status >= 200 && response.Status < 300 ? 0 : 1;
    }
    const auto opened = create->parsed() ? glamdb::CreateStore({id, root}) : glamdb::OpenExisting({root, id});
    if (!opened) {
        std::cerr << opened.error().code << ": " << opened.error().message << '\n';
        return 1;
    }
    const auto& info = (*opened)->GetStoreInfo();
    // StoreInfo contains validated UUID/profile tokens, never arbitrary
    // strings.
    std::cout << "{\"store_id\":\"" << info.store_id << "\",\"profile_id\":\"" << info.profile_id
              << "\",\"format_version\":" << info.format_version << ",\"durability_validation\":\"designed\"}\n";
    const auto closed = (*opened)->Shutdown(std::chrono::milliseconds(5000));
    if (!closed) {
        std::cerr << closed.error().code << ": " << closed.error().message << '\n';
        return 1;
    }
    return 0;
} catch (const glamdb::NTransport::TFailure& error) {
    std::cerr << error.Detail.code << ": " << error.Detail.message << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
} catch (...) {
    return 100;
}
