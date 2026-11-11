#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sqlite3.h>
#include <string>

namespace glamdb::test {

// Real SQLite VFS delegation; fail before or after an actual rollback-journal
// unlink. This models a lost COMMIT result, not loss of the OS page cache.
class JournalFaultVfs
{
public:
    JournalFaultVfs();
    ~JournalFaultVfs();
    JournalFaultVfs(const JournalFaultVfs&) = delete;
    JournalFaultVfs& operator=(const JournalFaultVfs&) = delete;
    void Arm(int journal_delete_number, bool after_unlink);
    void Disarm();
    void PauseBeforeDelete(int journal_delete_number);
    bool WaitUntilPaused(std::chrono::milliseconds timeout);
    void Release();
    const std::string& Name() const
    {
        return name_;
    }

private:
    static int Delete(sqlite3_vfs* vfs, const char* path, int sync_directory);
    sqlite3_vfs* parent_ = nullptr;
    sqlite3_vfs vfs_{};
    std::string name_;
    std::atomic<int> remaining_{0};
    bool after_unlink_ = false;
    std::atomic<int> pause_remaining_{0};
    std::mutex pause_mutex_;
    std::condition_variable pause_changed_;
    bool paused_ = false;
    bool released_ = true;
};

} // namespace glamdb::test
