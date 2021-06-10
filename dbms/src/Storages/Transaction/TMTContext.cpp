#include <Common/DNSCache.h>
#include <Flash/Mpp/MPPHandler.h>
#include <Interpreters/Context.h>
#include <Server/RaftConfigParser.h>
#include <Storages/Transaction/BackgroundService.h>
#include <Storages/Transaction/KVStore.h>
#include <Storages/Transaction/RegionExecutionResult.h>
#include <Storages/Transaction/RegionRangeKeys.h>
#include <Storages/Transaction/SchemaSyncer.h>
#include <Storages/Transaction/TMTContext.h>
#include <Storages/Transaction/TiDBSchemaSyncer.h>
#include <pingcap/pd/MockPDClient.h>

namespace DB
{
// default batch-read-index timeout is 10_000ms.
extern const uint64_t DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS = 10 * 1000;

TMTContext::TMTContext(Context & context_, const TiFlashRaftConfig & raft_config, const pingcap::ClusterConfig & cluster_config)
    : context(context_),
      kvstore(std::make_shared<KVStore>(context)),
      region_table(context),
      background_service(nullptr),
      cluster(raft_config.pd_addrs.size() == 0 ? std::make_shared<pingcap::kv::Cluster>()
                                               : std::make_shared<pingcap::kv::Cluster>(raft_config.pd_addrs, cluster_config)),
      ignore_databases(raft_config.ignore_databases),
      schema_syncer(raft_config.pd_addrs.size() == 0
              ? std::static_pointer_cast<SchemaSyncer>(std::make_shared<TiDBSchemaSyncer</*mock*/ true>>(cluster))
              : std::static_pointer_cast<SchemaSyncer>(std::make_shared<TiDBSchemaSyncer</*mock*/ false>>(cluster))),
      mpp_task_manager(std::make_shared<MPPTaskManager>()),
      engine(raft_config.engine),
      disable_bg_flush(raft_config.disable_bg_flush),
      replica_read_max_thread(1),
      batch_read_index_timeout_ms(DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS)
{}

void TMTContext::restore(const TiFlashRaftProxyHelper * proxy_helper)
{
    kvstore->restore(proxy_helper);
    region_table.restore();
    {
        Timestamp gc_safe_point = UINT64_MAX;
        if (auto pd_client = getPDClient(); !pd_client->isMock())
        {
            gc_safe_point
                = PDClientHelper::getGCSafePointWithRetry(pd_client, false, context.getSettingsRef().safe_point_update_interval_seconds);
        }
        kvstore->traverseRegions([&](const RegionID, const RegionPtr & region) { region->tryCompactionFilter(gc_safe_point); });
    }
    initialized = true;

    background_service = std::make_unique<BackgroundService>(*this);
}

KVStorePtr & TMTContext::getKVStore() { return kvstore; }

const KVStorePtr & TMTContext::getKVStore() const { return kvstore; }

ManagedStorages & TMTContext::getStorages() { return storages; }

const ManagedStorages & TMTContext::getStorages() const { return storages; }

RegionTable & TMTContext::getRegionTable() { return region_table; }

const RegionTable & TMTContext::getRegionTable() const { return region_table; }

BackgroundService & TMTContext::getBackgroundService() { return *background_service; }

const BackgroundService & TMTContext::getBackgroundService() const { return *background_service; }


Context & TMTContext::getContext() { return context; }

bool TMTContext::isInitialized() const { return initialized; }

SchemaSyncerPtr TMTContext::getSchemaSyncer() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return schema_syncer;
}

void TMTContext::setSchemaSyncer(SchemaSyncerPtr rhs)
{
    std::lock_guard<std::mutex> lock(mutex);
    schema_syncer = rhs;
}

pingcap::pd::ClientPtr TMTContext::getPDClient() const { return cluster->pd_client; }

MPPTaskManagerPtr TMTContext::getMPPTaskManager() { return mpp_task_manager; }

const std::unordered_set<std::string> & TMTContext::getIgnoreDatabases() const { return ignore_databases; }

void TMTContext::reloadConfig(const Poco::Util::AbstractConfiguration & config)
{
    static constexpr const char * TABLE_OVERLAP_THRESHOLD = "flash.overlap_threshold";
    static constexpr const char * COMPACT_LOG_MIN_PERIOD = "flash.compact_log_min_period";
    static constexpr const char * COMPACT_LOG_MIN_ROWS = "flash.compact_log_min_rows";
    static constexpr const char * COMPACT_LOG_MIN_BYTES = "flash.compact_log_min_bytes";
    static constexpr const char * REPLICA_READ_MAX_THREAD = "flash.replica_read_max_thread";
    static constexpr const char * BATCH_READ_INDEX_TIMEOUT_MS = "flash.batch_read_index_timeout_ms";


    getRegionTable().setTableCheckerThreshold(config.getDouble(TABLE_OVERLAP_THRESHOLD, 0.6));
    // default config about compact-log: period 120s, rows 40k, bytes 32MB.
    getKVStore()->setRegionCompactLogConfig(std::max(config.getUInt64(COMPACT_LOG_MIN_PERIOD, 120), 1),
        std::max(config.getUInt64(COMPACT_LOG_MIN_ROWS, 40 * 1024), 1),
        std::max(config.getUInt64(COMPACT_LOG_MIN_BYTES, 32 * 1024 * 1024), 1));
    {
        replica_read_max_thread = std::max(config.getUInt64(REPLICA_READ_MAX_THREAD, 1), 1);
        batch_read_index_timeout_ms = config.getUInt64(BATCH_READ_INDEX_TIMEOUT_MS, DEFAULT_BATCH_READ_INDEX_TIMEOUT_MS);
        LOG_INFO(&Logger::get(__FUNCTION__),
            "read-index max thread num: " << replicaReadMaxThread() << ", timeout: " << batchReadIndexTimeout() << "ms");
    }
}

const std::atomic_bool & TMTContext::getTerminated() const { return terminated; }

void TMTContext::setTerminated()
{
    terminated = true;
    // notify all region to stop learner read.
    kvstore->traverseRegions([](const RegionID, const RegionPtr & region) { region->notifyApplied(); });
}

UInt64 TMTContext::replicaReadMaxThread() const { return replica_read_max_thread.load(std::memory_order::memory_order_relaxed); }
UInt64 TMTContext::batchReadIndexTimeout() const { return batch_read_index_timeout_ms.load(std::memory_order::memory_order_relaxed); }

} // namespace DB
