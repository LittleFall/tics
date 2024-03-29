#pragma once

#include <Core/Block.h>
#include <Interpreters/ExpressionActions.h>
#include <Storages/DeltaMerge/DeltaTree.h>
#include <Storages/DeltaMerge/DeltaValueSpace.h>
#include <Storages/DeltaMerge/Index/MinMax.h>
#include <Storages/DeltaMerge/Range.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/DeltaMerge/SkippableBlockInputStream.h>
#include <Storages/DeltaMerge/StableValueSpace.h>
#include <Storages/Page/PageDefines.h>

namespace DB
{
class WriteBatch;
namespace DM
{

class Segment;
struct SegmentSnapshot;
using SegmentSnapshotPtr = std::shared_ptr<SegmentSnapshot>;
class StableValueSpace;
using StableValueSpacePtr = std::shared_ptr<StableValueSpace>;
class DeltaValueSpace;
using DeltaValueSpacePtr = std::shared_ptr<DeltaValueSpace>;
class RSOperator;
using RSOperatorPtr = std::shared_ptr<RSOperator>;

using SegmentPtr  = std::shared_ptr<Segment>;
using SegmentPair = std::pair<SegmentPtr, SegmentPtr>;
using Segments    = std::vector<SegmentPtr>;

/// A structure stores the informations to constantly read a segment instance.
struct SegmentSnapshot : private boost::noncopyable
{
    DeltaSnapshotPtr  delta;
    StableSnapshotPtr stable;

    SegmentSnapshot(const DeltaSnapshotPtr & delta_, const StableSnapshotPtr & stable_) : delta(delta_), stable(stable_) {}

    SegmentSnapshotPtr clone() { return std::make_shared<SegmentSnapshot>(delta->clone(), stable->clone()); }

    UInt64 getBytes() { return delta->getBytes() + stable->getBytes(); }
    UInt64 getRows() { return delta->getRows() + stable->getRows(); }
};

/// A segment contains many rows of a table. A table is split into segments by consecutive ranges.
///
/// The data of stable value space is stored in "data" storage, while data of delta value space is stored in "log" storage.
/// And all meta data is stored in "meta" storage.
class Segment : private boost::noncopyable
{
public:
    using Version = UInt32;
    static const Version CURRENT_VERSION;

    using DeltaTree = DefaultDeltaTree;

    struct ReadInfo
    {
        DeltaIndexIterator index_begin;
        DeltaIndexIterator index_end;

        ColumnDefines read_columns;
    };

    struct SplitInfo
    {
        bool        is_logical;
        RowKeyValue split_point;

        StableValueSpacePtr my_stable;
        StableValueSpacePtr other_stable;
    };

    Segment(const Segment &) = delete;
    Segment & operator=(const Segment &) = delete;
    Segment & operator=(Segment &&) = delete;

    Segment(UInt64                      epoch_, //
            const RowKeyRange &         rowkey_range_,
            PageId                      segment_id_,
            PageId                      next_segment_id_,
            const DeltaValueSpacePtr &  delta_,
            const StableValueSpacePtr & stable_);

    static SegmentPtr newSegment(DMContext &         context, //
                                 const RowKeyRange & rowkey_range,
                                 PageId              segment_id,
                                 PageId              next_segment_id,
                                 PageId              delta_id,
                                 PageId              stable_id);
    static SegmentPtr newSegment(DMContext &         context, //
                                 const RowKeyRange & rowkey_range,
                                 PageId              segment_id,
                                 PageId              next_segment_id);

    static SegmentPtr restoreSegment(DMContext & context, PageId segment_id);

    void serialize(WriteBatch & wb);

    bool writeToDisk(DMContext & dm_context, const PackPtr & pack);
    bool writeToCache(DMContext & dm_context, const Block & block, size_t offset, size_t limit);
    bool write(DMContext & dm_context, const Block & block); // For test only
    bool write(DMContext & dm_context, const RowKeyRange & delete_range);

    SegmentSnapshotPtr createSnapshot(const DMContext & dm_context, bool is_update = false) const;

    BlockInputStreamPtr getInputStream(const DMContext &          dm_context,
                                       const ColumnDefines &      columns_to_read,
                                       const SegmentSnapshotPtr & segment_snap,
                                       const RowKeyRanges &       read_ranges,
                                       const RSOperatorPtr &      filter,
                                       UInt64                     max_version,
                                       size_t                     expected_block_size);

    BlockInputStreamPtr getInputStream(const DMContext &     dm_context,
                                       const ColumnDefines & columns_to_read,
                                       const RowKeyRanges &  read_ranges,
                                       const RSOperatorPtr & filter              = {},
                                       UInt64                max_version         = MAX_UINT64,
                                       size_t                expected_block_size = DEFAULT_BLOCK_SIZE);

    /// Return a stream which is suitable for exporting data.
    ///  reorgize_block: put those rows with the same pk rows into the same block or not.
    BlockInputStreamPtr getInputStreamForDataExport(const DMContext &          dm_context,
                                                    const ColumnDefines &      columns_to_read,
                                                    const SegmentSnapshotPtr & segment_snap,
                                                    const RowKeyRange &        data_range,
                                                    size_t                     expected_block_size = DEFAULT_BLOCK_SIZE,
                                                    bool                       reorgnize_block     = true) const;

    BlockInputStreamPtr getInputStreamRaw(const DMContext &          dm_context,
                                          const ColumnDefines &      columns_to_read,
                                          const SegmentSnapshotPtr & segment_snap,
                                          bool                       do_range_filter,
                                          size_t                     expected_block_size = DEFAULT_BLOCK_SIZE);

    BlockInputStreamPtr getInputStreamRaw(const DMContext & dm_context, const ColumnDefines & columns_to_read);

    /// For those split, merge and mergeDelta methods, we should use prepareXXX/applyXXX combo in real production.
    /// split(), merge() and mergeDelta() are only used in test cases.

    SegmentPair split(DMContext & dm_context) const;
    SplitInfo prepareSplit(DMContext & dm_context, const SegmentSnapshotPtr & segment_snap, WriteBatches & wbs, bool need_rate_limit) const;
    SegmentPair
    applySplit(DMContext & dm_context, const SegmentSnapshotPtr & segment_snap, WriteBatches & wbs, SplitInfo & split_info) const;

    static SegmentPtr          merge(DMContext & dm_context, const SegmentPtr & left, const SegmentPtr & right);
    static StableValueSpacePtr prepareMerge(DMContext &                dm_context, //
                                            const SegmentPtr &         left,
                                            const SegmentSnapshotPtr & left_snap,
                                            const SegmentPtr &         right,
                                            const SegmentSnapshotPtr & right_snap,
                                            WriteBatches &             wbs,
                                            bool                       need_rate_limit);
    static SegmentPtr          applyMerge(DMContext &                 dm_context, //
                                          const SegmentPtr &          left,
                                          const SegmentSnapshotPtr &  left_snap,
                                          const SegmentPtr &          right,
                                          const SegmentSnapshotPtr &  right_snap,
                                          WriteBatches &              wbs,
                                          const StableValueSpacePtr & merged_stable);

    SegmentPtr mergeDelta(DMContext & dm_context) const;
    StableValueSpacePtr
               prepareMergeDelta(DMContext & dm_context, const SegmentSnapshotPtr & segment_snap, WriteBatches & wbs, bool need_rate_limit) const;
    SegmentPtr applyMergeDelta(DMContext &                 dm_context,
                               const SegmentSnapshotPtr &  segment_snap,
                               WriteBatches &              wbs,
                               const StableValueSpacePtr & new_stable) const;

    /// Flush delta's cache packs.
    bool flushCache(DMContext & dm_context);
    void placeDeltaIndex(DMContext & dm_context);

    bool compactDelta(DMContext & dm_context);

    size_t getEstimatedRows() const { return delta->getRows() + stable->getRows(); }
    size_t getEstimatedBytes() const { return delta->getBytes() + stable->getBytes(); }

    PageId segmentId() const { return segment_id; }
    PageId nextSegmentId() const { return next_segment_id; }

    void check(DMContext & dm_context, const String & when) const;

    const RowKeyRange & getRowKeyRange() const { return rowkey_range; }

    const DeltaValueSpacePtr &  getDelta() const { return delta; }
    const StableValueSpacePtr & getStable() const { return stable; }

    String simpleInfo() const;
    String info() const;

    using Lock = DeltaValueSpace::Lock;
    bool getUpdateLock(Lock & lock) const { return delta->getLock(lock); }

    Lock mustGetUpdateLock() const
    {
        Lock lock;
        if (!getUpdateLock(lock))
            throw Exception("Segment [" + DB::toString(segmentId()) + "] get update lock failed", ErrorCodes::LOGICAL_ERROR);
        return lock;
    }

    void abandon(DMContext & context)
    {
        LOG_DEBUG(log, "Abandon segment [" << segment_id << "]");
        delta->abandon(context);
    }
    bool hasAbandoned() { return delta->hasAbandoned(); }

    void drop(const FileProviderPtr & file_provider) { stable->drop(file_provider); }

    RowsAndBytes
    getRowsAndBytesInRange(DMContext & dm_context, const SegmentSnapshotPtr & segment_snap, const RowKeyRange & check_range, bool is_exact);

private:
    ReadInfo getReadInfo(const DMContext &          dm_context,
                         const ColumnDefines &      read_columns,
                         const SegmentSnapshotPtr & segment_snap,
                         const RowKeyRanges &       read_ranges,
                         UInt64                     max_version = MAX_UINT64) const;

    static ColumnDefines arrangeReadColumns(const ColumnDefine & handle, const ColumnDefines & columns_to_read);

    /// Create a stream which merged delta and stable streams together.
    template <bool skippable_place = false, class IndexIterator = DeltaIndexIterator>
    static SkippableBlockInputStreamPtr getPlacedStream(const DMContext &         dm_context,
                                                        const ColumnDefines &     read_columns,
                                                        const RowKeyRange &       rowkey_range,
                                                        const RSOperatorPtr &     filter,
                                                        const StableSnapshotPtr & stable_snap,
                                                        DeltaSnapshotPtr &        delta_snap,
                                                        const IndexIterator &     delta_index_begin,
                                                        const IndexIterator &     delta_index_end,
                                                        size_t                    expected_block_size,
                                                        UInt64                    max_version = MAX_UINT64);

    /// Merge delta & stable, and then take the middle one.
    RowKeyValue getSplitPointSlow(DMContext & dm_context, const ReadInfo & read_info, const SegmentSnapshotPtr & segment_snap) const;
    /// Only look up in the stable vs.
    RowKeyValue getSplitPointFast(DMContext & dm_context, const StableSnapshotPtr & stable_snap) const;

    SplitInfo prepareSplitLogical(DMContext &                dm_context, //
                                  const SegmentSnapshotPtr & segment_snap,
                                  RowKeyValue &              split_point,
                                  WriteBatches &             wbs) const;
    SplitInfo
    prepareSplitPhysical(DMContext & dm_context, const SegmentSnapshotPtr & segment_snap, WriteBatches & wbs, bool need_rate_limit) const;


    /// Make sure that all delta packs have been placed.
    /// Note that the index returned could be partial index, and cannot be updated to shared index.
    /// Returns <placed index, this index is fully indexed or not>
    std::pair<DeltaIndexPtr, bool> ensurePlace(const DMContext &         dm_context,
                                               const StableSnapshotPtr & stable_snap,
                                               DeltaSnapshotPtr &        delta_snap,
                                               const RowKeyRanges &      read_ranges,
                                               UInt64                    max_version) const;

    /// Reference the inserts/updates by delta tree.
    /// Returns fully placed or not. Some rows not match relevant_range are not placed.
    template <bool skippable_place>
    bool placeUpsert(const DMContext &         dm_context,
                     const StableSnapshotPtr & stable_snap,
                     DeltaSnapshotPtr &        delta_snap,
                     size_t                    delta_value_space_offset,
                     Block &&                  block,
                     DeltaTree &               delta_tree,
                     const RowKeyRange &       relevant_range) const;
    /// Reference the deletes by delta tree.
    /// Returns fully placed or not. Some rows not match relevant_range are not placed.
    template <bool skippable_place>
    bool placeDelete(const DMContext &         dm_context,
                     const StableSnapshotPtr & stable_snap,
                     DeltaSnapshotPtr &        delta_snap,
                     const RowKeyRange &       delete_range,
                     DeltaTree &               delta_tree,
                     const RowKeyRange &       relevant_range) const;

private:
    const UInt64 epoch; // After split / merge / merge delta, epoch got increased by 1.
    RowKeyRange  rowkey_range;
    bool         is_common_handle;
    size_t       rowkey_column_size;
    const PageId segment_id;
    const PageId next_segment_id;

    const DeltaValueSpacePtr  delta;
    const StableValueSpacePtr stable;

    Logger * log;
};

} // namespace DM
} // namespace DB
