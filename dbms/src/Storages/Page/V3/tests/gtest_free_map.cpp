#include <Common/Exception.h>
#include <Storages/Page/V3/spacemap/RBTree.h>
#include <Storages/Page/V3/spacemap/SpaceMap.h>
#include <Storages/Page/V3/spacemap/SpaceMapRBTree.h>
#include <Storages/Page/V3/spacemap/SpaceMapSTDMap.h>
#include <Storages/tests/TiFlashStorageTestBasic.h>
#include <TestUtils/TiFlashTestBasic.h>

#include <map>


namespace DB::PS::V3::tests
{
::testing::AssertionResult MapIterCompare(
    const char * lhs_expr,
    const char * rhs_expr,
    const std::map<int, int>::const_iterator lhs,
    const std::pair<int, int> rhs)
{
    if (lhs->first == rhs.first && lhs->second == rhs.second)
        return ::testing::AssertionSuccess();
    return ::testing::internal::EqFailure(
        lhs_expr,
        rhs_expr,
        fmt::format("{{{},{}}}", lhs->first, lhs->second),
        fmt::format("{{{}, {}}}", rhs.first, rhs.second),
        false);
}

#define ASSERT_ITER_EQ(iter, val) ASSERT_PRED_FORMAT2(MapIterCompare, iter, val)

TEST(STDMapUtil, FindLessEqual)
{
    std::map<int, int> m0{};
    ASSERT_EQ(details::findLessEQ(m0, 1), m0.end());

    std::map<int, int> m1{{1, 1}, {2, 2}, {3, 3}, {6, 6}};
    ASSERT_EQ(details::findLessEQ(m1, 0), m1.end());
    ASSERT_ITER_EQ(details::findLessEQ(m1, 1), std::make_pair(1, 1));
    ASSERT_ITER_EQ(details::findLessEQ(m1, 2), std::make_pair(2, 2));
    ASSERT_ITER_EQ(details::findLessEQ(m1, 3), std::make_pair(3, 3));
    for (int x = 4; x < 20; ++x)
    {
        if (x < 6)
            ASSERT_ITER_EQ(details::findLessEQ(m1, x), std::make_pair(3, 3));
        else
            ASSERT_ITER_EQ(details::findLessEQ(m1, x), std::make_pair(6, 6));
    }
}

struct Range
{
    size_t start;
    size_t end;
};

class SpaceMapTest
    : public testing::TestWithParam<SpaceMap::SpaceMapType>
{
public:
    SpaceMapTest()
        : test_type(GetParam())
    {}
    SpaceMap::SpaceMapType test_type;

protected:
    static SpaceMap::CheckerFunc
    genChecker(const Range * ranges, size_t range_size)
    {
        return [ranges, range_size](size_t idx, UInt64 start, UInt64 end) -> bool {
            return idx < range_size && ranges[idx].start == start && ranges[idx].end == end;
        };
    };
};

TEST_P(SpaceMapTest, InitAndDestory)
{
    SpaceMapPtr smap = SpaceMap::createSpaceMap(test_type, 0, 100);

    smap->logStats();
}


TEST_P(SpaceMapTest, MarkUnmark)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);

    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));

    ASSERT_TRUE(smap->markUsed(50, 1));
    ASSERT_FALSE(smap->markUsed(50, 1));

    ASSERT_TRUE(smap->isMarkUsed(50, 1));
    ASSERT_FALSE(smap->isMarkUsed(51, 1));

    Range ranges1[] = {{.start = 0,
                        .end = 50},
                       {.start = 51,
                        .end = 100}};

    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));

    ASSERT_TRUE(smap->markFree(50, 1));
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
    ASSERT_FALSE(smap->isMarkUsed(50, 1));
}

TEST_P(SpaceMapTest, MarkmarkFree)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);

    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
    ASSERT_FALSE(smap->isMarkUsed(1, 99));

    // call `isMarkUsed` with invalid length
    ASSERT_THROW({ smap->isMarkUsed(0, 1000); }, DB::Exception);

    ASSERT_TRUE(smap->markUsed(50, 10));
    ASSERT_FALSE(smap->markUsed(50, 10));
    ASSERT_FALSE(smap->markUsed(50, 9));
    ASSERT_FALSE(smap->markUsed(55, 5));
    Range ranges1[] = {{.start = 0,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));
    ASSERT_TRUE(smap->isMarkUsed(51, 5));

    ASSERT_TRUE(smap->markFree(50, 5));
    Range ranges2[] = {{.start = 0,
                        .end = 55},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));
    ASSERT_TRUE(smap->markFree(55, 5));
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
}

TEST_P(SpaceMapTest, MarkmarkFree2)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);

    ASSERT_TRUE(smap->markUsed(50, 20));
    ASSERT_FALSE(smap->markUsed(50, 1));
    ASSERT_FALSE(smap->markUsed(50, 20));
    ASSERT_FALSE(smap->markUsed(55, 15));
    Range ranges1[] = {{.start = 0,
                        .end = 50},
                       {.start = 70,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));
    ASSERT_TRUE(smap->isMarkUsed(51, 5));

    ASSERT_TRUE(smap->markFree(50, 5));
    Range ranges2[] = {{.start = 0,
                        .end = 55},
                       {.start = 70,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));

    ASSERT_TRUE(smap->markFree(60, 5));
    Range ranges3[] = {{.start = 0,
                        .end = 55},
                       {.start = 60,
                        .end = 65},
                       {.start = 70,
                        .end = 100}};

    ASSERT_TRUE(smap->check(genChecker(ranges3, 3), 3));

    ASSERT_TRUE(smap->markFree(65, 5));
    Range ranges4[] = {{.start = 0,
                        .end = 55},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges4, 2), 2));

    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->markFree(55, 5));
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
}

TEST_P(SpaceMapTest, TestMargins)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);

    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
    ASSERT_TRUE(smap->markUsed(50, 10));

    Range ranges1[] = {{.start = 0,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));

    ASSERT_TRUE(smap->isMarkUsed(50, 5));
    ASSERT_FALSE(smap->isMarkUsed(60, 1));

    // Test for two near markUsed
    ASSERT_TRUE(smap->markUsed(60, 10));
    Range ranges2[] = {{.start = 0,
                        .end = 50},
                       {.start = 70,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));

    ASSERT_TRUE(smap->markUsed(49, 1));
    Range ranges3[] = {{.start = 0,
                        .end = 49},
                       {.start = 70,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges3, 2), 2));

    ASSERT_TRUE(smap->markFree(49, 1));
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));
}

TEST_P(SpaceMapTest, TestMargins2)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);
    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
    ASSERT_TRUE(smap->markUsed(50, 10));

    // total in marked used range
    ASSERT_FALSE(smap->markUsed(50, 1));
    ASSERT_FALSE(smap->markUsed(59, 1));
    ASSERT_FALSE(smap->markUsed(55, 1));
    ASSERT_FALSE(smap->markUsed(55, 5));
    ASSERT_FALSE(smap->markUsed(50, 5));

    // Right margin in marked used space
    // Left margin contain freed space
    ASSERT_FALSE(smap->markUsed(45, 10));

    // Left margin in marked used space
    // Right margin contain freed space
    ASSERT_FALSE(smap->markUsed(55, 15));

    // Left margin align with marked used space left margin
    // But right margin contain freed space
    ASSERT_FALSE(smap->markUsed(50, 20));

    // Right margin align with marked used space right margin
    // But left margin contain freed space
    ASSERT_FALSE(smap->markUsed(40, 20));

    // Left margin in freed space
    // Right margin in freed space
    // But used space in the middle
    ASSERT_FALSE(smap->markUsed(40, 30));


    Range ranges1[] = {{.start = 0,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));

    ASSERT_TRUE(smap->markFree(50, 1));

    // Mark a space which contain a sub freed space.
    ASSERT_FALSE(smap->markFree(50, 2));
    ASSERT_FALSE(smap->markFree(50, 5));
    ASSERT_TRUE(smap->markFree(59, 1));

    // Left margin in marked used space
    // Right margin contain freed space
    ASSERT_FALSE(smap->markFree(58, 10));

    // Right margin in marked used space
    // Left margin contain freed space
    ASSERT_FALSE(smap->markFree(49, 10));
    smap->logStats();
    // Left margin align with marked used space left margin
    // But right margin contain freed space
    ASSERT_FALSE(smap->markFree(51, 20));
    smap->logStats();
    // Right margin align with marked used space right margin
    // But left margin contain freed space
    ASSERT_FALSE(smap->markUsed(40, 19));

    // Left margin in freed space
    // Right margin in freed space
    // But used space in the middle
    ASSERT_FALSE(smap->markUsed(40, 30));


    Range ranges2[] = {{.start = 0,
                        .end = 51},
                       {.start = 59,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));
}

TEST_P(SpaceMapTest, TestSearch)
{
    auto smap = SpaceMap::createSpaceMap(test_type, 0, 100);
    UInt64 offset;
    UInt64 max_cap;
    Range ranges[] = {{.start = 0,
                       .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges, 1), 1));
    ASSERT_TRUE(smap->markUsed(50, 10));

    std::tie(offset, max_cap) = smap->searchInsertOffset(20);
    ASSERT_EQ(offset, 0);
    ASSERT_EQ(max_cap, 40);

    Range ranges1[] = {{.start = 20,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges1, 2), 2));

    // We can't use `markFree` to restore the map status
    // It won't update `max_cap`/`max_offset` which inside space map
    // So just recreate a space map
    smap = SpaceMap::createSpaceMap(test_type, 0, 100);
    ASSERT_TRUE(smap->markUsed(50, 10));

    std::tie(offset, max_cap) = smap->searchInsertOffset(5);
    ASSERT_EQ(offset, 0);
    ASSERT_EQ(max_cap, 45);

    Range ranges2[] = {{.start = 5,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges2, 2), 2));

    // Test margin
    smap = SpaceMap::createSpaceMap(test_type, 0, 100);
    ASSERT_TRUE(smap->markUsed(50, 10));
    std::tie(offset, max_cap) = smap->searchInsertOffset(50);
    ASSERT_EQ(offset, 0);
    ASSERT_EQ(max_cap, 40);

    Range ranges3[] = {{.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges3, 1), 1));

    // Test invalid Size
    smap = SpaceMap::createSpaceMap(test_type, 0, 100);
    ASSERT_TRUE(smap->markUsed(50, 10));
    std::tie(offset, max_cap) = smap->searchInsertOffset(100);
    ASSERT_EQ(offset, UINT64_MAX);
    ASSERT_EQ(max_cap, 50);

    // No changed
    Range ranges4[] = {{.start = 0,
                        .end = 50},
                       {.start = 60,
                        .end = 100}};
    ASSERT_TRUE(smap->check(genChecker(ranges4, 2), 2));
}

INSTANTIATE_TEST_CASE_P(
    Type,
    SpaceMapTest,
    testing::Values(
        SpaceMap::SMAP64_RBTREE,
        SpaceMap::SMAP64_STD_MAP));

} // namespace DB::PS::V3::tests
