#include <Storages/DeltaMerge/tests/workload/Utils.h>
#include <fmt/ranges.h>

namespace DB::DM::tests
{
std::string localTime()
{
    time_t t = ::time(nullptr);
    std::stringstream ss;
    struct tm local_tm;
    ss << std::put_time(localtime_r(&t, &local_tm), "%Y%m%d_%X");
    return ss.str();
}

std::string fieldToString(const DataTypePtr & data_type, const Field & f)
{
    std::string family_name = data_type->getFamilyName();
    if (family_name == "Int8" || family_name == "Int16" || family_name == "Int32" || family_name == "Int64" || family_name == "Enum8" || family_name == "Enum16")
    {
        auto i = f.safeGet<Int64>();
        return std::to_string(i);
    }
    else if (family_name == "UInt8" || family_name == "UInt16" || family_name == "UInt32" || family_name == "UInt64")
    {
        auto i = f.safeGet<UInt64>();
        return std::to_string(i);
    }
    else if (family_name == "Float32" || family_name == "Float64")
    {
        auto i = f.safeGet<Float64>();
        return std::to_string(i);
    }
    else if (family_name == "String")
    {
        return f.toString();
    }
    else if (family_name == "MyDateTime")
    {
        auto i = f.safeGet<UInt64>();
        return MyDateTime(i).toString(6);
    }
    else if (family_name == "MyDate")
    {
        auto i = f.safeGet<UInt64>();
        return MyDate(i).toString();
    }
    else if (family_name == "Decimal")
    {
        auto t = f.getType();
        if (t == Field::Types::Which::Decimal32)
        {
            auto i = f.get<Decimal32>();
            auto scale = dynamic_cast<const DataTypeDecimal32 *>(data_type.get())->getScale();
            return i.toString(scale);
        }
        else if (t == Field::Types::Which::Decimal64)
        {
            auto i = f.get<Decimal64>();
            auto scale = dynamic_cast<const DataTypeDecimal64 *>(data_type.get())->getScale();
            return i.toString(scale);
        }
        else if (t == Field::Types::Which::Decimal128)
        {
            auto i = f.get<Decimal128>();
            auto scale = dynamic_cast<const DataTypeDecimal128 *>(data_type.get())->getScale();
            return i.toString(scale);
        }
        else if (t == Field::Types::Which::Decimal256)
        {
            auto i = f.get<Decimal256>();
            auto scale = dynamic_cast<const DataTypeDecimal256 *>(data_type.get())->getScale();
            return i.toString(scale);
        }
    }
    return f.toString();
}

std::vector<std::string> colToVec(const DataTypePtr & data_type, const ColumnPtr & col)
{
    std::vector<std::string> v;
    for (size_t i = 0; i < col->size(); i++)
    {
        const Field & f = (*col)[i];
        v.push_back(fieldToString(data_type, f));
    }
    return v;
}

std::string blockToString(const Block & block)
{
    std::string s = "id name type values\n";
    auto & cols = block.getColumnsWithTypeAndName();
    for (auto & col : cols)
    {
        s += fmt::format("{} {} {} {}\n", col.column_id, col.name, col.type->getFamilyName(), colToVec(col.type, col.column));
    }
    return s;
}

} // namespace DB::DM::tests