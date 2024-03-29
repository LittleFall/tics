#include <Interpreters/Context.h>
#include <Interpreters/Settings.h>
#include <Storages/DeltaMerge/StoragePool.h>
#include <Storages/PathPool.h>

namespace DB
{
namespace DM
{
static constexpr UInt64 STORAGE_LOG  = 1;
static constexpr UInt64 STORAGE_DATA = 2;
static constexpr UInt64 STORAGE_META = 3;

PageStorage::Config extractConfig(const Settings & settings, UInt64 subtype)
{
    PageStorage::Config config;
    config.open_file_max_idle_time = Seconds(settings.dt_open_file_max_idle_seconds);
    switch (subtype)
    {
    case STORAGE_LOG:
        config.num_write_slots = settings.dt_storage_pool_log_write_slots;
        break;
    case STORAGE_DATA:
        config.num_write_slots = settings.dt_storage_pool_data_write_slots;
        break;
    case STORAGE_META:
        config.num_write_slots = settings.dt_storage_pool_meta_write_slots;
        break;
    default:
        throw Exception("Unknown subtype in extractConfig: " + DB::toString(subtype));
    }
    return config;
}

StoragePool::StoragePool(const String & name, StoragePathPool & path_pool, const Context & global_ctx, const Settings & settings)
    : // The iops and bandwidth in log_storage are relatively high, use multi-disks if possible
      log_storage(name + ".log",
                  path_pool.getPSDiskDelegatorMulti("log"),
                  extractConfig(settings, STORAGE_LOG),
                  global_ctx.getFileProvider(),
                  global_ctx.getTiFlashMetrics()),
      // The iops in data_storage is low, only use the first disk for storing data
      data_storage(name + ".data",
                   path_pool.getPSDiskDelegatorSingle("data"),
                   extractConfig(settings, STORAGE_DATA),
                   global_ctx.getFileProvider(),
                   global_ctx.getTiFlashMetrics()),
      // The iops in meta_storage is relatively high, use multi-disks if possible
      meta_storage(name + ".meta",
                   path_pool.getPSDiskDelegatorMulti("meta"),
                   extractConfig(settings, STORAGE_META),
                   global_ctx.getFileProvider(),
                   global_ctx.getTiFlashMetrics()),
      max_log_page_id(0),
      max_data_page_id(0),
      max_meta_page_id(0)
{
}

void StoragePool::restore()
{
    log_storage.restore();
    data_storage.restore();
    meta_storage.restore();

    max_log_page_id  = log_storage.getMaxId();
    max_data_page_id = data_storage.getMaxId();
    max_meta_page_id = meta_storage.getMaxId();
}

void StoragePool::drop()
{
    meta_storage.drop();
    data_storage.drop();
    log_storage.drop();
}

bool StoragePool::gc(const Seconds & try_gc_period)
{
    {
        std::lock_guard<std::mutex> lock(mutex);

        Timepoint now = Clock::now();
        if (now < (last_try_gc_time.load() + try_gc_period))
            return false;

        last_try_gc_time = now;
    }

    bool ok = false;

    ok |= meta_storage.gc();
    ok |= data_storage.gc();
    ok |= log_storage.gc();

    return ok;
}

} // namespace DM
} // namespace DB
