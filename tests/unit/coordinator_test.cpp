#include "storage/engine.h"

#include "core/engine/engine_factory.h"
#include "tests/support/arrow_batch.h"
#include "tests/support/checked.h"
#include "tests/support/fake_storage.h"
#include "tests/support/temp_dir.h"

#include <future>
#include <gtest/gtest.h>

namespace glamdb {
namespace {

class CoordinatorTest : public testing::Test {
protected:
    test::TempDir temporary;
    std::filesystem::path root = temporary.path / "store";
    std::shared_ptr<test::FakeStorage> backend = std::make_shared<test::FakeStorage>(root);
    std::shared_ptr<Engine> engine;
    CreateStoreRequest create{test::store_id, root.string()};

    void SetUp() override {
        auto opened = detail::EngineFactory::Create(create, backend);
        ASSERT_TRUE(opened);
        engine = *opened;
        ASSERT_EQ(engine->CreateCollection({test::collection_id, "events", SchemaSpec{{{"n", FieldType::Int64, true}}}})
                      .disposition,
                  CommitDisposition::Committed);
    }
    AppendRequest Request(std::string id = test::operation_id) const {
        return {test::collection_id, std::move(id), {}, {test::Batch()}};
    }
    Result<std::shared_ptr<Engine>> Reopen() {
        engine.reset();
        return detail::EngineFactory::Open({root.string(), test::store_id}, backend);
    }
};

TEST_F(CoordinatorTest, AppendThenNewerAppendThenRetryReturnsExactOldReceipt) {
    const auto a = engine->AppendDurable(Request());
    ASSERT_EQ(a.disposition, CommitDisposition::Committed);
    const auto b = engine->AppendDurable(Request("00000000-0000-0000-0000-000000000006"));
    ASSERT_EQ(b.disposition, CommitDisposition::Committed);
    auto retry = Request();
    retry.expected_snapshot = {SnapshotPreconditionKind::NoSnapshot, 0};
    EXPECT_EQ(engine->AppendDurable(retry), a);
    EXPECT_EQ(backend->prepares, 2);
    EXPECT_EQ(engine->GetCollection(test::collection_id)->current_snapshot_id, test::Present(b.result).snapshot_id);
    auto opened = Reopen();
    ASSERT_TRUE(opened);
    engine = *opened;
    EXPECT_EQ(engine->AppendDurable(retry), a);
    EXPECT_EQ(test::Present(engine->FindAppendOutcome(test::collection_id, test::operation_id).value()), a);
    EXPECT_EQ(backend->prepares, 2);
}

TEST_F(CoordinatorTest, RegisteredFailureIsPermanentAndDifferentPayloadConflicts) {
    backend->fail_prepare = true;
    const auto failed = engine->AppendDurable(Request());
    ASSERT_EQ(failed.disposition, CommitDisposition::DefinitelyNotCommitted);
    ASSERT_TRUE(failed.error);
    EXPECT_EQ(test::Present(failed.error).retry_hint, RetryHint::NewOperationId);
    backend->fail_prepare = false;
    EXPECT_EQ(engine->AppendDurable(Request()), failed);
    auto conflict = Request();
    conflict.batches = {test::Batch({9})};
    EXPECT_EQ(test::Present(engine->AppendDurable(conflict).error).code, "OPERATION_CONFLICT");
    EXPECT_FALSE(engine->GetCollection(test::collection_id)->current_snapshot_id);
    auto opened = Reopen();
    ASSERT_TRUE(opened);
    engine = *opened;
    EXPECT_EQ(engine->AppendDurable(Request()), failed);
    EXPECT_EQ(backend->prepares, 1);
}

TEST_F(CoordinatorTest, EmptyInputDoesNotBindAndPreconditionFailureDoes) {
    auto empty = Request();
    empty.batches.clear();
    EXPECT_EQ(engine->AppendDurable(empty).disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_FALSE(engine->FindAppendOutcome(test::collection_id, test::operation_id)->has_value());
    auto mismatch = Request();
    mismatch.expected_snapshot = {SnapshotPreconditionKind::Equals, 7};
    const auto failed = engine->AppendDurable(mismatch);
    EXPECT_EQ(test::Present(failed.error).code, "PRECONDITION_FAILED");
    EXPECT_EQ(engine->AppendDurable(Request()), failed);
    EXPECT_EQ(backend->prepares, 0);
}

TEST_F(CoordinatorTest, GuardOrInnerFailureRollsBackPointerAndStoresFailure) {
    backend->fail_guard = true;
    const auto guarded = engine->AppendDurable(Request());
    ASSERT_EQ(guarded.disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_FALSE(engine->GetCollection(test::collection_id)->current_snapshot_id);
    backend->fail_guard = false;
    backend->fail_after_pointer = true;
    const auto inner = engine->AppendDurable(Request("00000000-0000-0000-0000-000000000006"));
    ASSERT_EQ(inner.disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_FALSE(engine->GetCollection(test::collection_id)->current_snapshot_id);
    EXPECT_TRUE(Reopen());
}

TEST_F(CoordinatorTest, CreateRetryRequiresExactNameIdAndSchema) {
    const CreateCollectionRequest request{test::collection_id, "events", SchemaSpec{{{"n", FieldType::Int64, true}}}};
    EXPECT_EQ(engine->CreateCollection(request).disposition, CommitDisposition::Committed);
    EXPECT_EQ(engine->ResolveCollection("event").error().code, "COLLECTION_NOT_FOUND");
    auto other = request;
    other.schema.fields[0].nullable = false;
    EXPECT_EQ(test::Present(engine->CreateCollection(other).error).code, "SCHEMA_MISMATCH");
    other = request;
    other.collection_id = "00000000-0000-0000-0000-000000000006";
    EXPECT_EQ(test::Present(engine->CreateCollection(other).error).code, "ALREADY_EXISTS");
}

TEST_F(CoordinatorTest, SameProcessSecondOpenIsLockedAndMissingOpenNeverCreates) {
    const auto second = detail::EngineFactory::Open({root.string(), test::store_id}, backend);
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "STATE_IN_USE");
    const auto missing = temporary.path / "missing";
    EXPECT_FALSE(OpenExisting({missing.string(), test::store_id}));
    EXPECT_FALSE(std::filesystem::exists(missing));
    auto opened = Reopen();
    ASSERT_TRUE(opened);
}

TEST_F(CoordinatorTest, CorruptOrMissingPublishedRootClosesReadiness) {
    ASSERT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::Committed);
    std::filesystem::remove(backend->last_root);
    const auto opened = Reopen();
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, "CORRUPTION");
    EXPECT_TRUE(std::filesystem::exists(root / "catalog.sqlite"));
}

TEST_F(CoordinatorTest, VerifierExceptionCannotTurnCommittedRetryIntoFailure) {
    ASSERT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::Committed);
    backend->throw_verifier = true;
    const auto retry = engine->AppendDurable(Request());
    EXPECT_EQ(retry.disposition, CommitDisposition::CommitUnknown);
    EXPECT_FALSE(engine->GetCollection(test::collection_id));
    backend->throw_verifier = false;
    EXPECT_TRUE(engine->Reconcile());
    EXPECT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::Committed);
}

TEST_F(CoordinatorTest, DetectedCorruptionBlocksFurtherWritesInTheLiveProcess) {
    ASSERT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::Committed);
    std::filesystem::remove(backend->last_root);
    const auto failed = engine->AppendDurable(Request("00000000-0000-0000-0000-000000000006"));
    ASSERT_EQ(failed.disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_EQ(test::Present(failed.error).code, "CORRUPTION");
    EXPECT_FALSE(engine->GetCollection(test::collection_id));
    const auto blocked = engine->AppendDurable(Request("00000000-0000-0000-0000-000000000007"));
    EXPECT_EQ(test::Present(blocked.error).code, "RECOVERY_REQUIRED");
    EXPECT_EQ(backend->prepares, 2);
}

TEST_F(CoordinatorTest, ActiveLookupAndShutdownTimeoutDoNotInventFailure) {
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    backend->on_prepare = [&] {
        entered.set_value();
        released.wait();
    };
    auto write = std::async(std::launch::async, [&] { return engine->AppendDurable(Request()); });
    entered.get_future().wait();
    const auto lookup = engine->FindAppendOutcome(test::collection_id, test::operation_id);
    ASSERT_FALSE(lookup);
    EXPECT_EQ(lookup.error().code, "OPERATION_IN_PROGRESS");
    const auto shutdown = engine->Shutdown(std::chrono::milliseconds(1));
    EXPECT_FALSE(shutdown);
    EXPECT_EQ(shutdown.error().code, "DEADLINE_EXCEEDED");
    release.set_value();
    const auto result = write.get();
    EXPECT_EQ(result.disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_EQ(test::Present(result.error).code, "CANCELLED");
    EXPECT_TRUE(engine->Shutdown(std::chrono::milliseconds(1000)));
}

TEST_F(CoordinatorTest, ShutdownReportsRecoveryExceptionAndRetainsOwnership) {
    ASSERT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::Committed);
    backend->throw_verifier = true;
    ASSERT_EQ(engine->AppendDurable(Request()).disposition, CommitDisposition::CommitUnknown);
    Status shutdown;
    EXPECT_NO_THROW(shutdown = engine->Shutdown(std::chrono::milliseconds(1000)));
    ASSERT_FALSE(shutdown);
    EXPECT_EQ(shutdown.error().code, "INTERNAL");
    const auto other = detail::EngineFactory::Open({root.string(), test::store_id}, backend);
    ASSERT_FALSE(other);
    EXPECT_EQ(other.error().code, "STATE_IN_USE");
    backend->throw_verifier = false;
    EXPECT_TRUE(engine->Shutdown(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(engine->Shutdown(std::chrono::milliseconds(1000)));
}

TEST(EngineBootstrap, NoProducerFailsClosedAndIdentityIsExplicit) {
    test::TempDir temporary;
    const auto root = temporary.path / "store";
    const auto created = CreateStore({test::store_id, root.string()});
    ASSERT_TRUE(created);
    EXPECT_EQ((*created)->GetStoreInfo().store_id, test::store_id);
    const auto collection =
        (*created)->CreateCollection({test::collection_id, "events", SchemaSpec{{{"n", FieldType::Int64, true}}}});
    EXPECT_EQ(collection.disposition, CommitDisposition::DefinitelyNotCommitted);
    EXPECT_EQ(test::Present(collection.error).code, "DEPENDENCY_UNAVAILABLE");
    EXPECT_FALSE(CreateStore({test::store_id, root.string()}));
}

} // namespace
} // namespace glamdb
