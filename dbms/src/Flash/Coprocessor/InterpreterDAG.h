#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <kvproto/coprocessor.pb.h>
#include <tipb/select.pb.h>
#pragma GCC diagnostic pop

#include <DataStreams/BlockIO.h>
#include <Flash/Coprocessor/DAGExpressionAnalyzer.h>
#include <Flash/Coprocessor/DAGQueryBlockInterpreter.h>
#include <Flash/Coprocessor/DAGQuerySource.h>
#include <Flash/Coprocessor/DAGUtils.h>
#include <Interpreters/AggregateDescription.h>
#include <Interpreters/ExpressionActions.h>
#include <Interpreters/IInterpreter.h>
#include <Storages/RegionQueryInfo.h>
#include <Storages/Transaction/Collator.h>
#include <Storages/Transaction/RegionException.h>
#include <Storages/Transaction/TMTStorages.h>

namespace DB
{

class Context;
class Region;
using RegionPtr = std::shared_ptr<Region>;

/** build ch plan from dag request: dag executors -> ch plan
  */
class InterpreterDAG : public IInterpreter
{
public:
    InterpreterDAG(Context & context_, const DAGQuerySource & dag_);

    ~InterpreterDAG() = default;

    BlockIO execute();

private:
    BlockInputStreams executeQueryBlock(DAGQueryBlock & query_block, std::vector<SubqueriesForSets> & subqueriesForSets);
    RegionException::RegionReadStatus getRegionReadStatus(const RegionPtr & current_region);

private:
    Context & context;

    const DAGQuerySource & dag;

    /// How many streams we ask for storage to produce, and in how many threads we will do further processing.
    size_t max_streams = 1;

    const bool keep_session_timezone_info;

    Poco::Logger * log;
};
} // namespace DB
