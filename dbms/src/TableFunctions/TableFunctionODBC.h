#pragma once

#if Poco_SQLODBC_FOUND || Poco_DataODBC_FOUND
#include <Common/config.h>

#include <TableFunctions/ITableFunction.h>


namespace DB
{
/* odbc (odbc connect string, table) - creates a temporary StorageODBC.
 * The structure of the table is taken from the mysql query "SELECT * FROM table WHERE 1=0".
 * If there is no such table, an exception is thrown.
 */
class TableFunctionODBC : public ITableFunction
{
public:
    static constexpr auto name = "odbc";
    std::string getName() const override
    {
        return name;
    }
private:
    StoragePtr executeImpl(const ASTPtr & ast_function, const Context & context) const override;
};
}

#endif
