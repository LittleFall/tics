#pragma once

#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>


namespace DB
{

using Names = std::vector<std::string>;
using NameSet = std::unordered_set<std::string>;
using NameToNameMap = std::unordered_map<std::string, std::string>;

/* TODO: only support a small number of names now
 */
class OrderedNameSet : public Names
{
public:
    bool has(const std::string & name) const
    {
        for (auto it : *this)
            if (it == name)
                return true;
        return false;
    }
};

}
