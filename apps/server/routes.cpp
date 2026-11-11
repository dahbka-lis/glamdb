#include "apps/server/routes.h"
#include "apps/server/databases.h"
#include "apps/server/scans.h"
#include "core/api/binary.h"

#include "apps/transport/outcome_json.h"
#include "core/api/canonical.h"

#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include <fstream>

namespace glamdb::NServer {
using namespace NTransport;
namespace {
bool IsApiPath(std::string_view path)
{
    return path == "/databases" || path.starts_with("/databases/") || path == "/capabilities" ||
        path == "/collections" || path == "/collections:resolve" || path.starts_with("/collections/");
}
TResponse JsonResponse(const TJson& value)
{
    auto body = value.dump();
    Require(body.size() <= MaxResponseBytes, "response exceeds transport limit", "RESOURCE_EXHAUSTED");
    return {200, std::move(body)};
}
TResponse OutcomeResponse(Result<json_adapter::Response> result)
{
    auto response = Unwrap(std::move(result));
    return {response.status, std::move(response.body)};
}
TJson Collection(const CollectionInfo& info)
{
    return TJson::parse(Unwrap(json_adapter::CollectionJson(info)));
}
std::vector<std::string> Split(const std::string& path)
{
    std::vector<std::string> parts;
    size_t position = 1;
    while (position <= path.size()) {
        const auto slash = path.find('/', position);
        parts.push_back(path.substr(position, slash == std::string::npos ? slash : slash - position));
        if (slash == std::string::npos) {
            break;
        }
        position = slash + 1;
    }
    return parts;
}
} // namespace

TRoutes::TRoutes(std::shared_ptr<Engine> engine, std::filesystem::path uiRoot, std::string token)
    : Scans_(std::make_shared<TScans>())
    , Engine_(std::move(engine))
    , UIRoot_(std::move(uiRoot))
    , Token_(std::move(token))
{ }

TRoutes::TRoutes(std::shared_ptr<TDatabases> databases, std::filesystem::path uiRoot, std::string token)
    : Scans_(std::make_shared<TScans>())
    , Databases_(std::move(databases))
    , UIRoot_(std::move(uiRoot))
    , Token_(std::move(token))
{
    std::weak_ptr<TScans> weak = Scans_;
    Databases_->SetScanCanceller([weak](const std::string& database) {
        if (const auto scans = weak.lock()) {
            scans->Clear(database);
        }
    });
}

bool TRoutes::IsAuthorized(const TRequest& request) const
{
    const auto path = DecodeUrl(request.Target.substr(0, request.Target.find('?')));
    if (!IsApiPath(path)) {
        return true;
    }
    const auto found = request.Headers.find("authorization");
    if (found == request.Headers.end()) {
        return false;
    }
    const auto expected = "Bearer " + Token_;
    size_t diff = expected.size() ^ found->second.size();
    for (size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(expected[i]) ^
            static_cast<unsigned char>(i < found->second.size() ? found->second[i] : 0);
    }
    return diff == 0;
}

TResponse TRoutes::Handle(const TRequest& request) const
{
    try {
        if (!IsAuthorized(request)) {
            return ErrorResponse(401, "UNAUTHORIZED", "bearer token required");
        }
        auto response = Route(request);
        Require(response.Body.size() <= MaxResponseBytes, "response exceeds transport limit", "RESOURCE_EXHAUSTED");
        return response;
    } catch (const TFailure& error) {
        return {ErrorStatus(error.Detail.code), EncodeError(error.Detail).dump()};
    } catch (const detail::Failure& error) {
        return {ErrorStatus(error.detail.code), EncodeError(error.detail).dump()};
    } catch (const TJson::exception&) {
        return ErrorResponse(400, "INVALID_ARGUMENT", "invalid JSON request");
    } catch (const std::bad_alloc&) {
        return ErrorResponse(503,
            "RESOURCE_EXHAUSTED",
            "request allocation failed; resolve write outcome by original ID");
    } catch (const std::exception&) {
        return ErrorResponse(500, "INTERNAL", "request failed; resolve write outcome by original ID");
    }
}

TResponse TRoutes::Route(const TRequest& request) const
{
    const auto [path, query] = ParseTarget(request.Target);
    if (request.Method != "GET" && request.Method != "POST") {
        return ErrorResponse(405, "METHOD_NOT_ALLOWED", "use GET or POST");
    }
    if (!IsApiPath(path)) {
        Require(request.Method == "GET", "static resources require GET");
        if (path == "/healthz") {
            return JsonResponse(
                {{"status", "live"}, {"backend", "local"}, {"mock", false}, {"durability_validation", "designed"}});
        }
        return StaticFile(path);
    }
    if (request.Method == "GET" && path == "/capabilities") {
        return JsonResponse({{"api_version", "v0"},
            {"backend", "local"},
            {"mock", false},
            {"durability_validation", "designed"},
            {"collection_modes", {"append"}},
            {"features",
                {{"database_management", static_cast<bool>(Databases_)},
                    {"append", true},
                    {"scan", true},
                    {"scan_pagination", true},
                    {"scan_cancel", true},
                    {"arrow_export", true},
                    {"stats", false},
                    {"snapshots", false},
                    {"files", false},
                    {"explain", false},
                    {"pagination", false},
                    {"zero_column_projection", false}}},
            {"predicate_ops", {"true", "eq", "ne", "lt", "le", "gt", "ge"}},
            {"limits",
                {{"request_bytes", MaxRequestBytes},
                    {"response_bytes", MaxResponseBytes},
                    {"append_rows", 10000},
                    {"scan_page_rows", 10000},
                    {"scan_sessions", 8},
                    {"scan_idle_seconds", 60},
                    {"batch_rows", 65536}}}});
    }
    TJson body;
    if (request.Method == "POST") {
        const auto type = request.Headers.find("content-type");
        if (type == request.Headers.end() ||
            (type->second != "application/json" && type->second != "application/json; charset=utf-8"))
        {
            return ErrorResponse(415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json");
        }
        Require(query.empty(), "POST query parameters are unsupported");
        body = ParseJson(request.Body);
        Require(body.is_object(), "request body must be an object");
    }
    if (path == "/databases" || path.starts_with("/databases/")) {
        Require(static_cast<bool>(Databases_), "database management requires --data-dir", "UNSUPPORTED_OPERATION");
        if (path == "/databases") {
            Require(query.empty(), "unknown query parameter");
            return JsonResponse(request.Method == "GET" ? Databases_->List() : Databases_->Create(body));
        }
        const auto parts = Split(path);
        if (parts.size() == 2 && request.Method == "GET") {
            Require(query.empty(), "unknown query parameter");
            return JsonResponse(Databases_->Get(parts[1]));
        }
        if (parts.size() == 3 && request.Method == "POST" &&
            (parts[2] == "open" || parts[2] == "close" || parts[2] == "reconcile"))
        {
            return JsonResponse(Databases_->Change(parts[1], parts[2], body));
        }
        Require(parts.size() >= 3 &&
                (parts[2] == "collections" || parts[2] == "collections:resolve" || parts[2] == "capabilities"),
            "route not found",
            "NOT_FOUND");
        // Dispatch using the raw suffix: percent decoding must happen exactly once.
        const auto rawPath = request.Target.substr(0, request.Target.find('?'));
        const auto first = rawPath.find('/', 1);
        const auto second = first == std::string::npos ? first : rawPath.find('/', first + 1);
        Require(second != std::string::npos, "invalid database route");
        auto scoped = request;
        scoped.Target = request.Target.substr(second);
        return Databases_->Use(parts[1], [&](const auto& engine) {
            auto routes = *this;
            routes.Engine_ = engine;
            routes.Databases_.reset();
            routes.DatabaseID_ = parts[1];
            return routes.Handle(scoped);
        });
    }
    Require(static_cast<bool>(Engine_), "select a database using /databases/{id}", "DATABASE_REQUIRED");
    if (request.Method == "GET" && path == "/collections:resolve") {
        Require(query.size() == 1 && query.contains("name"), "exact name is required");
        return JsonResponse(Collection(Unwrap(Engine_->ResolveCollection(query.at("name")))));
    }
    if (request.Method == "GET" && path == "/collections") {
        for (const auto& [key, value] : query) {
            (void)value;
            Require(key == "page_size" || key == "cursor", "unknown query parameter");
        }
        Require(!query.contains("cursor"), "pagination is unavailable", "UNSUPPORTED_OPERATION");
        int64_t pageSize = 100;
        if (query.contains("page_size")) {
            pageSize = ParseInt64(query.at("page_size"));
        }
        Require(pageSize >= 1 && pageSize <= 1000, "page_size must be 1..1000");
        const auto collections = Unwrap(Engine_->ListCollections());
        Require(collections.size() <= static_cast<size_t>(pageSize),
            "increase page_size; pagination is unavailable",
            "UNSUPPORTED_OPERATION");
        auto items = TJson::array();
        for (const auto& info : collections) {
            items.push_back(Collection(info));
        }
        return JsonResponse({{"items", items}, {"next_cursor", nullptr}});
    }
    if (request.Method == "POST" && path == "/collections") {
        CheckKeys(body, {"collection_id", "name", "schema", "mode"});
        Require(body.value("mode", "append") == "append",
            "only append collections are supported",
            "UNSUPPORTED_OPERATION");
        return OutcomeResponse(
            json_adapter::ToJson(Engine_->CreateCollection({body.at("collection_id").get<std::string>(),
                body.at("name").get<std::string>(),
                ParseSchema(body.at("schema"))})));
    }
    const auto parts = Split(path);
    Require(parts.size() >= 2 && parts[0] == "collections", "route not found", "NOT_FOUND");
    const auto& id = parts[1];
    if (request.Method == "GET" && parts.size() == 4 && parts[2] == "operations") {
        Require(query.empty(), "unknown query parameter");
        auto outcome = Unwrap(Engine_->FindAppendOutcome(id, parts[3]));
        if (!outcome.has_value()) {
            throw TFailure({"NOT_FOUND", "operation not found"});
        }
        return OutcomeResponse(json_adapter::ToJson(*outcome));
    }
    const std::string action = parts.size() >= 3 ? parts[2] : "";
    if (request.Method == "POST" && parts.size() == 4 && action == "scan" && parts[3] == "cancel") {
        // Releasing a scoped cursor must remain possible when an unrelated
        // core failure has fenced catalog reads pending reconciliation.
        Require(query.empty(), "unknown query parameter");
        return Scans_->Cancel(DatabaseID_, id, body);
    }
    const auto info = Unwrap(Engine_->GetCollection(id));
    if ((action == "stats" || action == "snapshots" || action == "files" || action == "explain")) {
        throw TFailure({"UNSUPPORTED_OPERATION", "native metadata/explain API is not available; inspect capabilities"});
    }
    Require(query.empty(), "unknown query parameter");
    if (request.Method == "GET") {
        if (parts.size() == 2) {
            return JsonResponse(Collection(info));
        }
        if (parts.size() == 3 && action == "schema") {
            return JsonResponse(Collection(info).at("schema"));
        }
    }
    if (request.Method == "POST" && (parts.size() == 3 || (parts.size() == 4 && action == "scan"))) {
        if (action == "append" && parts.size() == 3) {
            CheckKeys(body, {"operation_id", "rows", "expected_snapshot"});
            AppendRequest append;
            append.collection_id = id;
            append.operation_id = body.at("operation_id").get<std::string>();
            if (body.contains("expected_snapshot")) {
                append.expected_snapshot = body["expected_snapshot"].is_null()
                    ? SnapshotPrecondition{SnapshotPreconditionKind::NoSnapshot, 0}
                    : SnapshotPrecondition{SnapshotPreconditionKind::Equals, ParseInt64(body["expected_snapshot"])};
            }
            append.batches.push_back(ParseRows(info.schema, body.at("rows")));
            return OutcomeResponse(json_adapter::ToJson(Engine_->AppendDurable(append)));
        }
        if (action == "scan") {
            Require(parts.size() == 3 || parts[3] == "open", "route not found", "NOT_FOUND");
            const auto accept = request.Headers.find("accept");
            const auto media = accept == request.Headers.end() ? "application/json" : accept->second;
            if (media != "application/json" && media != "*/*" && media != "application/vnd.apache.arrow.stream") {
                return ErrorResponse(406, "NOT_ACCEPTABLE", "use JSON or Arrow stream");
            }
            return Scans_->Open(Engine_,
                DatabaseID_,
                id,
                body,
                media == "application/vnd.apache.arrow.stream",
                parts.size() == 4);
        }
    }
    return ErrorResponse(404, "NOT_FOUND", "route not found");
}

void TRoutes::CancelScans() const
{
    Scans_->Clear();
}
void TRoutes::CancelScans(const std::string& database) const
{
    Scans_->Clear(database);
}

TResponse TRoutes::StaticFile(const std::string& path) const
{
    const auto relative = path == "/" ? "index.html"
        : path == "/workbench/config" ? "config/theme.json"
                                      : path.substr(1);
    if (UIRoot_.empty() ||
        !(relative == "index.html" || relative == "config/theme.json" || relative.starts_with("assets/")))
    {
        return ErrorResponse(404, "NOT_FOUND", "resource not found");
    }
    std::filesystem::path file = UIRoot_;
    for (const auto& component : std::filesystem::path(relative)) {
        if (component == ".." || component == "." || component.empty()) {
            return ErrorResponse(404, "NOT_FOUND", "resource not found");
        }
        file /= component;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(file))) {
            return ErrorResponse(404, "NOT_FOUND", "resource not found");
        }
    }
    if (!std::filesystem::is_regular_file(file) || std::filesystem::file_size(file) > MaxResponseBytes) {
        return ErrorResponse(404, "NOT_FOUND", "resource not found");
    }
    std::ifstream input(file, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Require(input.good() || input.eof(), "cannot read UI asset", "IO_ERROR");
    const std::map<std::string, std::string> types{{".html", "text/html; charset=utf-8"},
        {".css", "text/css"},
        {".js", "text/javascript"},
        {".json", "application/json"},
        {".png", "image/png"},
        {".svg", "image/svg+xml"}};
    const auto type = types.find(file.extension().string());
    if (type == types.end()) {
        return ErrorResponse(404, "NOT_FOUND", "resource not found");
    }
    return {200, std::move(data), type->second};
}
} // namespace glamdb::NServer
