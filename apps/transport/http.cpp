#include "apps/transport/http.h"

#include <asio.hpp>

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace glamdb::NTransport {
namespace {
std::string Lower(std::string value)
{
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return value;
}
std::string Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t") - first + 1));
}
size_t ParseSize(std::string_view text)
{
    size_t size = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), size);
    Require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
        "invalid Content-Length");
    return size;
}
bool IsToken(std::string_view text)
{
    return !text.empty() && std::ranges::all_of(text, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            std::string_view("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) != std::string_view::npos;
    });
}
} // namespace

TRequest ParseHeaders(std::string_view bytes)
{
    Require(bytes.size() <= 16384 && bytes.ends_with("\r\n\r\n"), "invalid HTTP headers");
    TRequest result;
    const auto lineEnd = bytes.find("\r\n");
    const auto line = bytes.substr(0, lineEnd);
    const auto first = line.find(' '), last = line.rfind(' ');
    Require(first != std::string_view::npos && first != last && line.substr(last + 1) == "HTTP/1.1",
        "expected HTTP/1.1 request");
    result.Method = line.substr(0, first);
    result.Target = line.substr(first + 1, last - first - 1);
    Require(IsToken(result.Method) && result.Target.starts_with('/') && result.Target.size() <= 4096 &&
            std::ranges::all_of(result.Target,
                [](unsigned char c) {
                    return c > 32 && c < 127;
                }),
        "invalid request target");
    size_t position = lineEnd + 2;
    while (position < bytes.size() - 2) {
        const auto end = bytes.find("\r\n", position);
        const auto header = bytes.substr(position, end - position);
        const auto colon = header.find(':');
        Require(colon != std::string_view::npos && IsToken(header.substr(0, colon)), "invalid header name");
        auto name = Lower(std::string(header.substr(0, colon)));
        auto value = Trim(header.substr(colon + 1));
        Require(std::ranges::all_of(value,
                    [](unsigned char c) {
                        return c == 9 || (c >= 32 && c != 127);
                    }),
            "invalid header value");
        Require(result.Headers.emplace(std::move(name), std::move(value)).second, "duplicate header");
        position = end + 2;
    }
    Require(result.Headers.contains("host") && !result.Headers.at("host").empty(), "Host is required");
    Require(!result.Headers.contains("transfer-encoding"), "Transfer-Encoding is unsupported");
    Require(!result.Headers.contains("expect"), "Expect is unsupported");
    Require(!result.Headers.contains("content-encoding") || result.Headers.at("content-encoding") == "identity",
        "compressed requests are unsupported");
    if (result.Headers.contains("content-length")) {
        result.ContentLength = ParseSize(result.Headers.at("content-length"));
    }
    Require(result.ContentLength <= MaxRequestBytes, "request body is too large", "PAYLOAD_TOO_LARGE");
    Require(result.Method != "POST" || result.Headers.contains("content-length"), "POST requires Content-Length");
    Require(result.Method == "POST" || result.ContentLength == 0, "body only allowed for POST");
    return result;
}

std::string SerializeResponse(const TResponse& response)
{
    std::string output = "HTTP/1.1 " + std::to_string(response.Status) +
        " Response\r\nContent-Type: " + response.ContentType +
        "\r\nContent-Length: " + std::to_string(response.Body.size()) +
        "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: "
        "no-referrer\r\n";
    for (const auto& [name, value] : response.Headers) {
        output.append(name).append(": ").append(value).append("\r\n");
    }
    return output + "\r\n" + response.Body;
}

TResponse ErrorResponse(int status, std::string_view code, std::string_view message)
{
    return {status, EncodeError({std::string(code), std::string(message)}).dump()};
}

std::string DecodeUrl(std::string_view text)
{
    std::string result;
    for (size_t i = 0; i < text.size(); ++i) {
        auto c = text[i];
        if (c == '%') {
            Require(i + 2 < text.size(), "invalid URL escape");
            unsigned value = 0;
            const auto parsed = std::from_chars(text.data() + i + 1, text.data() + i + 3, value, 16);
            Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + i + 3, "invalid URL escape");
            c = static_cast<char>(value);
            i += 2;
        } else if (c == '+') {
            c = ' ';
        }
        Require(static_cast<unsigned char>(c) >= 32 && c != 127, "invalid URL character");
        result += c;
    }
    return result;
}

std::pair<std::string, std::map<std::string, std::string>> ParseTarget(std::string_view target)
{
    const auto question = target.find('?');
    std::pair<std::string, std::map<std::string, std::string>> result{DecodeUrl(target.substr(0, question)), {}};
    if (question == std::string_view::npos) {
        return result;
    }
    size_t position = question + 1;
    while (position < target.size()) {
        const auto end = target.find('&', position);
        const auto pair = target.substr(position, end == std::string_view::npos ? end : end - position);
        const auto equals = pair.find('=');
        Require(equals != std::string_view::npos &&
                result.second.emplace(DecodeUrl(pair.substr(0, equals)), DecodeUrl(pair.substr(equals + 1))).second,
            "invalid or duplicate query parameter");
        if (end == std::string_view::npos) {
            break;
        }
        position = end + 1;
    }
    return result;
}

std::string ReadToken()
{
    const char* value = std::getenv("GLAMDB_TOKEN");
    const std::string token = value ? value : "";
    Require(!token.empty() && token.size() <= 256 &&
            std::ranges::all_of(token,
                [](unsigned char c) {
                    return c >= 33 && c <= 126;
                }),
        "set GLAMDB_TOKEN to a nonempty printable ASCII bearer token");
    return token;
}

TResponse SendRequest(std::string_view host, uint16_t port, const TRequest& request)
{
    // The CLI's direct HTTP transport is loopback-only. Use SDK HTTPS through a
    // TLS proxy for remote hosts; never send a bearer token over remote HTTP.
    const auto address = asio::ip::make_address(host);
    Require(address.is_loopback(), "CLI HTTP requires a loopback address");
    Require(request.Target.starts_with('/') && request.Target.find_first_of("\r\n ") == std::string::npos,
        "invalid request path");
    Require(request.Method == "GET" || request.Method == "POST", "request method must be GET or POST");
    std::ostringstream output;
    output << request.Method << ' ' << request.Target << " HTTP/1.1\r\nHost: " << host << ':' << port
           << "\r\nAuthorization: Bearer " << ReadToken()
           << "\r\nAccept: application/json\r\nContent-Type: application/json\r\nContent-Length: "
           << request.Body.size() << "\r\nConnection: close\r\n\r\n"
           << request.Body;
    const auto bytes = output.str();
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    asio::steady_timer timer(io, std::chrono::seconds(65));
    std::error_code result;
    std::string reply;
    auto finish = [&](const std::error_code& error) {
        result = error;
        timer.cancel();
        std::error_code ignored;
        socket.close(ignored);
    };
    timer.async_wait([&](const std::error_code& error) {
        if (!error) {
            std::error_code ignored;
            socket.close(ignored);
        }
    });
    socket.async_connect(asio::ip::tcp::endpoint(address, port), [&](const std::error_code& error) {
        if (error) {
            finish(error);
            return;
        }
        asio::async_write(socket, asio::buffer(bytes), [&](const std::error_code& writeError, size_t) {
            if (writeError) {
                finish(writeError);
                return;
            }
            asio::async_read(socket,
                asio::dynamic_buffer(reply, MaxResponseBytes + 16384),
                [&](const std::error_code& readError, size_t) {
                    finish(readError);
                });
        });
    });
    io.run();
    Require(result == asio::error::eof, "HTTP exchange failed; write outcome may be unknown", "IO_ERROR");
    const auto headerEnd = reply.find("\r\n\r\n");
    Require(headerEnd != std::string::npos && headerEnd + 4 <= 16384 && reply.starts_with("HTTP/1.1 "),
        "invalid HTTP response; write outcome may be unknown",
        "IO_ERROR");
    const auto header = reply.substr(0, headerEnd + 4);
    TResponse response;
    const auto parsed = std::from_chars(header.data() + 9, header.data() + 12, response.Status);
    Require(parsed.ec == std::errc{} && parsed.ptr == header.data() + 12, "invalid HTTP status", "IO_ERROR");
    auto lower = Lower(header);
    const auto lengthPos = lower.find("\r\ncontent-length: ");
    Require(lengthPos != std::string::npos, "missing response length", "IO_ERROR");
    const auto start = lengthPos + 18;
    const auto size = ParseSize(std::string_view(header).substr(start, header.find("\r\n", start) - start));
    Require(size <= MaxResponseBytes, "HTTP response too large", "IO_ERROR");
    Require(reply.size() - header.size() == size, "interrupted response; write outcome may be unknown", "IO_ERROR");
    response.Body = reply.substr(header.size());
    return response;
}
} // namespace glamdb::NTransport
