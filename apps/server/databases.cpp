#include "apps/server/databases.h"

#include "core/api/canonical.h"
#include "core/catalog/sqlite.h"
#include "core/engine/store_lease.h"

#include <atomic>
#include <fcntl.h>
#include <mutex>
#include <shared_mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace glamdb::NServer {
using namespace NTransport;
namespace {
struct TDirectoryLock
{
    int Descriptor = -1;
    ~TDirectoryLock()
    {
        if (Descriptor >= 0) {
            close(Descriptor);
        }
    }
};
struct TEntry
{
    std::string ID, Name;
    std::mutex Lifecycle;
    mutable std::shared_mutex Gate;
    std::atomic<bool> Accepting{false};
    std::shared_ptr<Engine> Instance;
    bool Initialized = false;
    bool AutoOpen = true;
    bool Closing = false;
    std::string State = "closed";
    std::optional<ErrorDetail> Error;
};
TJson Describe(const TEntry& entry)
{
    TJson value{{"database_id", entry.ID}, {"name", entry.Name}, {"state", entry.State}};
    if (entry.Error) {
        value["error"] = EncodeError(*entry.Error).at("error");
    }
    if (entry.Instance && entry.State == "open") {
        const auto& info = entry.Instance->GetStoreInfo();
        value["store"] = {{"store_id", info.store_id},
            {"profile_id", info.profile_id},
            {"format_version", info.format_version}};
    }
    return value;
}
void ValidateName(const std::string& name)
{
    Require(!name.empty() && name.size() <= 128 && detail::ValidUtf8(name) &&
            name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos,
        "database name must be 1..128 UTF-8 bytes without path separators");
}
} // namespace

struct TDatabases::TImpl
{
    TDirectoryLock DirectoryLock;
    std::filesystem::path Root;
    detail::Database Registry;
    mutable std::mutex Mutex;
    std::map<std::string, std::shared_ptr<TEntry>> Entries;
    bool Healthy = true;

    void Write(std::string_view sql, std::initializer_list<detail::SqlValue> values = {})
    {
        // Caller holds Mutex. A failed SQLite write may have committed: stop
        // registry mutations until restart rather than act on uncertain intent.
        Require(Healthy, "registry needs restart after an uncertain write", "RECOVERY_REQUIRED");
        try {
            Registry.Execute(sql, values);
        } catch (...) {
            Healthy = false;
            throw;
        }
    }
    std::shared_ptr<TEntry> Find(const std::string& id) const
    {
        Require(detail::ValidUuid(id), "invalid database ID");
        std::lock_guard lock(Mutex);
        const auto it = Entries.find(id);
        Require(it != Entries.end(), "database not found", "NOT_FOUND");
        return it->second;
    }
    std::vector<std::shared_ptr<TEntry>> Snapshot() const
    {
        std::lock_guard lock(Mutex);
        std::vector<std::shared_ptr<TEntry>> result;
        for (const auto& [id, entry] : Entries) {
            (void)id;
            result.push_back(entry);
        }
        return result;
    }
    void Open(TEntry& entry, bool create)
    {
        {
            std::lock_guard lock(Mutex);
            Require(Healthy, "registry needs restart", "RECOVERY_REQUIRED");
        }
        Require(!entry.Closing, "retry close before reopening this database", "DATABASE_BUSY");
        if (entry.Instance) {
            Require(entry.State == "open", "database needs reconciliation or close", "RECOVERY_REQUIRED");
            return;
        }
        const auto path = Root / "databases" / entry.ID;
        try {
            auto engine =
                create && !entry.Initialized && !std::filesystem::exists(std::filesystem::symlink_status(path))
                ? Unwrap(CreateStore({entry.ID, path.string()}))
                : Unwrap(OpenExisting({path.string(), entry.ID}));
            entry.Instance = std::move(engine);
            {
                std::lock_guard lock(Mutex);
                Write("UPDATE databases SET initialized=1, auto_open=1 WHERE id=?", {entry.ID});
            }
            entry.Initialized = true;
            entry.AutoOpen = true;
            entry.State = "open";
            entry.Error.reset();
            entry.Accepting = true;
        } catch (const TFailure& error) {
            entry.State = "error";
            entry.Error = error.Detail;
            throw;
        } catch (const detail::Failure& error) {
            entry.State = "error";
            entry.Error = error.detail;
            throw;
        }
    }
};

TDatabases::TDatabases(const std::filesystem::path& directory, const std::string& registryVfs)
    : Impl_(std::make_unique<TImpl>())
{
    auto& self = *Impl_;
    Require(directory.is_absolute() && directory.has_filename() && directory == directory.lexically_normal() &&
            directory.string().find('\0') == std::string::npos &&
            std::filesystem::canonical(directory.parent_path()) == directory.parent_path(),
        "data directory must be an absolute path with an existing canonical parent");
    const bool exists = std::filesystem::exists(std::filesystem::symlink_status(directory));
    if (!exists) {
        Require(mkdir(directory.c_str(), 0700) == 0, "cannot create server data directory", "IO_ERROR");
    }
    self.DirectoryLock.Descriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    Require(self.DirectoryLock.Descriptor >= 0, "cannot open server data directory", "IO_ERROR");
    Require(flock(self.DirectoryLock.Descriptor, LOCK_EX | LOCK_NB) == 0,
        "server data directory is already owned",
        "STATE_IN_USE");
    self.Root = directory;
    const auto registry = directory / "registry.sqlite";
    const bool fresh = !std::filesystem::exists(std::filesystem::symlink_status(registry));
    Require(!fresh || std::filesystem::is_empty(directory),
        "registry missing from nonempty data directory; refusing to replace it",
        "CORRUPTION");
    self.Registry.Open(registry, SQLITE_OPEN_READWRITE | (fresh ? SQLITE_OPEN_CREATE : 0), registryVfs);
    Require(self.Registry.Text("PRAGMA journal_mode=DELETE") == "delete",
        "registry requires DELETE journal",
        "IO_ERROR");
    self.Registry.Execute("PRAGMA synchronous=EXTRA");
    Require(self.Registry.Integer("PRAGMA synchronous") == 3, "registry requires EXTRA synchronous", "IO_ERROR");
    if (fresh) {
        self.Registry.Execute("CREATE TABLE databases(id TEXT PRIMARY KEY NOT NULL, name TEXT UNIQUE NOT NULL, "
                              "initialized INTEGER NOT NULL CHECK(initialized IN (0,1)), auto_open INTEGER NOT NULL "
                              "CHECK(auto_open IN (0,1))) STRICT");
        self.Registry.Execute("PRAGMA user_version=1");
        Require(mkdir((directory / "databases").c_str(), 0700) == 0, "cannot create database parent", "IO_ERROR");
        Require(fsync(self.DirectoryLock.Descriptor) == 0, "cannot sync server data directory", "IO_ERROR");
        TDirectoryLock parent;
        parent.Descriptor = open(directory.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        Require(parent.Descriptor >= 0 && fsync(parent.Descriptor) == 0, "cannot sync server parent", "IO_ERROR");
    }
    Require(self.Registry.Integer("PRAGMA user_version") == 1, "unsupported server registry version", "CORRUPTION");
    Require(std::filesystem::is_directory(std::filesystem::symlink_status(directory / "databases")),
        "database parent is absent or a symlink",
        "CORRUPTION");
    for (const auto& row : self.Registry.Query("SELECT id,name,initialized,auto_open FROM databases ORDER BY name")) {
        auto entry = std::make_shared<TEntry>();
        entry->ID = detail::Required(row, 0);
        entry->Name = detail::Required(row, 1);
        Require(detail::ValidUuid(entry->ID), "invalid registered database identity", "CORRUPTION");
        ValidateName(entry->Name);
        const auto initialized = detail::Required(row, 2), autoOpen = detail::Required(row, 3);
        Require((initialized == "0" || initialized == "1") && (autoOpen == "0" || autoOpen == "1"),
            "invalid database lifecycle flags",
            "CORRUPTION");
        entry->Initialized = initialized == "1";
        entry->AutoOpen = autoOpen == "1";
        Require(self.Entries.size() < 64 && self.Entries.emplace(entry->ID, entry).second,
            "invalid or oversized registry",
            "CORRUPTION");
    }
    for (const auto& entry : self.Snapshot()) {
        if (entry->AutoOpen) {
            try {
                self.Open(*entry, false);
            } catch (const TFailure&) {
                // One unavailable store must not hide the database administration API.
                continue;
            }
        }
    }
}
TDatabases::~TDatabases() = default;

TJson TDatabases::List() const
{
    auto items = TJson::array();
    for (const auto& entry : Impl_->Snapshot()) {
        std::shared_lock lock(entry->Gate);
        items.push_back(Describe(*entry));
    }
    return {{"items", items}, {"next_cursor", nullptr}};
}
TJson TDatabases::Get(const std::string& id) const
{
    const auto entry = Impl_->Find(id);
    std::shared_lock lock(entry->Gate);
    return Describe(*entry);
}
TJson TDatabases::Create(const TJson& body)
{
    CheckKeys(body, {"name", "database_id"});
    const auto name = body.at("name").get<std::string>();
    ValidateName(name);
    const auto supplied = body.value("database_id", "");
    Require(!body.contains("database_id") || detail::ValidUuid(supplied), "invalid database ID");
    std::shared_ptr<TEntry> entry;
    {
        std::lock_guard lock(Impl_->Mutex);
        Require(Impl_->Healthy, "registry needs restart", "RECOVERY_REQUIRED");
        for (const auto& [id, candidate] : Impl_->Entries) {
            if (candidate->Name == name || id == supplied) {
                Require(candidate->Name == name && (supplied.empty() || id == supplied),
                    "database name or ID already exists",
                    "ALREADY_EXISTS");
                entry = candidate;
                break;
            }
        }
        if (!entry) {
            Require(Impl_->Entries.size() < 64, "database limit reached", "RESOURCE_EXHAUSTED");
            entry = std::make_shared<TEntry>();
            entry->Name = name;
            entry->ID = supplied.empty() ? detail::NewUuid() : supplied;
            Impl_->Write("INSERT INTO databases VALUES(?,?,0,1)", {entry->ID, entry->Name});
            Impl_->Entries.emplace(entry->ID, entry);
        }
    }
    std::lock_guard lifecycle(entry->Lifecycle);
    const bool admitted = entry->Accepting.exchange(false);
    std::unique_lock lock(entry->Gate);
    try {
        Impl_->Open(*entry, true);
        entry->Accepting = true;
    } catch (...) {
        entry->Accepting = admitted;
        throw;
    }
    return Describe(*entry);
}
TJson TDatabases::Change(const std::string& id, std::string_view action)
{
    Require(action == "open" || action == "close" || action == "reconcile", "unknown database action");
    const auto entry = Impl_->Find(id);
    std::lock_guard lifecycle(entry->Lifecycle);
    {
        std::lock_guard registryLock(Impl_->Mutex);
        Require(Impl_->Healthy, "registry needs restart", "RECOVERY_REQUIRED");
    }
    const bool admitted = entry->Accepting.exchange(false);
    std::unique_lock lock(entry->Gate); // admitted calls drain; new calls fail before waiting
    if (action == "open") {
        try {
            Impl_->Open(*entry, false);
            entry->Accepting = entry->State == "open";
        } catch (...) {
            entry->Accepting = admitted;
            throw;
        }
    } else if (action == "close") {
        {
            std::lock_guard registryLock(Impl_->Mutex);
            try {
                Impl_->Write("UPDATE databases SET auto_open=0 WHERE id=?", {entry->ID});
            } catch (const detail::Failure& error) {
                entry->State = "error";
                entry->Error = error.detail;
                entry->Accepting = admitted;
                throw;
            }
        }
        entry->AutoOpen = false;
        entry->Closing = true;
        entry->State = "closing";
        if (entry->Instance) {
            const auto closed = entry->Instance->Shutdown(std::chrono::seconds(60));
            if (!closed) {
                entry->Error = closed.error();
                throw TFailure(closed.error());
            }
            entry->Instance.reset();
        }
        entry->Closing = false;
        entry->State = "closed";
        entry->Error.reset();
    } else {
        Require(action == "reconcile", "unknown database action");
        Require(entry->Instance && !entry->Closing, "open database or retry close first", "DATABASE_CLOSED");
        const auto reconciled = entry->Instance->Reconcile();
        if (!reconciled) {
            entry->State = "error";
            entry->Error = reconciled.error();
            // Core readiness blocks unsafe work, but durable outcome lookup
            // must remain available while recovery is unresolved.
            entry->Accepting = true;
            throw TFailure(reconciled.error());
        }
        entry->State = "open";
        entry->Error.reset();
        entry->Accepting = true;
    }
    return Describe(*entry);
}
TResponse TDatabases::Use(const std::string& id, const std::function<TResponse(std::shared_ptr<Engine>)>& call) const
{
    const auto entry = Impl_->Find(id);
    Require(entry->Accepting, "database is closed or busy", "DATABASE_CLOSED");
    std::shared_lock lock(entry->Gate, std::try_to_lock);
    Require(lock.owns_lock() && entry->Accepting, "database is closed or busy", "DATABASE_CLOSED");
    return call(entry->Instance);
}
void TDatabases::Shutdown()
{
    std::optional<ErrorDetail> first;
    for (const auto& entry : Impl_->Snapshot()) {
        std::lock_guard lifecycle(entry->Lifecycle);
        entry->Accepting = false;
        std::unique_lock lock(entry->Gate);
        if (entry->Instance) {
            const auto closed = entry->Instance->Shutdown(std::chrono::seconds(60));
            if (closed) {
                entry->Instance.reset();
            } else if (!first) {
                first = closed.error();
            }
        }
    }
    if (first) {
        throw TFailure(*first);
    }
}
} // namespace glamdb::NServer
