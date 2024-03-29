#include <Common/config.h>

#include <Storages/registerStorages.h>
#include <Storages/StorageFactory.h>


namespace DB
{

void registerStorageLog(StorageFactory & factory);
void registerStorageTinyLog(StorageFactory & factory);
void registerStorageDeltaMerge(StorageFactory & factory);
void registerStorageStripeLog(StorageFactory & factory);
void registerStorageMergeTree(StorageFactory & factory);
void registerStorageNull(StorageFactory & factory);
void registerStorageMerge(StorageFactory & factory);
void registerStorageBuffer(StorageFactory & factory);
void registerStorageDistributed(StorageFactory & factory);
void registerStorageMemory(StorageFactory & factory);
void registerStorageFile(StorageFactory & factory);
void registerStorageDictionary(StorageFactory & factory);
void registerStorageSet(StorageFactory & factory);
void registerStorageJoin(StorageFactory & factory);
void registerStorageView(StorageFactory & factory);
void registerStorageMaterializedView(StorageFactory & factory);

// A buggy table just for test
void registerStorageDebugging(StorageFactory & factory);

#if Poco_SQLODBC_FOUND || Poco_DataODBC_FOUND
void registerStorageODBC(StorageFactory & factory);
#endif

#if USE_MYSQL
void registerStorageMySQL(StorageFactory & factory);
#endif

#if USE_RDKAFKA
void registerStorageKafka(StorageFactory & factory);
#endif


void registerStorages()
{
    auto & factory = StorageFactory::instance();

    registerStorageLog(factory);
    registerStorageTinyLog(factory);
    registerStorageDeltaMerge(factory);
    registerStorageStripeLog(factory);
    registerStorageMergeTree(factory);
    registerStorageNull(factory);
    registerStorageMerge(factory);
    registerStorageBuffer(factory);
    registerStorageDistributed(factory);
    registerStorageMemory(factory);
    registerStorageDebugging(factory);
    registerStorageFile(factory);
    registerStorageDictionary(factory);
    registerStorageSet(factory);
    registerStorageJoin(factory);
    registerStorageView(factory);
    registerStorageMaterializedView(factory);

    #if Poco_SQLODBC_FOUND || Poco_DataODBC_FOUND
    registerStorageODBC(factory);
    #endif

    #if USE_MYSQL
    registerStorageMySQL(factory);
    #endif

    #if USE_RDKAFKA
    registerStorageKafka(factory);
    #endif
}

}
