#include "apps/server/databases.h"
#include "core/api/binary.h"
#include "core/catalog/sqlite.h"
#include "tests/support/journal_fault_vfs.h"
#include "tests/support/temp_dir.h"

#include <gtest/gtest.h>

#include <future>
#include <thread>

namespace glamdb::NServer {
using namespace NTransport;
using namespace std::chrono_literals;

TEST(Databases, CloseDrainsOnlyItsOwnRequests)
{
    test::TempDir temporary;
    TDatabases databases(temporary.path / "server");
    const auto a = databases.Create({{"name", "a"}}).at("database_id").get<std::string>();
    const auto b = databases.Create({{"name", "b"}}).at("database_id").get<std::string>();
    std::promise<void> entered, release;
    auto released = release.get_future();
    auto reader = std::async(std::launch::async, [&] {
        return databases.Use(a, [&](const auto&) {
            entered.set_value();
            released.wait();
            return TResponse{200, "done"};
        });
    });
    entered.get_future().wait();
    auto closer = std::async(std::launch::async, [&] {
        return databases.Change(a, "close");
    });
    bool rejected = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!rejected && std::chrono::steady_clock::now() < deadline) {
        try {
            databases.Use(a, [](const auto&) {
                return TResponse{200, ""};
            });
        } catch (const TFailure&) {
            rejected = true;
        }
        std::this_thread::yield();
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(closer.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(databases
                  .Use(b,
                      [](const auto&) {
                          return TResponse{200, ""};
                      })
                  .Status,
        200);
    release.set_value();
    EXPECT_EQ(reader.get().Status, 200);
    EXPECT_EQ(closer.get().at("state"), "closed");
    EXPECT_EQ(databases.Change(a, "open").at("state"), "open");
    databases.Shutdown();
}

TEST(Databases, ConcurrentNameReservationHasOneIdentity)
{
    test::TempDir temporary;
    TDatabases databases(temporary.path / "server");
    auto one = std::async(std::launch::async, [&] {
        return databases.Create({{"name", "same"}});
    });
    auto two = std::async(std::launch::async, [&] {
        return databases.Create({{"name", "same"}});
    });
    EXPECT_EQ(one.get(), two.get());
    EXPECT_EQ(databases.List().at("items").size(), 1U);
    EXPECT_THROW(TDatabases other(temporary.path / "server"), TFailure);
    databases.Shutdown();
}

TEST(Databases, FailedCloseRetainsOwnershipUntilRetry)
{
    test::TempDir temporary;
    TDatabases databases(temporary.path / "server");
    const auto id = databases.Create({{"name", "a"}}).at("database_id").get<std::string>();
    databases.Use(id, [&](const auto& engine) {
        const auto created =
            engine->CreateCollection({test::collection_id, "c", SchemaSpec{{{"key", FieldType::String, false}}}});
        EXPECT_EQ(created.disposition, CommitDisposition::Committed);
        return TResponse{200, ""};
    });
    std::filesystem::path metadata;
    const auto root = temporary.path / "server/databases" / id;
    for (const auto& file : std::filesystem::recursive_directory_iterator(root / "warehouse")) {
        if (file.path().string().ends_with(".metadata.json")) {
            metadata = file.path();
            break;
        }
    }
    ASSERT_FALSE(metadata.empty());
    const auto saved = temporary.path / "saved-metadata";
    std::filesystem::rename(metadata, saved);
    EXPECT_THROW(databases.Change(id, "reconcile"), TFailure);
    EXPECT_THROW(databases.Change(id, "close"), TFailure);
    EXPECT_EQ(databases.Get(id).at("state"), "closing");
    EXPECT_THROW(databases.Change(id, "open"), TFailure);
    EXPECT_THROW(databases.Change(id, "reconcile"), TFailure);
    const auto competing = OpenExisting({(temporary.path / "server/databases" / id).string(), id});
    ASSERT_FALSE(competing);
    EXPECT_EQ(competing.error().code, "STATE_IN_USE");
    std::filesystem::rename(saved, metadata);
    EXPECT_EQ(databases.Change(id, "close").at("state"), "closed");
    EXPECT_EQ(databases.Change(id, "open").at("state"), "open");
    databases.Shutdown();
}

TEST(Databases, RegistryCommitFailuresNeverRecreateStores)
{
    for (const int commit : {1, 2}) {
        for (const bool after : {false, true}) {
            SCOPED_TRACE(std::to_string(commit) + (after ? " after unlink" : " before unlink"));
            test::TempDir temporary;
            test::JournalFaultVfs vfs;
            const auto directory = temporary.path / "server";
            const auto root = directory / "databases" / test::store_id;
            {
                TDatabases databases(directory, vfs.Name());
                vfs.Arm(commit, after);
                EXPECT_THROW(databases.Create({{"name", "a"}, {"database_id", test::store_id}}), detail::Failure);
                // No store I/O follows an uncertain reservation; a failed final
                // registry update retains the complete store for restart.
                EXPECT_EQ(std::filesystem::exists(root), commit == 2);
                EXPECT_THROW(databases.Create({{"name", "other"}}), TFailure);
                vfs.Disarm();
                databases.Shutdown();
            }
            TDatabases reopened(directory);
            const auto value = reopened.Create({{"name", "a"}, {"database_id", test::store_id}});
            EXPECT_EQ(value.at("state"), "open");
            EXPECT_EQ(reopened.List().at("items").size(), 1U);
            reopened.Shutdown();
        }
    }
}

TEST(Databases, ReservationCrashAndIncompleteStoreRemainExplicit)
{
    test::TempDir temporary;
    const auto directory = temporary.path / "server";
    {
        TDatabases databases(directory);
        databases.Shutdown();
    }
    {
        detail::Database registry;
        registry.Open(directory / "registry.sqlite", SQLITE_OPEN_READWRITE, "");
        registry.Execute("INSERT INTO databases VALUES(?,?,0,1)", {test::store_id, "reserved"});
    }
    {
        TDatabases databases(directory);
        EXPECT_EQ(databases.Get(test::store_id).at("state"), "error");
        EXPECT_THROW(databases.Change(test::store_id, "open"), TFailure);
        EXPECT_EQ(databases.Create({{"name", "reserved"}}).at("state"), "open");
        databases.Shutdown();
    }
    const auto other = directory / "databases" / test::table_uuid;
    std::filesystem::create_directory(other);
    {
        detail::Database registry;
        registry.Open(directory / "registry.sqlite", SQLITE_OPEN_READWRITE, "");
        registry.Execute("INSERT INTO databases VALUES(?,?,0,1)", {test::table_uuid, "incomplete"});
    }
    {
        TDatabases databases(directory);
        EXPECT_EQ(databases.Get(test::table_uuid).at("state"), "error");
        EXPECT_THROW(databases.Create({{"name", "incomplete"}}), TFailure);
        EXPECT_TRUE(std::filesystem::is_empty(other));
        EXPECT_EQ(databases.Get(test::store_id).at("state"), "open");
        databases.Shutdown();
    }
    std::filesystem::remove(directory / "registry.sqlite");
    EXPECT_THROW(TDatabases databases(directory), TFailure);
}
} // namespace glamdb::NServer
