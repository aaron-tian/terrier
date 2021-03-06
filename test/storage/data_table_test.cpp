#include "storage/data_table.h"
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/object_pool.h"
#include "storage/storage_util.h"
#include "transaction/transaction_context.h"
#include "transaction/transaction_util.h"
#include "util/storage_test_util.h"
#include "util/test_harness.h"

namespace terrier {
// Not thread-safe
class RandomDataTableTestObject {
 public:
  template <class Random>
  RandomDataTableTestObject(storage::BlockStore *block_store, const uint16_t max_col, const double null_bias,
                            Random *generator)
      : layout_(StorageTestUtil::RandomLayout(max_col, generator)),
        table_(block_store, layout_),
        null_bias_(null_bias) {}

  ~RandomDataTableTestObject() {
    for (auto ptr : loose_pointers_) delete[] ptr;
    for (auto ptr : loose_txns_) delete ptr;
    delete[] select_buffer_;
  }

  template <class Random>
  storage::TupleSlot InsertRandomTuple(const timestamp_t timestamp, Random *generator,
                                       common::ObjectPool<storage::BufferSegment> *buffer_pool) {
    // generate a random redo ProjectedRow to Insert
    auto *redo_buffer = common::AllocationUtil::AllocateAligned(redo_initializer_.ProjectedRowSize());
    loose_pointers_.push_back(redo_buffer);
    storage::ProjectedRow *redo = redo_initializer_.InitializeRow(redo_buffer);
    StorageTestUtil::PopulateRandomRow(redo, layout_, null_bias_, generator);

    // generate a txn with an UndoRecord to populate on Insert
    auto *txn = new transaction::TransactionContext(timestamp, timestamp, buffer_pool);
    loose_txns_.push_back(txn);

    storage::TupleSlot slot = table_.Insert(txn, *redo);
    inserted_slots_.push_back(slot);
    tuple_versions_[slot].emplace_back(timestamp, redo);

    return slot;
  }

  // be sure to only update tuple incrementally (cannot go back in time)
  template <class Random>
  bool RandomlyUpdateTuple(const timestamp_t timestamp, const storage::TupleSlot slot, Random *generator,
                           common::ObjectPool<storage::BufferSegment> *buffer_pool) {
    // tuple must already exist
    TERRIER_ASSERT(tuple_versions_.find(slot) != tuple_versions_.end(), "Slot not found.");

    // generate a random redo ProjectedRow to Update
    std::vector<uint16_t> update_col_ids = StorageTestUtil::ProjectionListRandomColumns(layout_, generator);
    storage::ProjectedRowInitializer update_initializer(layout_, update_col_ids);
    auto *update_buffer = common::AllocationUtil::AllocateAligned(update_initializer.ProjectedRowSize());
    storage::ProjectedRow *update = update_initializer.InitializeRow(update_buffer);
    StorageTestUtil::PopulateRandomRow(update, layout_, null_bias_, generator);

    // generate a txn with an UndoRecord to populate on Insert
    auto *txn = new transaction::TransactionContext(timestamp, timestamp, buffer_pool);
    loose_txns_.push_back(txn);

    bool result = table_.Update(txn, slot, *update);

    if (result) {
      // manually apply the delta in an append-only fashion
      auto *version_buffer = common::AllocationUtil::AllocateAligned(redo_initializer_.ProjectedRowSize());
      loose_pointers_.push_back(version_buffer);
      // Copy previous version
      TERRIER_MEMCPY(version_buffer, tuple_versions_[slot].back().second, redo_initializer_.ProjectedRowSize());
      auto *version = reinterpret_cast<storage::ProjectedRow *>(version_buffer);
      // apply delta
      storage::StorageUtil::ApplyDelta(layout_, *update, version);
      tuple_versions_[slot].emplace_back(timestamp, version);
    }

    // the update buffer does not need to live past this scope
    delete[] update_buffer;
    return result;
  }

  const storage::BlockLayout &Layout() const { return layout_; }

  const std::vector<storage::TupleSlot> &InsertedTuples() const { return inserted_slots_; }

  // or nullptr of no version of this tuple is visible to the timestamp
  const storage::ProjectedRow *GetReferenceVersionedTuple(const storage::TupleSlot slot, const timestamp_t timestamp) {
    TERRIER_ASSERT(tuple_versions_.find(slot) != tuple_versions_.end(), "Slot not found.");
    auto &versions = tuple_versions_[slot];
    // search backwards so the first entry with smaller timestamp can be returned
    for (auto i = static_cast<int64_t>(versions.size() - 1); i >= 0; i--)
      if (transaction::TransactionUtil::NewerThan(timestamp, versions[i].first) || timestamp == versions[i].first)
        return versions[i].second;
    return nullptr;
  }

  storage::ProjectedRow *SelectIntoBuffer(const storage::TupleSlot slot, const timestamp_t timestamp,
                                          common::ObjectPool<storage::BufferSegment> *buffer_pool) {
    // generate a txn with an UndoRecord to populate on Insert
    auto *txn = new transaction::TransactionContext(timestamp, timestamp, buffer_pool);
    loose_txns_.push_back(txn);

    // generate a redo ProjectedRow for Select
    storage::ProjectedRow *select_row = redo_initializer_.InitializeRow(select_buffer_);
    table_.Select(txn, slot, select_row);
    return select_row;
  }

 private:
  storage::BlockLayout layout_;
  storage::DataTable table_;
  std::vector<storage::TupleSlot> inserted_slots_;
  using tuple_version = std::pair<timestamp_t, storage::ProjectedRow *>;
  // oldest to newest
  std::unordered_map<storage::TupleSlot, std::vector<tuple_version>> tuple_versions_;
  std::vector<byte *> loose_pointers_;
  std::vector<transaction::TransactionContext *> loose_txns_;
  double null_bias_;
  // These always over-provision in the case of partial selects or deltas, which is fine.
  storage::ProjectedRowInitializer redo_initializer_{layout_, StorageTestUtil::ProjectionListAllColumns(layout_)};
  byte *select_buffer_ = common::AllocationUtil::AllocateAligned(redo_initializer_.ProjectedRowSize());
};

struct DataTableTests : public TerrierTest {
  storage::BlockStore block_store_{100};
  common::ObjectPool<storage::BufferSegment> buffer_pool_{10000};
  std::default_random_engine generator_;
  std::uniform_real_distribution<double> null_ratio_{0.0, 1.0};
};

// Generates a random table layout and coin flip bias for an attribute being null, inserts num_inserts random tuples
// into an empty DataTable. Then, Selects the inserted TupleSlots and compares the results to the original inserted
// random tuple. Repeats for num_iterations.
// NOLINTNEXTLINE
TEST_F(DataTableTests, SimpleInsertSelect) {
  const uint32_t num_iterations = 50;
  const uint32_t num_inserts = 1000;
  const uint16_t max_columns = 100;
  for (uint32_t iteration = 0; iteration < num_iterations; ++iteration) {
    RandomDataTableTestObject tested(&block_store_, max_columns, null_ratio_(generator_), &generator_);

    // Populate the table with random tuples
    for (uint32_t i = 0; i < num_inserts; ++i) tested.InsertRandomTuple(timestamp_t(0), &generator_, &buffer_pool_);

    EXPECT_EQ(num_inserts, tested.InsertedTuples().size());

    std::vector<uint16_t> all_cols = StorageTestUtil::ProjectionListAllColumns(tested.Layout());
    for (const auto &inserted_tuple : tested.InsertedTuples()) {
      storage::ProjectedRow *stored = tested.SelectIntoBuffer(inserted_tuple, timestamp_t(1), &buffer_pool_);
      const storage::ProjectedRow *ref = tested.GetReferenceVersionedTuple(inserted_tuple, timestamp_t(1));
      EXPECT_TRUE(StorageTestUtil::ProjectionListEqual(tested.Layout(), stored, ref));
    }
  }
}

// Generates a random table layout and coin flip bias for an attribute being null, inserts 1 random tuple into an empty
// DataTable. Then, randomly updates the tuple num_updates times. Finally, Selects at each timestamp to verify that the
// delta chain produces the correct tuple. Repeats for num_iterations.
// NOLINTNEXTLINE
TEST_F(DataTableTests, SimpleVersionChain) {
  const uint32_t num_iterations = 50;
  const uint32_t num_updates = 10;
  const uint16_t max_columns = 100;

  for (uint32_t iteration = 0; iteration < num_iterations; ++iteration) {
    RandomDataTableTestObject tested(&block_store_, max_columns, null_ratio_(generator_), &generator_);
    timestamp_t timestamp(0);

    storage::TupleSlot tuple = tested.InsertRandomTuple(timestamp++, &generator_, &buffer_pool_);
    EXPECT_EQ(1, tested.InsertedTuples().size());

    for (uint32_t i = 0; i < num_updates; ++i)
      tested.RandomlyUpdateTuple(timestamp++, tuple, &generator_, &buffer_pool_);

    std::vector<byte *> select_buffers(num_updates + 1);

    uint32_t num_versions = num_updates + 1;
    std::vector<uint16_t> all_col_ids = StorageTestUtil::ProjectionListAllColumns(tested.Layout());
    for (uint32_t i = 0; i < num_versions; i++) {
      const storage::ProjectedRow *reference_version = tested.GetReferenceVersionedTuple(tuple, timestamp_t(i));
      storage::ProjectedRow *stored_version = tested.SelectIntoBuffer(tuple, timestamp_t(i), &buffer_pool_);
      EXPECT_TRUE(StorageTestUtil::ProjectionListEqual(tested.Layout(), reference_version, stored_version));
    }
  }
}

// Generates a random table layout and coin flip bias for an attribute being null, inserts 1 random tuple into an empty
// DataTable. Then, randomly updates the tuple with a negative timestamp, representing an uncommitted transaction. Then
// a second update attempts to change the tuple and should fail. Then, the first transaction's timestamp is updated to a
// positive number representing a commit. Then, the second transaction updates again and should succeed, and its
// timestamp is changed to positive. Lastly, Selects at first timestamp to verify that the delta chain produces the
// correct tuple. Repeats for num_iterations.
// NOLINTNEXTLINE
TEST_F(DataTableTests, WriteWriteConflictUpdateFails) {
  const uint32_t num_iterations = 50;
  const uint16_t max_columns = 100;

  for (uint32_t iteration = 0; iteration < num_iterations; ++iteration) {
    RandomDataTableTestObject tested(&block_store_, max_columns, null_ratio_(generator_), &generator_);
    storage::TupleSlot tuple = tested.InsertRandomTuple(timestamp_t(0), &generator_, &buffer_pool_);
    // take the write lock by updating with "negative" timestamp
    EXPECT_TRUE(tested.RandomlyUpdateTuple(timestamp_t(UINT64_MAX), tuple, &generator_, &buffer_pool_));
    // second transaction attempts to write, should fail
    EXPECT_FALSE(tested.RandomlyUpdateTuple(timestamp_t(1), tuple, &generator_, &buffer_pool_));

    std::vector<uint16_t> all_col_ids = StorageTestUtil::ProjectionListAllColumns(tested.Layout());
    storage::ProjectedRow *stored = tested.SelectIntoBuffer(tuple, timestamp_t(UINT64_MAX), &buffer_pool_);
    const storage::ProjectedRow *ref = tested.GetReferenceVersionedTuple(tuple, timestamp_t(UINT64_MAX));
    EXPECT_TRUE(StorageTestUtil::ProjectionListEqual(tested.Layout(), ref, stored));
  }
}
}  // namespace terrier
