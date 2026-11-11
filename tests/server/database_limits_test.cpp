#include "apps/server/databases.h"
#include "core/catalog/sqlite.h"
#include "tests/support/journal_fault_vfs.h"
#include "tests/support/temp_dir.h"

#include <gtest/gtest.h>

namespace glamdb::NServer {
using namespace NTransport;

TEST(DatabaseLimits, UncertainOpenIntentRetainsAuthoritativeConfiguration)
{
    for (const bool after : {false, true}) {
        SCOPED_TRACE(after ? "after journal unlink" : "before journal unlink");
        test::TempDir temporary;
        test::JournalFaultVfs vfs;
        const auto directory = temporary.path / "server";
        std::string id;
        {
            TDatabases databases(directory, vfs.Name());
            id = databases.Create({{"name", "limited"}, {"limits", {{"max_collections", 1}}}})
                     .at("database_id")
                     .get<std::string>();
            databases.Change(id, "close");
            vfs.Arm(1, after);
            EXPECT_THROW(databases.Change(id, "open", {{"limits", {{"max_collections", 2}}}}), detail::Failure);
            EXPECT_THROW(databases.Change(id, "open"), TFailure);
            EXPECT_THROW(databases.Create({{"name", "other"}}), TFailure);
            vfs.Disarm();
            databases.Shutdown();
        }
        TDatabases reopened(directory);
        const auto info = reopened.Get(id);
        EXPECT_EQ(info.at("limits").at("max_collections"), after ? 2 : 1);
        EXPECT_EQ(info.at("state"), after ? "open" : "closed");
        EXPECT_EQ(reopened.Create({{"name", "limited"}}).at("limits"), info.at("limits"));
        EXPECT_EQ(reopened.List().at("items").size(), 1U);
        reopened.Shutdown();
    }
}

TEST(DatabaseLimits, MigrationCommitFailureRetainsVersionOneIdentityAndDefaults)
{
    for (const bool after : {false, true}) {
        SCOPED_TRACE(after ? "after journal unlink" : "before journal unlink");
        test::TempDir temporary;
        test::JournalFaultVfs vfs;
        const auto directory = temporary.path / "server";
        std::filesystem::create_directories(directory / "databases");
        {
            detail::Database registry;
            registry.Open(directory / "registry.sqlite", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "");
            registry.Execute("CREATE TABLE databases(id TEXT PRIMARY KEY NOT NULL, name TEXT UNIQUE NOT NULL, "
                             "initialized INTEGER NOT NULL CHECK(initialized IN (0,1)), auto_open INTEGER NOT NULL "
                             "CHECK(auto_open IN (0,1))) STRICT");
            registry.Execute("INSERT INTO databases VALUES(?,?,0,0)", {test::store_id, "reserved"});
            registry.Execute("PRAGMA user_version=1");
        }
        vfs.Arm(1, after);
        EXPECT_THROW(TDatabases databases(directory, vfs.Name()), detail::Failure);
        vfs.Disarm();
        TDatabases reopened(directory);
        const auto info = reopened.Get(test::store_id);
        EXPECT_EQ(info.at("name"), "reserved");
        EXPECT_EQ(info.at("state"), "closed");
        EXPECT_EQ(info.at("limits").at("max_operations"), 4096);
        EXPECT_EQ(info.at("limits").at("max_collections"), 8);
        EXPECT_EQ(reopened.List().at("items").size(), 1U);
        reopened.Shutdown();
    }
}
} // namespace glamdb::NServer
