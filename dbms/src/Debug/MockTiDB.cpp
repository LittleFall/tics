#include <DataTypes/DataTypeDecimal.h>
#include <DataTypes/DataTypeDate.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeNothing.h>
#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeSet.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeString.h>

#include <Functions/FunctionHelpers.h>

#include <Debug/MockTiDB.h>

namespace DB
{

using ColumnInfo = TiDB::ColumnInfo;
using TableInfo = TiDB::TableInfo;
using PartitionInfo = TiDB::PartitionInfo;
using PartitionDefinition = TiDB::PartitionDefinition;
using Table = MockTiDB::Table;
using TablePtr = MockTiDB::TablePtr;

Table::Table(const String & database_name_, const String & table_name_, TableInfo && table_info_)
    : table_info(std::move(table_info_)), database_name(database_name_), table_name(table_name_)
{
}

String MockTiDB::getSchemaJson(TableID table_id)
{
    std::lock_guard lock(tables_mutex);

    auto it = tables_by_id.find(table_id);
    if (it == tables_by_id.end())
    {
        throw Exception("Mock TiDB table with ID " + toString(table_id) + " does not exists", ErrorCodes::UNKNOWN_TABLE);
    }

    return it->second->table_info.serialize(false);
}

void MockTiDB::dropTable(const String & database_name, const String & table_name)
{
    std::lock_guard lock(tables_mutex);

    String qualified_name = database_name + "." + table_name;
    auto it_by_name = tables_by_name.find(qualified_name);
    if (it_by_name == tables_by_name.end())
        return;

    const auto & table = it_by_name->second;
    if (table->isPartitionTable())
    {
        for (const auto & partition : table->table_info.partition.definitions)
        {
            tables_by_id.erase(partition.id);
        }
    }
    tables_by_id.erase(table->id());


    tables_by_name.erase(it_by_name);
}

TableID MockTiDB::newTable(const String & database_name, const String & table_name, const ColumnsDescription & columns)
{
    std::lock_guard lock(tables_mutex);

    String qualified_name = database_name + "." + table_name;
    if (tables_by_name.find(qualified_name) != tables_by_name.end())
    {
        throw Exception("Mock TiDB table " + qualified_name + " already exists", ErrorCodes::TABLE_ALREADY_EXISTS);
    }

    TableInfo table_info;

    if (databases.find(database_name) != databases.end())
    {
        databases.emplace(database_name, databases.size());
    }
    table_info.db_id = databases[database_name];
    table_info.db_name = database_name;
    table_info.id = static_cast<TableID>(tables_by_id.size()) + MaxSystemTableID + 1;
    table_info.name = table_name;

    int i = 0;
    for (auto & column : columns.getAllPhysical())
    {
        ColumnInfo column_info;
        column_info.id = (i++);
        column_info.name = column.name;
        const IDataType *nested_type = column.type.get();
        if (!column.type->isNullable())
        {
            column_info.setNotNullFlag();
        }
        else
        {
            auto nullable_type = checkAndGetDataType<DataTypeNullable>(nested_type);
            nested_type = nullable_type->getNestedType().get();
        }
        if (column.type->isUnsignedInteger())
        {
            column_info.setUnsignedFlag();
        }
        if (checkDataType<DataTypeDecimal>(nested_type))
        {
            auto decimal_type = checkAndGetDataType<DataTypeDecimal>(nested_type);
            column_info.flen = decimal_type->getPrec();
            column_info.decimal = decimal_type->getScale();
        }

#ifdef M
#error "Please undefine macro M first."
#endif
#define M(tt, v, cf, cfu, ct, ctu) \
        if (checkDataType<DataType##ct>(nested_type) || checkDataType<DataType##ctu>(nested_type)) column_info.tp = TiDB::Type##tt; else
        COLUMN_TYPES(M)
#undef M
        throw DB::Exception("Invalid ?", ErrorCodes::LOGICAL_ERROR);

        table_info.columns.emplace_back(column_info);
    }

    table_info.pk_is_handle = false;
    table_info.comment = "Mocked.";

    auto table = std::make_shared<Table>(database_name, table_name, std::move(table_info));
    tables_by_id.emplace(table->table_info.id, table);
    tables_by_name.emplace(database_name + "." + table_name, table);

    return table->table_info.id;
}

TableID MockTiDB::newPartition(const String & database_name, const String & table_name, const String & partition_name)
{
    std::lock_guard lock(tables_mutex);

    TablePtr table = getTableByNameInternal(database_name, table_name);
    TableInfo & table_info = table->table_info;

    const auto & part_def = find_if(table_info.partition.definitions.begin(), table_info.partition.definitions.end(), [& partition_name](PartitionDefinition & part_def) {
        return part_def.name == partition_name;
    });
    if (part_def != table_info.partition.definitions.end())
        throw Exception("Mock TiDB table " + database_name + "." + table_name + " already has partition " + partition_name, ErrorCodes::LOGICAL_ERROR);

    table_info.is_partition_table = true;
    table_info.partition.enable = true;
    table_info.partition.num++;
    TableID partition_id = table_info.id + table_info.partition.num;
    PartitionDefinition partition_def;
    partition_def.id = partition_id;
    partition_def.name = partition_name;
    table_info.partition.definitions.emplace_back(partition_def);

    // Map the same table object with partition ID as key, so mock schema syncer behaves the same as TiDB,
    // i.e. gives the table info by partition ID.
    tables_by_id.emplace(partition_id, table);

    return partition_id;
}

TablePtr MockTiDB::getTableByName(const String & database_name, const String & table_name)
{
    std::lock_guard lock(tables_mutex);

    return getTableByNameInternal(database_name, table_name);
}

TablePtr MockTiDB::getTableByNameInternal(const String & database_name, const String & table_name)
{
    String qualified_name = database_name + "." + table_name;
    auto it = tables_by_name.find(qualified_name);
    if (it == tables_by_name.end())
    {
        throw Exception("Mock TiDB table " + qualified_name + " does not exists", ErrorCodes::UNKNOWN_TABLE);
    }

    return it->second;
}

}
