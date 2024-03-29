#include <Columns/ColumnConst.h>
#include <Columns/ColumnFixedString.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Common/typeid_cast.h>
#include <Core/ColumnNumbers.h>
#include <DataStreams/IProfilingBlockInputStream.h>
#include <DataStreams/materializeBlock.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypesNumber.h>
#include <Functions/FunctionHelpers.h>
#include <Interpreters/Join.h>
#include <Interpreters/NullableUtils.h>
#include <common/logger_useful.h>

#include "executeQuery.h"


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_SET_DATA_VARIANT;
    extern const int LOGICAL_ERROR;
    extern const int SET_SIZE_LIMIT_EXCEEDED;
    extern const int TYPE_MISMATCH;
    extern const int ILLEGAL_COLUMN;
}

/// Do I need to use the hash table maps_*_full, in which we remember whether the row was joined.
static bool getFullness(ASTTableJoin::Kind kind)
{
    return kind == ASTTableJoin::Kind::Right || kind == ASTTableJoin::Kind::Full;
}

Join::Join(const Names & key_names_left_, const Names & key_names_right_, bool use_nulls_,
    const SizeLimits & limits, ASTTableJoin::Kind kind_, ASTTableJoin::Strictness strictness_, size_t build_concurrency_,
    const TiDB::TiDBCollators & collators_, const String & left_filter_column_, const String & right_filter_column_,
    const String & other_filter_column_, ExpressionActionsPtr other_condition_ptr_)
    : kind(kind_), strictness(strictness_),
    key_names_left(key_names_left_),
    key_names_right(key_names_right_),
    use_nulls(use_nulls_),
    build_concurrency(build_concurrency_),
    collators(collators_),
    left_filter_column(left_filter_column_),
    right_filter_column(right_filter_column_),
    other_filter_column(other_filter_column_),
    other_condition_ptr(other_condition_ptr_),
    original_strictness(strictness),
    have_finish_build(true),
    log(&Logger::get("Join")),
    limits(limits)
{
    build_set_exceeded.store(false);
    for (size_t i = 0; i < build_concurrency; i++)
        pools.emplace_back(std::make_shared<Arena>());
    if (build_concurrency > 1 && getFullness(kind))
        pools.emplace_back(std::make_shared<Arena>());
    if (!other_filter_column.empty())
    {
        /// if there is other_condition, then should keep all the valid rows during probe stage
        if (strictness == ASTTableJoin::Strictness::Any)
        {
            strictness = ASTTableJoin::Strictness::All;
        }
    }
}

void Join::setFinishBuildTable(bool finish_)
{
    std::lock_guard<std::mutex> lk(build_table_mutex);
    have_finish_build = finish_;
    build_table_cv.notify_all();
}


Join::Type Join::chooseMethod(const ColumnRawPtrs & key_columns, Sizes & key_sizes)
{
    size_t keys_size = key_columns.size();

    if (keys_size == 0)
        return Type::CROSS;

    bool all_fixed = true;
    size_t keys_bytes = 0;
    key_sizes.resize(keys_size);
    for (size_t j = 0; j < keys_size; ++j)
    {
        if (!key_columns[j]->isFixedAndContiguous())
        {
            all_fixed = false;
            break;
        }
        key_sizes[j] = key_columns[j]->sizeOfValueIfFixed();
        keys_bytes += key_sizes[j];
    }

    /// If there is one numeric key that fits in 64 bits
    if (keys_size == 1 && key_columns[0]->isNumeric())
    {
        size_t size_of_field = key_columns[0]->sizeOfValueIfFixed();
        if (size_of_field == 1)
            return Type::key8;
        if (size_of_field == 2)
            return Type::key16;
        if (size_of_field == 4)
            return Type::key32;
        if (size_of_field == 8)
            return Type::key64;
        if (size_of_field == 16)
            return Type::keys128;
        throw Exception("Logical error: numeric column has sizeOfField not in 1, 2, 4, 8, 16.", ErrorCodes::LOGICAL_ERROR);
    }

    /// If the keys fit in N bits, we will use a hash table for N-bit-packed keys
    if (all_fixed && keys_bytes <= 16)
        return Type::keys128;
    if (all_fixed && keys_bytes <= 32)
        return Type::keys256;

    /// If there is single string key, use hash table of it's values.
    if (keys_size == 1
        && (typeid_cast<const ColumnString *>(key_columns[0])
            || (key_columns[0]->isColumnConst() && typeid_cast<const ColumnString *>(&static_cast<const ColumnConst *>(key_columns[0])->getDataColumn()))))
        return Type::key_string;

    if (keys_size == 1 && typeid_cast<const ColumnFixedString *>(key_columns[0]))
        return Type::key_fixed_string;

    /// Otherwise, will use set of cryptographic hashes of unambiguously serialized values.
    return Type::hashed;
}


template <typename Maps>
static void initImpl(Maps & maps, Join::Type type, size_t build_concurrency)
{
    switch (type)
    {
        case Join::Type::EMPTY:            break;
        case Join::Type::CROSS:            break;

    #define M(TYPE) \
        case Join::Type::TYPE: maps.TYPE = std::make_unique<typename decltype(maps.TYPE)::element_type>(build_concurrency); break;
        APPLY_FOR_JOIN_VARIANTS(M)
    #undef M

        default:
            throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
    }
}

template <typename Maps>
static size_t getTotalRowCountImpl(const Maps & maps, Join::Type type)
{
    switch (type)
    {
        case Join::Type::EMPTY:            return 0;
        case Join::Type::CROSS:            return 0;

    #define M(NAME) \
        case Join::Type::NAME: return maps.NAME ? maps.NAME->rowCount() : 0;
        APPLY_FOR_JOIN_VARIANTS(M)
    #undef M

        default:
            throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
    }
}

template <typename Maps>
static size_t getTotalByteCountImpl(const Maps & maps, Join::Type type)
{
    switch (type)
    {
        case Join::Type::EMPTY:            return 0;
        case Join::Type::CROSS:            return 0;

    #define M(NAME) \
        case Join::Type::NAME: return maps.NAME ? maps.NAME->getBufferSizeInBytes() : 0;
        APPLY_FOR_JOIN_VARIANTS(M)
    #undef M

        default:
            throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
    }
}


template <Join::Type type>
struct KeyGetterForType;

template <> struct KeyGetterForType<Join::Type::key8> { using Type = JoinKeyGetterOneNumber<UInt8>; };
template <> struct KeyGetterForType<Join::Type::key16> { using Type = JoinKeyGetterOneNumber<UInt16>; };
template <> struct KeyGetterForType<Join::Type::key32> { using Type = JoinKeyGetterOneNumber<UInt32>; };
template <> struct KeyGetterForType<Join::Type::key64> { using Type = JoinKeyGetterOneNumber<UInt64>; };
template <> struct KeyGetterForType<Join::Type::key_string> { using Type = JoinKeyGetterString; };
template <> struct KeyGetterForType<Join::Type::key_fixed_string> { using Type = JoinKeyGetterFixedString; };
template <> struct KeyGetterForType<Join::Type::keys128> { using Type = JoinKeyGetterFixed<UInt128>; };
template <> struct KeyGetterForType<Join::Type::keys256> { using Type = JoinKeyGetterFixed<UInt256>; };
template <> struct KeyGetterForType<Join::Type::hashed> { using Type = JoinKeyGetterHashed; };


void Join::init(Type type_)
{
    type = type_;

    if (kind == ASTTableJoin::Kind::Cross)
        return;

    if (!getFullness(kind))
    {
        if (strictness == ASTTableJoin::Strictness::Any)
            initImpl(maps_any, type, build_concurrency);
        else
            initImpl(maps_all, type, build_concurrency);
    }
    else
    {
        if (strictness == ASTTableJoin::Strictness::Any)
            initImpl(maps_any_full, type, build_concurrency);
        else
            initImpl(maps_all_full, type, build_concurrency);
    }
}

size_t Join::getTotalRowCount() const
{
    size_t res = 0;

    if (type == Type::CROSS)
    {
        for (const auto & block : blocks)
            res += block.rows();
    }
    else
    {
        res += getTotalRowCountImpl(maps_any, type);
        res += getTotalRowCountImpl(maps_all, type);
        res += getTotalRowCountImpl(maps_any_full, type);
        res += getTotalRowCountImpl(maps_all_full, type);
    }

    return res;
}

size_t Join::getTotalByteCount() const
{
    size_t res = 0;

    if (type == Type::CROSS)
    {
        for (const auto & block : blocks)
            res += block.bytes();
    }
    else
    {
        res += getTotalByteCountImpl(maps_any, type);
        res += getTotalByteCountImpl(maps_all, type);
        res += getTotalByteCountImpl(maps_any_full, type);
        res += getTotalByteCountImpl(maps_all_full, type);
        for (auto & pool : pools) {
            /// note the return value might not be accurate since it does not use lock, but should be enough for current usage
            res += pool->size();
        }
    }

    return res;
}


static void convertColumnToNullable(ColumnWithTypeAndName & column)
{
    column.type = makeNullable(column.type);
    if (column.column)
        column.column = makeNullable(column.column);
}


void Join::setSampleBlock(const Block & block)
{
    std::unique_lock lock(rwlock);

    if (!empty())
        return;

    size_t keys_size = key_names_right.size();
    ColumnRawPtrs key_columns(keys_size);

    for (size_t i = 0; i < keys_size; ++i)
    {
        key_columns[i] = block.getByName(key_names_right[i]).column.get();

        /// We will join only keys, where all components are not NULL.
        if (key_columns[i]->isColumnNullable())
            key_columns[i] = &static_cast<const ColumnNullable &>(*key_columns[i]).getNestedColumn();
    }

    /// Choose data structure to use for JOIN.
    init(chooseMethod(key_columns, key_sizes));

    sample_block_with_columns_to_add = materializeBlock(block);

    /// Move from `sample_block_with_columns_to_add` key columns to `sample_block_with_keys`, keeping the order.
    size_t pos = 0;
    while (pos < sample_block_with_columns_to_add.columns())
    {
        const auto & name = sample_block_with_columns_to_add.getByPosition(pos).name;
        if (key_names_right.end() != std::find(key_names_right.begin(), key_names_right.end(), name))
        {
            sample_block_with_keys.insert(sample_block_with_columns_to_add.getByPosition(pos));
            sample_block_with_columns_to_add.erase(pos);
        }
        else
            ++pos;
    }

    size_t num_columns_to_add = sample_block_with_columns_to_add.columns();

    for (size_t i = 0; i < num_columns_to_add; ++i)
    {
        auto & column = sample_block_with_columns_to_add.getByPosition(i);
        if (!column.column)
            column.column = column.type->createColumn();
    }

    /// In case of LEFT and FULL joins, if use_nulls, convert joined columns to Nullable.
    if (use_nulls && (kind == ASTTableJoin::Kind::Left || kind == ASTTableJoin::Kind::Full))
        for (size_t i = 0; i < num_columns_to_add; ++i)
            convertColumnToNullable(sample_block_with_columns_to_add.getByPosition(i));
}


namespace
{
    void insertRowToList(Join::RowRefList * list, Join::RowRefList * elem, Block * stored_block, size_t index) {
        elem->next = list->next;
        list->next = elem;
        elem->block = stored_block;
        elem->row_num = index;
    }

    /// Inserting an element into a hash table of the form `key -> reference to a string`, which will then be used by JOIN.
    template <ASTTableJoin::Strictness STRICTNESS, typename Map, typename KeyGetter>
    struct Inserter
    {
        static void insert(Map & map, const typename Map::key_type & key, Block * stored_block, size_t i, Arena & pool);
    };

    template <typename Map, typename KeyGetter>
    struct Inserter<ASTTableJoin::Strictness::Any, Map, KeyGetter>
    {
        using MappedType = typename Map::mapped_type;
        static void insert(Map & map, const typename Map::key_type & key, Block * stored_block, size_t i, Arena & pool)
        {
            typename Map::iterator it;
            bool inserted;
            map.emplace(key, it, inserted);

            if (inserted)
            {
                KeyGetter::onNewKey(it->first, pool);
                new (&it->second) MappedType(stored_block, i);
            }
        }
    };

    template <typename Map, typename KeyGetter>
    struct Inserter<ASTTableJoin::Strictness::All, Map, KeyGetter>
    {
        using MappedType = typename Map::mapped_type;
        static void insert(Map & map, const typename Map::key_type & key, Block * stored_block, size_t i, Arena & pool)
        {
            typename Map::iterator it;
            bool inserted;
            map.emplace(key, it, inserted);

            if (inserted)
            {
                KeyGetter::onNewKey(it->first, pool);
                new (&it->second) MappedType(stored_block, i);
            }
            else
            {
                /** The first element of the list is stored in the value of the hash table, the rest in the pool.
                 * We will insert each time the element into the second place.
                 * That is, the former second element, if it was, will be the third, and so on.
                 */
                auto elem = reinterpret_cast<MappedType *>(pool.alloc(sizeof(MappedType)));
                insertRowToList(&it->second, elem, stored_block, i);
            }
        }
    };


    template <ASTTableJoin::Strictness STRICTNESS, typename KeyGetter, typename Map, bool has_null_map>
    void NO_INLINE insertFromBlockImplTypeCase(
        Map & map, size_t rows, const ColumnRawPtrs & key_columns,
        size_t keys_size, const Sizes & key_sizes, const TiDB::TiDBCollators & collators,
        Block * stored_block, ConstNullMapPtr null_map, Join::RowRefList * rows_not_inserted_to_map,
        std::mutex &, size_t, Arenas & pools)
    {
        KeyGetter key_getter(key_columns, collators);
        std::vector<std::string> sort_key_containers;
        sort_key_containers.resize(key_columns.size());

        for (size_t i = 0; i < rows; ++i)
        {
            if (has_null_map && (*null_map)[i])
            {
                if (rows_not_inserted_to_map)
                {
                    /// for right/full out join, need to record the rows not inserted to map
                    auto elem = reinterpret_cast<Join::RowRefList *>(pools[0]->alloc(sizeof(Join::RowRefList)));
                    insertRowToList(rows_not_inserted_to_map, elem, stored_block, i);
                }
                continue;
            }

            auto key = key_getter.getKey(key_columns, keys_size, i, key_sizes, sort_key_containers);
            Inserter<STRICTNESS, typename Map::SegmentType::HashTable, KeyGetter>::insert(map.getSegmentTable(0), key, stored_block, i, *pools[0]);
        }
    }

    template <ASTTableJoin::Strictness STRICTNESS, typename KeyGetter, typename Map, bool has_null_map>
    void NO_INLINE insertFromBlockImplTypeCaseWithLock(
            Map & map, size_t rows, const ColumnRawPtrs & key_columns,
            size_t keys_size, const Sizes & key_sizes, const TiDB::TiDBCollators & collators,
            Block * stored_block, ConstNullMapPtr null_map, Join::RowRefList * rows_not_inserted_to_map,
            std::mutex & not_inserted_rows_mutex, size_t block_index, Arenas & pools)
    {
        KeyGetter key_getter(key_columns, collators);
        std::vector<std::string> sort_key_containers;
        sort_key_containers.resize(key_columns.size());
        size_t segment_size = map.getSegmentSize();
        /// when inserting with lock, first calculate and save the segment index for each row, then
        /// insert the rows segment by segment to avoid too much conflict. This will introduce some overheads:
        /// 1. key_getter.getKey will be called twice, here we do not cache key because it can not be cached
        /// with relatively low cost(if key is stringRef, just cache a stringRef is meaningless, we need to cache the whole `sort_key_containers`)
        /// 2. hash value is calculated twice, maybe we can refine the code to cache the hash value
        /// 3. extra memory to store the segment index info
        std::vector<std::vector<size_t>> segment_index_info;
        if (has_null_map && rows_not_inserted_to_map)
        {
            segment_index_info.resize(segment_size + 1);
        }
        else
        {
            segment_index_info.resize(segment_size);
        }
        size_t rows_per_seg = rows / segment_index_info.size();
        for (size_t i = 0; i < segment_index_info.size(); i++)
        {
            segment_index_info[i].reserve(rows_per_seg);
        }
        for (size_t i = 0; i < rows; i++)
        {
            if (has_null_map && (*null_map)[i])
            {
                if (rows_not_inserted_to_map)
                    segment_index_info[segment_index_info.size() - 1].push_back(i);
                continue;
            }
            auto key = key_getter.getKey(key_columns, keys_size, i, key_sizes, sort_key_containers);
            size_t segment_index = 0;
            size_t hash_value = 0;
            if (!map.isZero(key))
            {
                hash_value = map.hash(key);
                segment_index = hash_value % segment_size;
            }
            segment_index_info[segment_index].push_back(i);
        }
        for (size_t insert_index = 0; insert_index < segment_index_info.size(); insert_index++)
        {
            size_t segment_index = (insert_index + block_index) % segment_index_info.size();
            if (segment_index == segment_size)
            {
                /// null value
                std::lock_guard<std::mutex> lk(not_inserted_rows_mutex);
                for (size_t i = 0; i < segment_index_info[segment_index].size(); i++)
                {
                    /// for right/full out join, need to record the rows not inserted to map
                    auto elem = reinterpret_cast<Join::RowRefList *>(pools[segment_index]->alloc(sizeof(Join::RowRefList)));
                    insertRowToList(rows_not_inserted_to_map, elem, stored_block, segment_index_info[segment_index][i]);
                }
            }
            else
            {
                std::lock_guard<std::mutex> lk(map.getSegmentMutex(segment_index));
                for (size_t i = 0; i < segment_index_info[segment_index].size(); i++)
                {
                    auto key = key_getter.getKey(key_columns, keys_size, segment_index_info[segment_index][i], key_sizes, sort_key_containers);
                    Inserter<STRICTNESS, typename Map::SegmentType::HashTable, KeyGetter>::insert(map.getSegmentTable(segment_index), key, stored_block, segment_index_info[segment_index][i], *pools[segment_index]);
                }
            }
        }
    }


    template <ASTTableJoin::Strictness STRICTNESS, typename KeyGetter, typename Map>
    void insertFromBlockImplType(
        Map & map, size_t rows, const ColumnRawPtrs & key_columns,
        size_t keys_size, const Sizes & key_sizes, const TiDB::TiDBCollators & collators,
        Block * stored_block, ConstNullMapPtr null_map, Join::RowRefList * rows_not_inserted_to_map,
        std::mutex & not_inserted_rows_mutex, size_t block_index, size_t insert_concurrency, Arenas & pools)
    {
        if (null_map)
        {
            if (insert_concurrency > 1)
            {
                insertFromBlockImplTypeCaseWithLock<STRICTNESS, KeyGetter, Map, true>(map, rows, key_columns, keys_size, key_sizes,
                                                                              collators,
                                                                              stored_block, null_map,
                                                                              rows_not_inserted_to_map, not_inserted_rows_mutex, block_index, pools);
            }
            else
            {
                insertFromBlockImplTypeCase<STRICTNESS, KeyGetter, Map, true>(map, rows, key_columns, keys_size, key_sizes,
                                                                              collators,
                                                                              stored_block, null_map,
                                                                              rows_not_inserted_to_map, not_inserted_rows_mutex, block_index, pools);
            }
        }
        else
        {
            if (insert_concurrency > 1)
            {
                insertFromBlockImplTypeCaseWithLock<STRICTNESS, KeyGetter, Map, false>(map, rows, key_columns, keys_size, key_sizes,
                                                                               collators,
                                                                               stored_block, null_map,
                                                                               rows_not_inserted_to_map, not_inserted_rows_mutex, block_index, pools);
            }
            else
            {
                insertFromBlockImplTypeCase<STRICTNESS, KeyGetter, Map, false>(map, rows, key_columns, keys_size, key_sizes,
                                                                               collators,
                                                                               stored_block, null_map,
                                                                               rows_not_inserted_to_map, not_inserted_rows_mutex, block_index, pools);
            }
        }
    }


    template <ASTTableJoin::Strictness STRICTNESS, typename Maps>
    void insertFromBlockImpl(
        Join::Type type, Maps & maps, size_t rows, const ColumnRawPtrs & key_columns,
        size_t keys_size, const Sizes & key_sizes, const TiDB::TiDBCollators & collators,
        Block * stored_block, ConstNullMapPtr null_map, Join::RowRefList * rows_not_inserted_to_map,
        std::mutex & not_inserted_rows_mutex, size_t block_index, size_t insert_concurrency, Arenas & pools)
    {
        switch (type)
        {
            case Join::Type::EMPTY:            break;
            case Join::Type::CROSS:            break;    /// Do nothing. We have already saved block, and it is enough.

        #define M(TYPE) \
            case Join::Type::TYPE: \
                insertFromBlockImplType<STRICTNESS, typename KeyGetterForType<Join::Type::TYPE>::Type>(\
                    *maps.TYPE, rows, key_columns, keys_size, key_sizes, collators, stored_block, null_map,\
                    rows_not_inserted_to_map, not_inserted_rows_mutex, block_index, insert_concurrency, pools); \
                    break;
            APPLY_FOR_JOIN_VARIANTS(M)
        #undef M

            default:
                throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
        }
    }
}

void recordFilteredRows(const Block & block, const String & filter_column, ColumnPtr & null_map_holder, ConstNullMapPtr & null_map)
{
    if (filter_column.empty())
        return;
    auto & column = block.getByName(filter_column).column;
    if (column->isColumnNullable())
    {
        const ColumnNullable & column_nullable = static_cast<const ColumnNullable &>(*column);
        if (!null_map_holder)
        {
            null_map_holder = column_nullable.getNullMapColumnPtr();
        }
        else
        {
            MutableColumnPtr mutable_null_map_holder = (*std::move(null_map_holder)).mutate();

            PaddedPODArray<UInt8> & mutable_null_map = static_cast<ColumnUInt8 &>(*mutable_null_map_holder).getData();
            const PaddedPODArray<UInt8> & other_null_map = column_nullable.getNullMapData();
            for (size_t i = 0, size = mutable_null_map.size(); i < size; ++i)
                mutable_null_map[i] |= other_null_map[i];

            null_map_holder = std::move(mutable_null_map_holder);
        }
    }

    if (!null_map_holder)
    {
        null_map_holder = ColumnVector<UInt8>::create(column->size(), 0);
    }
    MutableColumnPtr mutable_null_map_holder = (*std::move(null_map_holder)).mutate();
    PaddedPODArray<UInt8> & mutable_null_map = static_cast<ColumnUInt8 &>(*mutable_null_map_holder).getData();

    auto & nested_column = column->isColumnNullable() ? static_cast<const ColumnNullable &>(*column).getNestedColumnPtr() : column;
    for (size_t i = 0, size = nested_column->size(); i < size; ++i)
        mutable_null_map[i] |= (!nested_column->getInt(i));

    null_map_holder = std::move(mutable_null_map_holder);

    null_map = &static_cast<const ColumnUInt8 &>(*null_map_holder).getData();
}

bool Join::insertFromBlock(const Block & block)
{
    if (empty())
        throw Exception("Logical error: Join was not initialized", ErrorCodes::LOGICAL_ERROR);
    std::unique_lock lock(rwlock);
    blocks.push_back(block);
    Block * stored_block = &blocks.back();
    return insertFromBlockInternal(stored_block, 0);
}

void Join::insertFromBlockASync(const Block & block, ThreadPool & thread_pool)
{
    if (empty())
        throw Exception("Logical error: Join was not initialized", ErrorCodes::LOGICAL_ERROR);
    std::shared_lock lock(rwlock);
    Block * stored_block = nullptr;
    size_t block_index = 0;
    {
        std::lock_guard<std::mutex> lk(blocks_lock);
        blocks.push_back(block);
        stored_block = &blocks.back();
        block_index = blocks.size();
    }
    auto memory_tracker = current_memory_tracker;
    thread_pool.schedule([&, stored_block, block_index, memory_tracker]
    {
        current_memory_tracker = memory_tracker;
        if (build_set_exceeded.load())
            return;
        if (!insertFromBlockInternal(stored_block, block_index))
        {
            build_set_exceeded.store(true);
        }
    });
}

bool Join::insertFromBlockInternal(Block * stored_block, size_t block_index)
{
    size_t keys_size = key_names_right.size();
    ColumnRawPtrs key_columns(keys_size);

    const Block & block = *stored_block;

    /// Rare case, when keys are constant. To avoid code bloat, simply materialize them.
    Columns materialized_columns;

    /// Memoize key columns to work.
    for (size_t i = 0; i < keys_size; ++i)
    {
        key_columns[i] = block.getByName(key_names_right[i]).column.get();

        if (ColumnPtr converted = key_columns[i]->convertToFullColumnIfConst())
        {
            materialized_columns.emplace_back(converted);
            key_columns[i] = materialized_columns.back().get();
        }
    }

    /// We will insert to the map only keys, where all components are not NULL.
    ColumnPtr null_map_holder;
    ConstNullMapPtr null_map{};
    extractNestedColumnsAndNullMap(key_columns, null_map_holder, null_map);
    /// reuse null_map to record the filtered rows, the rows contains NULL or does not
    /// match the join filter will not insert to the maps
    recordFilteredRows(block, right_filter_column, null_map_holder, null_map);

    size_t rows = block.rows();

    if (getFullness(kind))
    {
        /** Move the key columns to the beginning of the block.
          * This is where NonJoinedBlockInputStream will expect.
          */
        size_t key_num = 0;
        for (const auto & name : key_names_right)
        {
            size_t pos = stored_block->getPositionByName(name);
            ColumnWithTypeAndName col = stored_block->safeGetByPosition(pos);
            stored_block->erase(pos);
            stored_block->insert(key_num, std::move(col));
            ++key_num;
        }
    }
    else
    {
        /// Remove the key columns from stored_block, as they are not needed.
        for (const auto & name : key_names_right)
            stored_block->erase(stored_block->getPositionByName(name));
    }

    size_t size = stored_block->columns();

    /// Rare case, when joined columns are constant. To avoid code bloat, simply materialize them.
    for (size_t i = 0; i < size; ++i)
    {
        ColumnPtr col = stored_block->safeGetByPosition(i).column;
        if (ColumnPtr converted = col->convertToFullColumnIfConst())
            stored_block->safeGetByPosition(i).column = converted;
    }

    /// In case of LEFT and FULL joins, if use_nulls, convert joined columns to Nullable.
    if (use_nulls && (kind == ASTTableJoin::Kind::Left || kind == ASTTableJoin::Kind::Full))
    {
        for (size_t i = getFullness(kind) ? keys_size : 0; i < size; ++i)
        {
            convertColumnToNullable(stored_block->getByPosition(i));
        }
    }

    if (kind != ASTTableJoin::Kind::Cross)
    {
        /// Fill the hash table.
        if (!getFullness(kind))
        {
            if (strictness == ASTTableJoin::Strictness::Any)
                insertFromBlockImpl<ASTTableJoin::Strictness::Any>(type, maps_any, rows, key_columns, keys_size,
                        key_sizes, collators, stored_block, null_map, nullptr,
                        not_inserted_rows_mutex, block_index, build_concurrency, pools);
            else
                insertFromBlockImpl<ASTTableJoin::Strictness::All>(type, maps_all, rows, key_columns, keys_size,
                        key_sizes, collators, stored_block, null_map, nullptr,
                        not_inserted_rows_mutex, block_index, build_concurrency, pools);
        }
        else
        {
            if (strictness == ASTTableJoin::Strictness::Any)
                insertFromBlockImpl<ASTTableJoin::Strictness::Any>(type, maps_any_full, rows, key_columns, keys_size,
                        key_sizes, collators, stored_block, null_map, &rows_not_inserted_to_map,
                        not_inserted_rows_mutex, block_index, build_concurrency, pools);
            else
                insertFromBlockImpl<ASTTableJoin::Strictness::All>(type, maps_all_full, rows, key_columns, keys_size,
                        key_sizes, collators, stored_block, null_map, &rows_not_inserted_to_map,
                        not_inserted_rows_mutex, block_index, build_concurrency, pools);
        }
    }

    return limits.check(getTotalRowCount(), getTotalByteCount(), "JOIN", ErrorCodes::SET_SIZE_LIMIT_EXCEEDED);
}


namespace
{
    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, typename Map>
    struct Adder;

    template <typename Map>
    struct Adder<ASTTableJoin::Kind::Left, ASTTableJoin::Strictness::Any, Map>
    {
        static void addFound(const typename Map::SegmentType::HashTable::const_iterator & it, size_t num_columns_to_add, MutableColumns & added_columns,
            size_t /*i*/, IColumn::Filter * /*filter*/, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/,
            const std::vector<size_t> & right_indexes)
        {
            for (size_t j = 0; j < num_columns_to_add; ++j)
                added_columns[j]->insertFrom(*it->second.block->getByPosition(right_indexes[j]).column.get(), it->second.row_num);
        }

        static void addNotFound(size_t num_columns_to_add, MutableColumns & added_columns,
            size_t /*i*/, IColumn::Filter * /*filter*/, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/)
        {
            for (size_t j = 0; j < num_columns_to_add; ++j)
                added_columns[j]->insertDefault();
        }
    };

    template <typename Map>
    struct Adder<ASTTableJoin::Kind::Inner, ASTTableJoin::Strictness::Any, Map>
    {
        static void addFound(const typename Map::SegmentType::HashTable::const_iterator & it, size_t num_columns_to_add, MutableColumns & added_columns,
            size_t i, IColumn::Filter * filter, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/,
            const std::vector<size_t> & right_indexes)
        {
            (*filter)[i] = 1;

            for (size_t j = 0; j < num_columns_to_add; ++j)
                added_columns[j]->insertFrom(*it->second.block->getByPosition(right_indexes[j]).column.get(), it->second.row_num);
        }

        static void addNotFound(size_t /*num_columns_to_add*/, MutableColumns & /*added_columns*/,
            size_t i, IColumn::Filter * filter, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/)
        {
            (*filter)[i] = 0;
        }
    };

    template <typename Map>
    struct Adder<ASTTableJoin::Kind::Anti, ASTTableJoin::Strictness::Any, Map>
    {
        static void addFound(const typename Map::SegmentType::HashTable::const_iterator & /*it*/, size_t /*num_columns_to_add*/, MutableColumns & /*added_columns*/,
                             size_t i, IColumn::Filter * filter, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/,
                             const std::vector<size_t> & /*right_indexes*/)
        {
            (*filter)[i] = 0;
        }

        static void addNotFound(size_t num_columns_to_add, MutableColumns & added_columns,
                                size_t i, IColumn::Filter * filter, IColumn::Offset & /*current_offset*/, IColumn::Offsets * /*offsets*/)
        {
            (*filter)[i] = 1;
            for (size_t j = 0; j < num_columns_to_add; ++j)
                added_columns[j]->insertDefault();
        }
    };

    template <ASTTableJoin::Kind KIND, typename Map>
    struct Adder<KIND, ASTTableJoin::Strictness::All, Map>
    {
        static void addFound(const typename Map::SegmentType::HashTable::const_iterator & it, size_t num_columns_to_add, MutableColumns & added_columns,
            size_t i, IColumn::Filter * filter, IColumn::Offset & current_offset, IColumn::Offsets * offsets,
            const std::vector<size_t> & right_indexes)
        {
            size_t rows_joined = 0;
            for (auto current = &static_cast<const typename Map::mapped_type::Base_t &>(it->second); current != nullptr; current = current->next)
            {
                for (size_t j = 0; j < num_columns_to_add; ++j)
                    added_columns[j]->insertFrom(*current->block->getByPosition(right_indexes[j]).column.get(), current->row_num);

                ++rows_joined;
            }

            current_offset += rows_joined;
            (*offsets)[i] = current_offset;
            if (KIND == ASTTableJoin::Kind::Anti)
                /// anti join with other condition is very special: if the row is matched during probe stage, we can not throw it
                /// away because it might failed in other condition, so we add the matched rows to the result, but set (*filter)[i] = 0
                /// to indicate that the row is matched during probe stage, this will be used in handleOtherConditions
                (*filter)[i] = 0;
        }

        static void addNotFound(size_t num_columns_to_add, MutableColumns & added_columns,
            size_t i, IColumn::Filter * filter, IColumn::Offset & current_offset, IColumn::Offsets * offsets)
        {
            if (KIND == ASTTableJoin::Kind::Inner)
            {
                (*offsets)[i] = current_offset;
            }
            else
            {
                if (KIND == ASTTableJoin::Kind::Anti)
                    (*filter)[i] = 1;
                ++current_offset;
                (*offsets)[i] = current_offset;

                for (size_t j = 0; j < num_columns_to_add; ++j)
                    added_columns[j]->insertDefault();
            }
        }
    };

    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, typename KeyGetter, typename Map, bool has_null_map>
    void NO_INLINE joinBlockImplTypeCase(
        const Map & map, size_t rows, const ColumnRawPtrs & key_columns, const Sizes & key_sizes,
        MutableColumns & added_columns, ConstNullMapPtr null_map, std::unique_ptr<IColumn::Filter> & filter,
        IColumn::Offset & current_offset, std::unique_ptr<IColumn::Offsets> & offsets_to_replicate,
        const std::vector<size_t> & right_indexes, const TiDB::TiDBCollators & collators)
    {
        size_t keys_size = key_columns.size();
        size_t num_columns_to_add = right_indexes.size();

        KeyGetter key_getter(key_columns, collators);
        std::vector<std::string> sort_key_containers;
        sort_key_containers.resize(key_columns.size());

        for (size_t i = 0; i < rows; ++i)
        {
            if (has_null_map && (*null_map)[i])
            {
                Adder<KIND, STRICTNESS, Map>::addNotFound(
                    num_columns_to_add, added_columns, i, filter.get(), current_offset, offsets_to_replicate.get());
            }
            else
            {
                auto key = key_getter.getKey(key_columns, keys_size, i, key_sizes, sort_key_containers);
                size_t segment_index = 0;
                size_t hash_value = 0;
                if (map.getSegmentSize() > 0 && !map.isZero(key))
                {
                    hash_value = map.hash(key);
                    segment_index = hash_value % map.getSegmentSize();
                }
                auto & internalMap = map.getSegmentTable(segment_index);
                /// do not require segment lock because in join, the hash table can not be changed in probe stage.
                typename Map::SegmentType::HashTable::const_iterator it = map.getSegmentSize() > 0 ? internalMap.find(key, hash_value) : internalMap.find(key);

                if (it != internalMap.end())
                {
                    it->second.setUsed();
                    Adder<KIND, STRICTNESS, Map>::addFound(
                        it, num_columns_to_add, added_columns, i, filter.get(), current_offset, offsets_to_replicate.get(), right_indexes);
                }
                else
                    Adder<KIND, STRICTNESS, Map>::addNotFound(
                        num_columns_to_add, added_columns, i, filter.get(), current_offset, offsets_to_replicate.get());
            }
        }
    }

    template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, typename KeyGetter, typename Map>
    void joinBlockImplType(
        const Map & map, size_t rows, const ColumnRawPtrs & key_columns, const Sizes & key_sizes,
        MutableColumns & added_columns, ConstNullMapPtr null_map, std::unique_ptr<IColumn::Filter> & filter,
        IColumn::Offset & current_offset, std::unique_ptr<IColumn::Offsets> & offsets_to_replicate,
        const std::vector<size_t> & right_indexes, const TiDB::TiDBCollators & collators)
    {
        if (null_map)
            joinBlockImplTypeCase<KIND, STRICTNESS, KeyGetter, Map, true>(
                map, rows, key_columns, key_sizes, added_columns, null_map, filter,
                current_offset, offsets_to_replicate, right_indexes, collators);
        else
            joinBlockImplTypeCase<KIND, STRICTNESS, KeyGetter, Map, false>(
                map, rows, key_columns, key_sizes, added_columns, null_map, filter,
                current_offset, offsets_to_replicate, right_indexes, collators);
    }
}

/**
 * handle other join conditions
 * Join Kind/Strictness               ALL               ANY
 *     INNER                    TiDB inner join    TiDB semi join
 *     LEFT                     TiDB left join     should not happen
 *     RIGHT                    should not happen  should not happen
 *     ANTI                     should not happen  TiDB anti semi join
 * @param block
 * @param offsets_to_replicate
 * @param left_table_columns
 * @param right_table_columns
 */
void Join::handleOtherConditions(Block & block, std::unique_ptr<IColumn::Filter> & anti_filter, std::unique_ptr<IColumn::Offsets> & offsets_to_replicate, const std::vector<size_t> & right_table_columns) const
{
    other_condition_ptr->execute(block);
    const ColumnVector<UInt8> * filter_column = nullptr;
    ColumnVector<UInt8> * mutable_nested_column = nullptr;
    if (block.getByName(other_filter_column).column->isColumnNullable())
    {
        auto * nullable_column = checkAndGetColumn<ColumnNullable>(block.getByName(other_filter_column).column.get());
        mutable_nested_column = static_cast<ColumnVector<UInt8> *>(nullable_column->getNestedColumnPtr()->assumeMutable().get());
        auto & mutable_nested_column_data = mutable_nested_column->getData();
        for (size_t i = 0; i < nullable_column->size(); i++)
        {
            if (nullable_column->isNullAt(i))
                mutable_nested_column_data[i] = 0;
        }
        filter_column = mutable_nested_column;
    }
    else
    {
        filter_column = checkAndGetColumn<ColumnVector<UInt8>>(block.getByName(other_filter_column).column.get());
    }
    auto & filter = filter_column->getData();

    if (kind == ASTTableJoin::Kind::Inner && original_strictness == ASTTableJoin::Strictness::All)
    {
        /// inner join, just use other_filter_column to filter result
        for (size_t i = 0; i < block.columns(); i++)
        {
            block.safeGetByPosition(i).column = block.safeGetByPosition(i).column->filter(filter, -1);
        }
        return;
    }

    ColumnUInt8::Container row_filter;
    row_filter.resize(filter.size());
    size_t prev_offset = 0;
    for (size_t i = 0; i < offsets_to_replicate->size(); i++)
    {
        size_t start = prev_offset;
        size_t end = (*offsets_to_replicate)[i];
        bool has_row_kept = false;
        for (size_t x = start; x < end; x++)
        {
            if (original_strictness == ASTTableJoin::Strictness::Any)
            {
                /// for semi/anti join, at most one row is kept
                row_filter[x] = !has_row_kept && filter[x];
            }
            else
            {
                /// kind = Anti && strictness = ALL should not happens
                row_filter[x] = filter[x];
            }
            if (row_filter[x])
                has_row_kept = true;
        }
        /// for outer join, at least one row must be kept
        if (kind == ASTTableJoin::Kind::Left && !has_row_kept)
            row_filter[start] = 1;
        if (kind == ASTTableJoin::Kind::Anti)
        {
            if ((*anti_filter)[i])
                /// for anti join, if the equal join condition is not matched, it always need to be selected
                row_filter[start] = 1;
            else
            {
                if (has_row_kept)
                {
                    /// has_row_kept = true means at least one row is joined, in
                    /// case of anti join, this row should not be returned
                    for (size_t index = start; index < end; index++)
                        row_filter[index] = 0;
                }
                else
                {
                    row_filter[start] = 1;
                }
            }
        }
        prev_offset = end;
    }
    if (kind == ASTTableJoin::Kind::Left)
    {
        /// for left join, convert right column to null if not joined
        for (size_t i = 0; i < right_table_columns.size(); i++)
        {
            auto & column = block.getByPosition(right_table_columns[i]);
            if (!column.column->isColumnNullable())
            {
                throw Exception("Should not reach here, the right table column for left join must be nullable");
            }
            auto current_column = column.column;
            auto result_column = (*std::move(current_column)).mutate();
            static_cast<ColumnNullable &>(*result_column).applyNegatedNullMap(*filter_column);
            column.column = std::move(result_column);
        }
        for (size_t i = 0; i < block.columns(); i++)
            block.getByPosition(i).column = block.getByPosition(i).column->filter(row_filter, -1);
        return;
    }
    if (kind == ASTTableJoin::Kind::Inner || kind == ASTTableJoin::Kind::Anti)
    {
        /// for semi/anti join, filter out not matched rows
        for (size_t i = 0; i < block.columns(); i++)
            block.getByPosition(i).column = block.getByPosition(i).column->filter(row_filter, -1);
        return;
    }
    throw Exception("Logical error: unknown combination of JOIN", ErrorCodes::LOGICAL_ERROR);
}

template <ASTTableJoin::Kind KIND, ASTTableJoin::Strictness STRICTNESS, typename Maps>
void Join::joinBlockImpl(Block & block, const Maps & maps) const
{
    size_t keys_size = key_names_left.size();
    ColumnRawPtrs key_columns(keys_size);

    /// Rare case, when keys are constant. To avoid code bloat, simply materialize them.
    Columns materialized_columns;

    /// Memoize key columns to work with.
    for (size_t i = 0; i < keys_size; ++i)
    {
        key_columns[i] = block.getByName(key_names_left[i]).column.get();

        if (ColumnPtr converted = key_columns[i]->convertToFullColumnIfConst())
        {
            materialized_columns.emplace_back(converted);
            key_columns[i] = materialized_columns.back().get();
        }
    }

    /// Keys with NULL value in any column won't join to anything.
    ColumnPtr null_map_holder;
    ConstNullMapPtr null_map{};
    extractNestedColumnsAndNullMap(key_columns, null_map_holder, null_map);
    /// reuse null_map to record the filtered rows, the rows contains NULL or does not
    /// match the join filter won't join to anything
    recordFilteredRows(block, left_filter_column, null_map_holder, null_map);

    size_t existing_columns = block.columns();

    /** If you use FULL or RIGHT JOIN, then the columns from the "left" table must be materialized.
      * Because if they are constants, then in the "not joined" rows, they may have different values
      *  - default values, which can differ from the values of these constants.
      */
    if (getFullness(kind))
    {
        for (size_t i = 0; i < existing_columns; ++i)
        {
            auto & col = block.getByPosition(i).column;

            if (ColumnPtr converted = col->convertToFullColumnIfConst())
                col = converted;

            /// If use_nulls, convert left columns (except keys) to Nullable.
            if (use_nulls)
            {
                if (std::end(key_names_left) == std::find(key_names_left.begin(), key_names_left.end(), block.getByPosition(i).name))
                    convertColumnToNullable(block.getByPosition(i));
            }
        }
    }

    /** For LEFT/INNER JOIN, the saved blocks do not contain keys.
      * For FULL/RIGHT JOIN, the saved blocks contain keys;
      *  but they will not be used at this stage of joining (and will be in `AdderNonJoined`), and they need to be skipped.
      */
    size_t num_columns_to_skip = 0;
    if (getFullness(kind))
        num_columns_to_skip = keys_size;

    /// Add new columns to the block.
    size_t num_columns_to_add = sample_block_with_columns_to_add.columns();
    MutableColumns added_columns;
    added_columns.reserve(num_columns_to_add);

    std::vector<size_t> right_table_column_indexes;
    for (size_t i = 0; i < num_columns_to_add; i++)
    {
        right_table_column_indexes.push_back(i + existing_columns);
    }

    std::vector<size_t> right_indexes;
    right_indexes.reserve(num_columns_to_add);

    for (size_t i = 0; i < num_columns_to_add; ++i)
    {
        const ColumnWithTypeAndName & src_column = sample_block_with_columns_to_add.safeGetByPosition(i);

        /// Don't insert column if it's in left block.
        if (!block.has(src_column.name))
        {
            added_columns.push_back(src_column.column->cloneEmpty());
            added_columns.back()->reserve(src_column.column->size());
            right_indexes.push_back(num_columns_to_skip + i);
        }
    }

    size_t rows = block.rows();

    /// Used with ANY INNER JOIN
    std::unique_ptr<IColumn::Filter> filter;

    if (((kind == ASTTableJoin::Kind::Inner || kind == ASTTableJoin::Kind::Right) && strictness == ASTTableJoin::Strictness::Any)
    || kind == ASTTableJoin::Kind::Anti)
        filter = std::make_unique<IColumn::Filter>(rows);

    /// Used with ALL ... JOIN
    IColumn::Offset current_offset = 0;
    std::unique_ptr<IColumn::Offsets> offsets_to_replicate;

    if (strictness == ASTTableJoin::Strictness::All)
        offsets_to_replicate = std::make_unique<IColumn::Offsets>(rows);

    switch (type)
    {
    #define M(TYPE) \
        case Join::Type::TYPE: \
            joinBlockImplType<KIND, STRICTNESS, typename KeyGetterForType<Join::Type::TYPE>::Type>(\
                *maps.TYPE, rows, key_columns, key_sizes, added_columns, null_map, \
                filter, current_offset, offsets_to_replicate, right_indexes, collators); \
            break;
        APPLY_FOR_JOIN_VARIANTS(M)
    #undef M

        default:
            throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
    }

    for (size_t i = 0; i < num_columns_to_add; ++i)
    {
        const ColumnWithTypeAndName & sample_col = sample_block_with_columns_to_add.getByPosition(i);
        block.insert(ColumnWithTypeAndName(std::move(added_columns[i]), sample_col.type, sample_col.name));
    }

    /// If ANY INNER | RIGHT JOIN - filter all the columns except the new ones.
    if (filter && !(kind == ASTTableJoin::Kind::Anti && strictness == ASTTableJoin::Strictness::All))
        for (size_t i = 0; i < existing_columns; ++i)
            block.safeGetByPosition(i).column = block.safeGetByPosition(i).column->filter(*filter, -1);

    /// If ALL ... JOIN - we replicate all the columns except the new ones.
    if (offsets_to_replicate)
        for (size_t i = 0; i < existing_columns; ++i)
            block.safeGetByPosition(i).column = block.safeGetByPosition(i).column->replicate(*offsets_to_replicate);

    /// handle other conditions
    if (!other_filter_column.empty())
    {
        if (!offsets_to_replicate)
            throw Exception("Should not reach here, the strictness of join with other condition must be ALL");
        handleOtherConditions(block, filter, offsets_to_replicate, right_table_column_indexes);
    }
}


void Join::joinBlockImplCross(Block & block) const
{
    /// Add new columns to the block.
    size_t num_existing_columns = block.columns();
    size_t num_columns_to_add = sample_block_with_columns_to_add.columns();

    size_t rows_left = block.rows();

    ColumnRawPtrs src_left_columns(num_existing_columns);
    MutableColumns dst_columns(num_existing_columns + num_columns_to_add);

    for (size_t i = 0; i < num_existing_columns; ++i)
    {
        src_left_columns[i] = block.getByPosition(i).column.get();
        dst_columns[i] = src_left_columns[i]->cloneEmpty();
    }

    for (size_t i = 0; i < num_columns_to_add; ++i)
    {
        const ColumnWithTypeAndName & src_column = sample_block_with_columns_to_add.getByPosition(i);
        dst_columns[num_existing_columns + i] = src_column.column->cloneEmpty();
        block.insert(src_column);
    }

    /// NOTE It would be better to use `reserve`, as well as `replicate` methods to duplicate the values of the left block.

    for (size_t i = 0; i < rows_left; ++i)
    {
        for (const Block & block_right : blocks)
        {
            size_t rows_right = block_right.rows();

            for (size_t col_num = 0; col_num < num_existing_columns; ++col_num)
                for (size_t j = 0; j < rows_right; ++j)
                    dst_columns[col_num]->insertFrom(*src_left_columns[col_num], i);

            for (size_t col_num = 0; col_num < num_columns_to_add; ++col_num)
            {
                const IColumn * column_right = block_right.getByPosition(col_num).column.get();

                for (size_t j = 0; j < rows_right; ++j)
                    dst_columns[num_existing_columns + col_num]->insertFrom(*column_right, j);
            }
        }
    }

    block = block.cloneWithColumns(std::move(dst_columns));
}


void Join::checkTypesOfKeys(const Block & block_left, const Block & block_right) const
{
    size_t keys_size = key_names_left.size();

    for (size_t i = 0; i < keys_size; ++i)
    {
        /// Compare up to Nullability.

        DataTypePtr left_type = removeNullable(block_left.getByName(key_names_left[i]).type);
        DataTypePtr right_type = removeNullable(block_right.getByName(key_names_right[i]).type);

        if (!left_type->equals(*right_type))
            throw Exception("Type mismatch of columns to JOIN by: "
                + key_names_left[i] + " " + left_type->getName() + " at left, "
                + key_names_right[i] + " " + right_type->getName() + " at right",
                ErrorCodes::TYPE_MISMATCH);
    }
}


void Join::joinBlock(Block & block) const
{
//    std::cerr << "joinBlock: " << block.dumpStructure() << "\n";

    // ck will use this function to generate header, that's why here is a check.
    std::unique_lock lk(build_table_mutex);

    build_table_cv.wait(lk, [&](){ return have_finish_build; });

    std::shared_lock lock(rwlock);

    checkTypesOfKeys(block, sample_block_with_keys);

    if (kind == ASTTableJoin::Kind::Left && strictness == ASTTableJoin::Strictness::Any)
        joinBlockImpl<ASTTableJoin::Kind::Left, ASTTableJoin::Strictness::Any>(block, maps_any);
    else if (kind == ASTTableJoin::Kind::Inner && strictness == ASTTableJoin::Strictness::Any)
        joinBlockImpl<ASTTableJoin::Kind::Inner, ASTTableJoin::Strictness::Any>(block, maps_any);
    else if (kind == ASTTableJoin::Kind::Left && strictness == ASTTableJoin::Strictness::All)
        joinBlockImpl<ASTTableJoin::Kind::Left, ASTTableJoin::Strictness::All>(block, maps_all);
    else if (kind == ASTTableJoin::Kind::Inner && strictness == ASTTableJoin::Strictness::All)
        joinBlockImpl<ASTTableJoin::Kind::Inner, ASTTableJoin::Strictness::All>(block, maps_all);
    else if (kind == ASTTableJoin::Kind::Full && strictness == ASTTableJoin::Strictness::Any)
        joinBlockImpl<ASTTableJoin::Kind::Left, ASTTableJoin::Strictness::Any>(block, maps_any_full);
    else if (kind == ASTTableJoin::Kind::Right && strictness == ASTTableJoin::Strictness::Any)
        joinBlockImpl<ASTTableJoin::Kind::Inner, ASTTableJoin::Strictness::Any>(block, maps_any_full);
    else if (kind == ASTTableJoin::Kind::Full && strictness == ASTTableJoin::Strictness::All)
        joinBlockImpl<ASTTableJoin::Kind::Left, ASTTableJoin::Strictness::All>(block, maps_all_full);
    else if (kind == ASTTableJoin::Kind::Right && strictness == ASTTableJoin::Strictness::All)
        joinBlockImpl<ASTTableJoin::Kind::Inner, ASTTableJoin::Strictness::All>(block, maps_all_full);
    else if (kind == ASTTableJoin::Kind::Anti && strictness == ASTTableJoin::Strictness::Any)
        joinBlockImpl<ASTTableJoin::Kind::Anti, ASTTableJoin::Strictness::Any>(block, maps_any);
    else if (kind == ASTTableJoin::Kind::Anti && strictness == ASTTableJoin::Strictness::All)
        joinBlockImpl<ASTTableJoin::Kind::Anti, ASTTableJoin::Strictness::All>(block, maps_all);
    else if (kind == ASTTableJoin::Kind::Cross)
        joinBlockImplCross(block);
    else
        throw Exception("Logical error: unknown combination of JOIN", ErrorCodes::LOGICAL_ERROR);
}


void Join::joinTotals(Block & block) const
{
    Block totals_without_keys = totals;

    if (totals_without_keys)
    {
        for (const auto & name : key_names_right)
            totals_without_keys.erase(totals_without_keys.getPositionByName(name));

        for (size_t i = 0; i < totals_without_keys.columns(); ++i)
            block.insert(totals_without_keys.safeGetByPosition(i));
    }
    else
    {
        /// We will join empty `totals` - from one row with the default values.

        for (size_t i = 0; i < sample_block_with_columns_to_add.columns(); ++i)
        {
            const auto & col = sample_block_with_columns_to_add.getByPosition(i);
            block.insert({
                col.type->createColumnConstWithDefaultValue(1)->convertToFullColumnIfConst(),
                col.type,
                col.name});
        }
    }
}


template <ASTTableJoin::Strictness STRICTNESS, typename Mapped>
struct AdderNonJoined;

template <typename Mapped>
struct AdderNonJoined<ASTTableJoin::Strictness::Any, Mapped>
{
    static size_t add(const Mapped & mapped,
        size_t num_columns_left, MutableColumns & columns_left,
        size_t num_columns_right, MutableColumns & columns_right)
    {
        for (size_t j = 0; j < num_columns_left; ++j)
            columns_left[j]->insertDefault();

        for (size_t j = 0; j < num_columns_right; ++j)
            columns_right[j]->insertFrom(*mapped.block->getByPosition(j).column.get(), mapped.row_num);
        return 1;
    }
};

template <typename Mapped>
struct AdderNonJoined<ASTTableJoin::Strictness::All, Mapped>
{
    static size_t add(const Mapped & mapped,
        size_t num_columns_left, MutableColumns & columns_left,
        size_t num_columns_right, MutableColumns & columns_right)
    {
        size_t rows_added = 0;
        for (auto current = &static_cast<const typename Mapped::Base_t &>(mapped); current != nullptr; current = current->next)
        {
            for (size_t j = 0; j < num_columns_left; ++j)
                columns_left[j]->insertDefault();

            for (size_t j = 0; j < num_columns_right; ++j)
                columns_right[j]->insertFrom(*current->block->getByPosition(j).column.get(), current->row_num);
            rows_added++;
        }
        return rows_added;
    }
};


/// Stream from not joined earlier rows of the right table.
class NonJoinedBlockInputStream : public IProfilingBlockInputStream
{
public:
    NonJoinedBlockInputStream(const Join & parent_, const Block & left_sample_block, size_t max_block_size_)
        : parent(parent_), max_block_size(max_block_size_), add_not_mapped_rows(true)
    {
        /** left_sample_block contains keys and "left" columns.
          * result_sample_block - keys, "left" columns, and "right" columns.
          */

        size_t num_keys = parent.key_names_left.size();
        size_t num_columns_left = left_sample_block.columns() - num_keys;
        size_t num_columns_right = parent.sample_block_with_columns_to_add.columns();

        result_sample_block = materializeBlock(left_sample_block);

        /// Add columns from the right-side table to the block.
        for (size_t i = 0; i < num_columns_right; ++i)
        {
            const ColumnWithTypeAndName & src_column = parent.sample_block_with_columns_to_add.getByPosition(i);
            result_sample_block.insert(src_column.cloneEmpty());
        }

        column_indices_left.reserve(num_columns_left);
        column_indices_keys_and_right.reserve(num_keys + num_columns_right);
        std::vector<bool> is_key_column_in_left_block(num_keys + num_columns_left, false);

        for (const std::string & key : parent.key_names_left)
        {
            size_t key_pos = left_sample_block.getPositionByName(key);
            is_key_column_in_left_block[key_pos] = true;
            /// Here we establish the mapping between key columns of the left- and right-side tables.
            /// key_pos index is inserted in the position corresponding to key column in parent.blocks
            /// (saved blocks of the right-side table) and points to the same key column
            /// in the left_sample_block and thus in the result_sample_block.
            column_indices_keys_and_right.push_back(key_pos);
        }

        for (size_t i = 0; i < num_keys + num_columns_left; ++i)
        {
            if (!is_key_column_in_left_block[i])
                column_indices_left.push_back(i);
        }

        for (size_t i = 0; i < num_columns_right; ++i)
            column_indices_keys_and_right.push_back(num_keys + num_columns_left + i);

        /// If use_nulls, convert left columns to Nullable.
        if (parent.use_nulls)
        {
            for (size_t i = 0; i < num_columns_left; ++i)
            {
                convertColumnToNullable(result_sample_block.getByPosition(column_indices_left[i]));
            }
        }

        columns_left.resize(num_columns_left);
        columns_keys_and_right.resize(num_keys + num_columns_right);
    }

    String getName() const override { return "NonJoined"; }

    Block getHeader() const override { return result_sample_block; };


protected:
    Block readImpl() override
    {
        if (parent.blocks.empty())
            return Block();

        if (add_not_mapped_rows)
        {
            current_not_mapped_row = parent.rows_not_inserted_to_map.next;
            add_not_mapped_rows = false;
        }

        if (parent.strictness == ASTTableJoin::Strictness::Any)
            return createBlock<ASTTableJoin::Strictness::Any>(parent.maps_any_full);
        else if (parent.strictness == ASTTableJoin::Strictness::All)
            return createBlock<ASTTableJoin::Strictness::All>(parent.maps_all_full);
        else
            throw Exception("Logical error: unknown JOIN strictness (must be ANY or ALL)", ErrorCodes::LOGICAL_ERROR);
    }

private:
    const Join & parent;
    size_t max_block_size;
    bool add_not_mapped_rows;

    Block result_sample_block;
    /// Indices of columns in result_sample_block that come from the left-side table (except key columns).
    ColumnNumbers column_indices_left;
    /// Indices of key columns in result_sample_block or columns that come from the right-side table.
    /// Order is significant: it is the same as the order of columns in the blocks of the right-side table that are saved in parent.blocks.
    ColumnNumbers column_indices_keys_and_right;
    /// Columns of the current output block corresponding to column_indices_left.
    MutableColumns columns_left;
    /// Columns of the current output block corresponding to column_indices_keys_and_right.
    MutableColumns columns_keys_and_right;

    std::unique_ptr<void, std::function<void(void *)>> position;    /// type erasure
    size_t current_segment;
    Join::RowRefList * current_not_mapped_row;


    template <ASTTableJoin::Strictness STRICTNESS, typename Maps>
    Block createBlock(const Maps & maps)
    {
        size_t num_columns_left = column_indices_left.size();
        size_t num_columns_right = column_indices_keys_and_right.size();

        for (size_t i = 0; i < num_columns_left; ++i)
        {
            const auto & src_col = result_sample_block.safeGetByPosition(column_indices_left[i]);
            columns_left[i] = src_col.type->createColumn();
        }

        for (size_t i = 0; i < num_columns_right; ++i)
        {
            const auto & src_col = result_sample_block.safeGetByPosition(column_indices_keys_and_right[i]);
            columns_keys_and_right[i] = src_col.type->createColumn();
        }

        size_t rows_added = 0;

        switch (parent.type)
        {
        #define M(TYPE) \
            case Join::Type::TYPE: \
                rows_added = fillColumns<STRICTNESS>(*maps.TYPE, num_columns_left, columns_left, num_columns_right, columns_keys_and_right); \
                break;
            APPLY_FOR_JOIN_VARIANTS(M)
        #undef M

            default:
                throw Exception("Unknown JOIN keys variant.", ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
        }

        if (!rows_added)
            return {};

        Block res = result_sample_block.cloneEmpty();
        for (size_t i = 0; i < num_columns_left; ++i)
            res.getByPosition(column_indices_left[i]).column = std::move(columns_left[i]);
        for (size_t i = 0; i < num_columns_right; ++i)
            res.getByPosition(column_indices_keys_and_right[i]).column = std::move(columns_keys_and_right[i]);

        return res;
    }


    template <ASTTableJoin::Strictness STRICTNESS, typename Map>
    size_t fillColumns(const Map & map,
        size_t num_columns_left, MutableColumns & columns_left,
        size_t num_columns_right, MutableColumns & columns_right)
    {
        size_t rows_added = 0;
        while (current_not_mapped_row != nullptr)
        {
            rows_added++;
            for (size_t j = 0; j < num_columns_left; ++j)
                columns_left[j]->insertDefault();

            for (size_t j = 0; j < num_columns_right; ++j)
                columns_right[j]->insertFrom(*current_not_mapped_row->block->getByPosition(j).column.get(),
                                             current_not_mapped_row->row_num);

            current_not_mapped_row = current_not_mapped_row->next;
            if (rows_added == max_block_size)
            {
                return rows_added;
            }
        }


        if (!position)
        {
            current_segment = 0;
            position = decltype(position)(
                    static_cast<void *>(new typename Map::SegmentType::HashTable::const_iterator(map.getSegmentTable(current_segment).begin())),
                    [](void *ptr) { delete reinterpret_cast<typename Map::SegmentType::HashTable::const_iterator *>(ptr); });
        }

        /// use pointer instead of reference because `it` need to be re-assigned latter
        auto it = reinterpret_cast<typename Map::SegmentType::HashTable::const_iterator *>(position.get());
        auto end = map.getSegmentTable(current_segment).end();

        for (; *it != end || current_segment < map.getSegmentSize() - 1; ++(*it))
        {
            if (*it == end)
            {
                // move to next internal hash table
                do {
                    current_segment++;
                    position = decltype(position)(
                            static_cast<void *>(new typename Map::SegmentType::HashTable::const_iterator(
                                    map.getSegmentTable(current_segment).begin())),
                            [](void *ptr) { delete reinterpret_cast<typename Map::SegmentType::HashTable::const_iterator *>(ptr); });
                    it = reinterpret_cast<typename Map::SegmentType::HashTable::const_iterator *>(position.get());
                    end = map.getSegmentTable(current_segment).end();
                } while (*it == end && current_segment < map.getSegmentSize() - 1);
                if (*it == end)
                    break;
            }
            if ((*it)->second.getUsed())
                continue;

            rows_added += AdderNonJoined<STRICTNESS, typename Map::mapped_type>::add((*it)->second, num_columns_left, columns_left, num_columns_right, columns_right);

            if (rows_added >= max_block_size)
            {
                ++(*it);
                break;
            }
        }
        return rows_added;
    }
};


BlockInputStreamPtr Join::createStreamWithNonJoinedRows(const Block & left_sample_block, size_t max_block_size) const
{
    return std::make_shared<NonJoinedBlockInputStream>(*this, left_sample_block, max_block_size);
}


}
