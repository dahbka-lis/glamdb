#include "apps/client/http.h"
#include <CLI/CLI.hpp>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
void Write(const std::filesystem::path& path, std::string_view bytes)
{
    if (path.empty() || path == "-") {
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        if (!std::cout) {
            throw std::runtime_error("output write failed; server operation may already have committed");
        }
    } else {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        output.close();
        if (!output) {
            throw std::runtime_error("output write or close failed; server operation may already have committed");
        }
    }
}
}
int main(int argc, char** argv)
try {
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    CLI::App app{"GlamDB network client"};
    app.require_subcommand(1);
    auto* request = app.add_subcommand("request", "Call any server API route; write exact response bytes");
    auto* create = app.add_subcommand("create-store", "Create a database through the running server");
    glamdb::NClient::TRequest input;
    std::string url, host = "127.0.0.1", path, file, output, headers, name;
    uint16_t port = 8080;
    for (auto* command : {request, create}) {
        command->add_option("--url", url, "Full http(s) URL (create-store: server base URL)");
        command->add_option("--host", host, "Loopback HTTP hostname; use --url for HTTPS");
        command->add_option("--port", port, "HTTP port");
        command->add_option("--ca-file", input.CaFile, "Additional PEM certificate authority");
        command->add_option("--timeout", input.TimeoutSeconds, "Whole request deadline in seconds");
        command->add_option("--output,-o", output, "Response file; default stdout; binary-safe");
        command->add_option("--headers", headers, "Write status and response headers as JSON to this file");
    }
    request->add_option("--path", path, "API route (required unless --url includes the route)");
    request->add_option("--method", input.Method, "GET, POST or DELETE");
    request->add_option("--body", file, "Request bytes file, or - for stdin");
    request->add_option("--accept", input.Accept, "Response media type, including application/vnd.apache.arrow.stream");
    request->add_option("--content-type", input.ContentType, "Request media type");
    create->add_option("--name", name, "Database name")->required();
    if (argc == 1) {
        std::cout << app.help();
        return 0;
    }
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        return app.exit(error) == 0 ? 0 : 2;
    }
    if (!url.empty() &&
        (request->count("--host") || request->count("--port") || create->count("--host") || create->count("--port")))
    {
        throw std::runtime_error("--url cannot be combined with --host/--port");
    }
    if (headers == "-" || (!headers.empty() && headers == output)) {
        throw std::runtime_error("headers require a separate output file");
    }
    if (create->parsed()) {
        path = "/databases";
        input.Method = "POST";
        input.Body = nlohmann::json{{"name", name}}.dump();
    } else if (!file.empty()) {
        std::ifstream source;
        if (file != "-") {
            source.open(file, std::ios::binary);
            if (!source) {
                throw std::runtime_error("cannot open input file");
            }
        }
        auto& stream = file == "-" ? std::cin : source;
        char block[4096];
        while (stream.read(block, sizeof(block)) || stream.gcount() > 0) {
            input.Body.append(block, static_cast<size_t>(stream.gcount()));
            if (input.Body.size() > size_t{16} * 1024 * 1024) {
                throw std::runtime_error("input body too large");
            }
        }
        if (stream.bad()) {
            throw std::runtime_error("input read failed");
        }
        if (!request->count("--method")) {
            input.Method = "POST";
        }
    }
    if (!path.empty() && (!path.starts_with('/') || path.starts_with("//"))) {
        throw std::runtime_error("path must begin with one slash");
    }
    if (url.empty()) {
        if (path.empty()) {
            throw std::runtime_error("--path or --url is required");
        }
        if (host.find(':') != std::string::npos && !host.starts_with('[')) {
            host = '[' + host + ']';
        }
        input.Url = "http://" + host + ':' + std::to_string(port) + path;
    } else {
        if (!path.empty()) {
            while (url.ends_with('/')) {
                url.pop_back();
            }
            url += path;
        }
        input.Url = url;
    }
    const auto response = glamdb::NClient::SendRequest(input);
    if (!headers.empty()) {
        Write(headers, nlohmann::json{{"status", response.Status}, {"headers", response.Headers}}.dump(2) + '\n');
    }
    Write(output, response.Body);
    return response.Status >= 200 && response.Status < 300 ? 0 : 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
} catch (...) {
    return 100;
}
