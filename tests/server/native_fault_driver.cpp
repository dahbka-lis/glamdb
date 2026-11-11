#include "apps/server/databases.h"
#include "apps/server/server.h"
#include "tests/support/journal_fault_vfs.h"

#include <iostream>
#include <sstream>
#include <thread>

// Test-only control channel on stdin; HTTP and every store operation use the
// production managed server and storage backend. No fault route is installed.
int main(int argc, char** argv)
try {
    if (argc != 2) {
        return 2;
    }
    glamdb::test::JournalFaultVfs core, registry;
    auto* coreVfs = sqlite3_vfs_find(core.Name().c_str());
    if (sqlite3_vfs_register(coreVfs, 1) != SQLITE_OK) {
        return 3;
    }
    auto databases = std::make_shared<glamdb::NServer::TDatabases>(argv[1], registry.Name());
    glamdb::NServer::TRoutes routes(databases, {}, "fault-test-token");
    std::thread controls([&] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::istringstream command(line);
            std::string target;
            int commit = 0, after = 0;
            command >> target >> commit >> after;
            if (target == "pause") {
                core.PauseBeforeDelete(commit);
                std::cout << "armed\n" << std::flush;
                continue;
            }
            if (target == "wait") {
                std::cout << (core.WaitUntilPaused(std::chrono::seconds(10)) ? "paused\n" : "timeout\n") << std::flush;
                continue;
            }
            if (target == "release") {
                core.Release();
                std::cout << "armed\n" << std::flush;
                continue;
            }
            auto& vfs = target == "registry" ? registry : core;
            if (commit == 0) {
                vfs.Disarm();
            } else {
                vfs.Arm(commit, after != 0);
            }
            std::cout << "armed\n" << std::flush;
        }
        core.Release();
    });
    glamdb::NServer::RunServer(routes, "127.0.0.1", 0, std::chrono::seconds(10));
    controls.join();
    databases->Shutdown();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
