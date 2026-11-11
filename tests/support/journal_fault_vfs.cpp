#include "tests/support/journal_fault_vfs.h"

#include "core/api/binary.h"

#include <mutex>
#include <unordered_map>

namespace glamdb::test {
namespace {
std::mutex registry_mutex;
std::unordered_map<sqlite3_vfs*, JournalFaultVfs*> registry;
std::atomic<int> next_id{0};
} // namespace

JournalFaultVfs::JournalFaultVfs()
    : parent_(sqlite3_vfs_find(nullptr))
    , name_("glamdb-journal-fault-" + std::to_string(++next_id))
{
    detail::Require(parent_ != nullptr, "INTERNAL", "default VFS is missing");
    // Retain the original pAppData: the parent's other callbacks use it.
    vfs_ = *parent_;
    vfs_.pNext = nullptr;
    vfs_.zName = name_.c_str();
    vfs_.xDelete = Delete;
    {
        std::lock_guard lock(registry_mutex);
        registry[&vfs_] = this;
    }
    detail::Require(sqlite3_vfs_register(&vfs_, 0) == SQLITE_OK, "INTERNAL", "cannot register fault VFS");
}

JournalFaultVfs::~JournalFaultVfs()
{
    sqlite3_vfs_unregister(&vfs_);
    std::lock_guard lock(registry_mutex);
    registry.erase(&vfs_);
}

void JournalFaultVfs::Arm(int journal_delete_number, bool after_unlink)
{
    after_unlink_ = after_unlink;
    remaining_ = journal_delete_number;
}
void JournalFaultVfs::Disarm()
{
    remaining_ = 0;
}

void JournalFaultVfs::PauseBeforeDelete(int journal_delete_number)
{
    std::lock_guard lock(pause_mutex_);
    paused_ = false;
    released_ = false;
    pause_remaining_ = journal_delete_number;
}
bool JournalFaultVfs::WaitUntilPaused(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(pause_mutex_);
    return pause_changed_.wait_for(lock, timeout, [&] {
        return paused_;
    });
}
void JournalFaultVfs::Release()
{
    std::lock_guard lock(pause_mutex_);
    released_ = true;
    pause_changed_.notify_all();
}

int JournalFaultVfs::Delete(sqlite3_vfs* vfs, const char* path, int sync_directory)
{
    JournalFaultVfs* owner = nullptr;
    {
        std::lock_guard lock(registry_mutex);
        const auto found = registry.find(vfs);
        if (found == registry.end()) {
            return SQLITE_IOERR_DELETE;
        }
        owner = found->second;
    }
    const bool journal = path != nullptr && std::string_view(path).ends_with("-journal");
    if (journal && owner->pause_remaining_.load() > 0 && owner->pause_remaining_.fetch_sub(1) == 1) {
        std::unique_lock lock(owner->pause_mutex_);
        owner->paused_ = true;
        owner->pause_changed_.notify_all();
        owner->pause_changed_.wait(lock, [&] {
            return owner->released_;
        });
    }
    const bool fail = journal && owner->remaining_.load() > 0 && owner->remaining_.fetch_sub(1) == 1;
    if (fail && !owner->after_unlink_) {
        return SQLITE_IOERR_DELETE;
    }
    const auto result = owner->parent_->xDelete(owner->parent_, path, sync_directory);
    return fail && result == SQLITE_OK ? SQLITE_IOERR_DIR_FSYNC : result;
}

} // namespace glamdb::test
