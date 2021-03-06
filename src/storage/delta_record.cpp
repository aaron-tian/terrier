#include "storage/delta_record.h"
#include <algorithm>
#include <functional>
#include <utility>
#include <vector>
#include "storage/storage_util.h"

namespace terrier::storage {
ProjectedRow *ProjectedRow::CopyProjectedRowLayout(void *head, const ProjectedRow &other) {
  auto *result = reinterpret_cast<ProjectedRow *>(head);
  auto header_size = reinterpret_cast<uintptr_t>(&other.Bitmap()) - reinterpret_cast<uintptr_t>(&other);
  TERRIER_MEMCPY(result, &other, header_size);
  result->Bitmap().Clear(result->num_cols_);
  return result;
}

// TODO(Tianyu): I don't think we can reasonably fit these into a cache line?
ProjectedRowInitializer::ProjectedRowInitializer(const terrier::storage::BlockLayout &layout,
                                                 std::vector<uint16_t> col_ids)
    : col_ids_(std::move(col_ids)), offsets_(col_ids_.size()) {
  TERRIER_ASSERT(!col_ids_.empty(), "cannot initialize an empty ProjectedRow");
  TERRIER_ASSERT(col_ids.size() < layout.NumCols(),
                 "projected row should have number of columns smaller than the table's");
  // TODO(Tianyu): We should really assert that the projected row has a subset of columns, but that
  // is a bit more complicated.

  // Sort the projection list for optimal space utilization and delta application performance
  // If the col ids are valid ones laid out by BlockLayout, ascending order of id guarantees
  // descending order in attribute size.
  std::sort(col_ids_.begin(), col_ids_.end(), std::less<>());
  size_ = sizeof(ProjectedRow);  // size and num_col size
  // space needed to store col_ids, must be padded up so that the following offsets are aligned
  size_ = StorageUtil::PadUpToSize(sizeof(uint32_t), size_ + static_cast<uint32_t>(col_ids_.size() * sizeof(uint16_t)));
  // space needed to store value offsets, we pad up so that bitmaps start 64-bit aligned
  size_ = StorageUtil::PadUpToSize(sizeof(uint64_t), size_ + static_cast<uint32_t>(col_ids_.size() * sizeof(uint32_t)));
  // space needed to store the bitmap, padded up to the size of the first value in this projected row
  size_ = StorageUtil::PadUpToSize(layout.AttrSize(col_ids_[0]),
                                   size_ + common::RawBitmap::SizeInBytes(static_cast<uint32_t>(col_ids_.size())));
  for (uint32_t i = 0; i < col_ids_.size(); i++) {
    offsets_[i] = size_;
    // Pad up to either the next value's size, or 8 bytes at the end of the ProjectedRow.
    auto next_size =
        static_cast<uint8_t>(i == col_ids_.size() - 1 ? sizeof(uint64_t) : layout.AttrSize(col_ids_[i + 1]));
    size_ = StorageUtil::PadUpToSize(next_size, size_ + layout.AttrSize(col_ids_[i]));
  }
}

ProjectedRow *ProjectedRowInitializer::InitializeRow(void *head) const {
  TERRIER_ASSERT(reinterpret_cast<uintptr_t>(head) % sizeof(uint64_t) == 0,
                 "start of ProjectedRow needs to be aligned to 8 bytes to"
                 "ensure correctness of alignment of its members");
  auto *result = reinterpret_cast<ProjectedRow *>(head);
  result->size_ = size_;
  result->num_cols_ = static_cast<uint16_t>(col_ids_.size());
  for (uint32_t i = 0; i < col_ids_.size(); i++) result->ColumnIds()[i] = col_ids_[i];
  for (uint32_t i = 0; i < col_ids_.size(); i++) result->AttrValueOffsets()[i] = offsets_[i];
  result->Bitmap().Clear(result->num_cols_);
  return result;
}

UndoRecord *UndoRecord::Initialize(void *head, timestamp_t timestamp, TupleSlot slot, DataTable *table,
                                   const ProjectedRowInitializer &initializer) {
  auto *result = reinterpret_cast<UndoRecord *>(head);

  result->next_ = nullptr;
  result->timestamp_.store(timestamp);
  result->table_ = table;
  result->slot_ = slot;

  initializer.InitializeRow(result->varlen_contents_);

  return result;
}
}  // namespace terrier::storage
