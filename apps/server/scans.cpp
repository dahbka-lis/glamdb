#include "apps/server/scans.h"
#include "core/api/binary.h"
#include "core/api/canonical.h"

#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

namespace glamdb::NServer {
using namespace NTransport;
namespace {
constexpr size_t MaxSessions = 8;
constexpr size_t PageRows = 10000;
constexpr size_t PageBytes = size_t{4} * 1024 * 1024;
constexpr auto IdleTimeout = std::chrono::seconds(60);
std::string Token()
{
    std::random_device random;
    std::string value;
    constexpr char Hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        value += Hex[random() & 15];
    }
    return value;
}
class TBoundedOutput final : public arrow::io::OutputStream
{
public:
    arrow::Status Close() override
    {
        Closed_ = true;
        return arrow::Status::OK();
    }
    bool closed() const override
    {
        return Closed_;
    }
    arrow::Result<int64_t> Tell() const override
    {
        return static_cast<int64_t>(Bytes.size());
    }
    arrow::Status Write(const void* data, int64_t size) override
    {
        if (Closed_ || size < 0 || static_cast<uint64_t>(size) > MaxResponseBytes - Bytes.size()) {
            return arrow::Status::CapacityError("Arrow response exceeds transport limit");
        }
        Bytes.append(static_cast<const char*>(data), static_cast<size_t>(size));
        return arrow::Status::OK();
    }
    std::string Bytes;

private:
    bool Closed_ = false;
};
TJson Snapshot(const ScanInfo& info)
{
    return info.selector_resolved.kind == SnapshotSelectorKind::Id
        ? TJson(std::to_string(info.selector_resolved.snapshot_id))
        : TJson(nullptr);
}
} // namespace

struct TScans::TImpl
{
    struct TSession
    {
        std::string Database, Collection, NextToken, ReplayToken;
        bool Arrow = false;
        std::shared_ptr<ScanCursor> Cursor;
        std::shared_ptr<arrow::RecordBatch> Pending;
        ScanInfo Info;
        TJson Schema;
        TResponse Replay;
        std::chrono::steady_clock::time_point Touched = std::chrono::steady_clock::now();
        ~TSession()
        {
            if (Cursor) {
                Cursor->Cancel();
            }
        }
    };
    std::mutex Mutex;
    std::condition_variable Wake;
    bool Stopping = false;
    std::vector<std::unique_ptr<TSession>> Sessions;
    std::thread Reaper;

    TImpl()
        : Reaper([this] {
            std::unique_lock lock(Mutex);
            while (!Stopping) {
                Wake.wait_for(lock, std::chrono::seconds(1));
                Expire();
            }
        })
    { }
    ~TImpl()
    {
        {
            std::lock_guard lock(Mutex);
            Stopping = true;
        }
        Wake.notify_all();
        Reaper.join();
    }
    void Expire()
    {
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(Sessions, [&](const auto& session) {
            return now - session->Touched >= IdleTimeout;
        });
    }
    TSession& Find(const std::string& database, const std::string& collection, const std::string& token)
    {
        Require(!token.empty() && token.size() == 32, "scan cursor not found or expired", "NOT_FOUND");
        for (auto& session : Sessions) {
            if (session->Database == database && session->Collection == collection &&
                (session->NextToken == token || session->ReplayToken == token))
            {
                session->Touched = std::chrono::steady_clock::now();
                return *session;
            }
        }
        throw TFailure({"NOT_FOUND", "scan cursor not found or expired"});
    }
    TResponse Page(TSession& session, const std::string& token)
    {
        if (session.ReplayToken == token) {
            return session.Replay;
        }
        auto sink = std::make_shared<TBoundedOutput>();
        std::shared_ptr<arrow::ipc::RecordBatchWriter> writer;
        if (session.Arrow) {
            const auto created =
                arrow::ipc::MakeStreamWriter(sink, std::const_pointer_cast<arrow::Schema>(session.Info.output_schema));
            Require(created.ok(), "cannot create Arrow stream", "RESOURCE_EXHAUSTED");
            writer = *created;
        }
        auto rows = TJson::array();
        size_t count = 0, bytes = 0;
        bool ended = false;
        while (count < PageRows && bytes < PageBytes) {
            if (!session.Pending) {
                session.Pending = Unwrap(session.Cursor->Next());
            }
            if (!session.Pending) {
                ended = true;
                break;
            }
            size_t take = 0;
            while (take < static_cast<size_t>(session.Pending->num_rows())) {
                const auto row = EncodeRows(*session.Pending->Slice(static_cast<int64_t>(take), 1)).at(0);
                const auto size = row.dump().size() + 1;
                if (count && (bytes + size > PageBytes || count == PageRows)) {
                    break;
                }
                bytes += size;
                ++count;
                ++take;
                if (!session.Arrow) {
                    rows.push_back(row);
                }
            }
            if (writer && take) {
                Require(writer->WriteRecordBatch(*session.Pending->Slice(0, static_cast<int64_t>(take))).ok(),
                    "Arrow response exceeds transport limit",
                    "RESOURCE_EXHAUSTED");
            }
            session.Pending = take == static_cast<size_t>(session.Pending->num_rows())
                ? nullptr
                : session.Pending->Slice(static_cast<int64_t>(take));
            if (session.Pending) {
                break;
            }
        }
        // Look ahead once so the final page advertises EOS, never an empty fake
        // success after a read failure. Failures invalidate the entire session.
        if (!session.Pending && !ended) {
            session.Pending = Unwrap(session.Cursor->Next());
        }
        const auto next = session.Pending ? Token() : std::string{};
        const auto snapshot = Snapshot(session.Info);
        TResponse response;
        if (writer) {
            Require(writer->Close().ok(), "Arrow response exceeds transport limit", "RESOURCE_EXHAUSTED");
            response = {200, std::move(sink->Bytes), "application/vnd.apache.arrow.stream"};
            response.Headers["X-GlamDB-Snapshot-Id"] = snapshot.is_null() ? "initial" : snapshot.get<std::string>();
            response.Headers["X-Content-SHA256"] = detail::Sha256(response.Body);
            if (!next.empty()) {
                response.Headers["X-GlamDB-Next-Cursor"] = next;
            }
        } else {
            response = {200,
                TJson{{"schema", session.Schema},
                    {"snapshot_id", snapshot},
                    {"rows", std::move(rows)},
                    {"identity", EncodeIdentity(session.Info.identity)},
                    {"next_cursor", next.empty() ? TJson(nullptr) : TJson(next)},
                    {"profile", {{"rows_returned", std::to_string(count)}}}}
                    .dump()};
        }
        Require(response.Body.size() <= MaxResponseBytes,
            "scan response exceeds transport limit",
            "RESOURCE_EXHAUSTED");
        session.ReplayToken = token;
        session.NextToken = next;
        session.Replay = response;
        session.Touched = std::chrono::steady_clock::now();
        if (next.empty()) {
            session.Cursor->Cancel();
            session.Cursor.reset();
        }
        return response;
    }
};
TScans::TScans()
    : Impl_(std::make_unique<TImpl>())
{ }
TScans::~TScans() = default;
TResponse TScans::Open(const std::shared_ptr<Engine>& engine,
    const std::string& database,
    const std::string& collection,
    const TJson& body,
    bool arrow,
    bool openOnly)
{
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Expire();
    TImpl::TSession* session = nullptr;
    std::string token;
    if (body.contains("cursor")) {
        Require(!openOnly, "open does not accept a cursor");
        CheckKeys(body, {"cursor"});
        token = body.at("cursor").get<std::string>();
        session = &Impl_->Find(database, collection, token);
        Require(session->Arrow == arrow, "cursor representation cannot change");
    } else {
        if (Impl_->Sessions.size() >= MaxSessions) {
            // Completed replay caches retain no engine ownership and can be
            // evicted to admit another scan. Live cursors are never evicted.
            const auto done = std::find_if(Impl_->Sessions.begin(), Impl_->Sessions.end(), [](const auto& item) {
                return !item->Cursor;
            });
            if (done != Impl_->Sessions.end()) {
                Impl_->Sessions.erase(done);
            }
        }
        Require(Impl_->Sessions.size() < MaxSessions,
            "too many scan sessions; cancel unused cursors",
            "RESOURCE_EXHAUSTED");
        const auto collectionInfo = Unwrap(engine->GetCollection(collection));
        auto created = std::make_unique<TImpl::TSession>();
        created->Cursor = Unwrap(engine->Scan(ParseScan(collection, collectionInfo.schema, body)));
        created->Info = created->Cursor->Info();
        created->Database = database;
        created->Collection = collection;
        created->Arrow = arrow;
        created->NextToken = Token();
        created->Schema = EncodeSchema(*created->Info.output_schema);
        for (auto& field : created->Schema["fields"]) {
            for (size_t i = 0; i < collectionInfo.schema.fields.size(); ++i) {
                if (field["name"] == collectionInfo.schema.fields[i].name) {
                    field["field_id"] = i + 1;
                }
            }
        }
        session = created.get();
        token = session->NextToken;
        Impl_->Sessions.push_back(std::move(created));
    }
    if (openOnly) {
        return {200,
            TJson{{"cursor", token},
                {"snapshot_id", Snapshot(session->Info)},
                {"schema", session->Schema},
                {"identity", EncodeIdentity(session->Info.identity)}}
                .dump()};
    }
    try {
        return Impl_->Page(*session, token);
    } catch (...) {
        std::erase_if(Impl_->Sessions, [&](const auto& item) {
            return item.get() == session;
        });
        throw;
    }
}
TResponse TScans::Cancel(const std::string& database, const std::string& collection, const TJson& body)
{
    CheckKeys(body, {"cursor"});
    const auto token = body.at("cursor").get<std::string>();
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Expire();
    auto* session = &Impl_->Find(database, collection, token);
    std::erase_if(Impl_->Sessions, [&](const auto& item) {
        return item.get() == session;
    });
    return {200, R"({"cancelled":true})"};
}
void TScans::Clear(const std::string& database)
{
    std::lock_guard lock(Impl_->Mutex);
    std::erase_if(Impl_->Sessions, [&](const auto& session) {
        return session->Database == database;
    });
}
void TScans::Clear()
{
    std::lock_guard lock(Impl_->Mutex);
    Impl_->Sessions.clear();
}
} // namespace glamdb::NServer
