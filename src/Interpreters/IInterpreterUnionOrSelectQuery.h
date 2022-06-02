#pragma once

#include <Interpreters/Context.h>
#include <Interpreters/IInterpreter.h>
#include <Interpreters/SelectQueryOptions.h>
#include <Parsers/IAST_fwd.h>
#include <DataTypes/DataTypesNumber.h>

namespace DB
{

class IInterpreterUnionOrSelectQuery : public IInterpreter
{
public:
    IInterpreterUnionOrSelectQuery(const ASTPtr & query_ptr_, ContextPtr context_, const SelectQueryOptions & options_)
        : IInterpreterUnionOrSelectQuery(query_ptr_, Context::createCopy(context_), options_)
    {
    }

    IInterpreterUnionOrSelectQuery(const ASTPtr & query_ptr_, ContextMutablePtr context_, const SelectQueryOptions & options_)
        : query_ptr(query_ptr_)
        , context(context_)
        , options(options_)
        , max_streams(context->getSettingsRef().max_threads)
    {
        if (options.shard_num)
            context->addSpecialScalar(
                "_shard_num",
                Block{{DataTypeUInt32().createColumnConst(1, *options.shard_num), std::make_shared<DataTypeUInt32>(), "_shard_num"}});
        if (options.shard_count)
            context->addSpecialScalar(
                "_shard_count",
                Block{{DataTypeUInt32().createColumnConst(1, *options.shard_count), std::make_shared<DataTypeUInt32>(), "_shard_count"}});
    }

    virtual void buildQueryPlan(QueryPlan & query_plan) = 0;
    QueryPipelineBuilder buildQueryPipeline();

    virtual void ignoreWithTotals() = 0;

    virtual ~IInterpreterUnionOrSelectQuery() override = default;

    Block getSampleBlock() { return result_header; }

    size_t getMaxStreams() const { return max_streams; }

    void extendQueryLogElemImpl(QueryLogElement & elem, const ASTPtr &, ContextPtr) const override;

    /// Returns whether the query uses the view source from the Context
    /// The view source is a virtual storage that currently only materialized views use to replace the source table
    /// with the incoming block only
    /// This flag is useful to know for how long we can cache scalars generated by this query: If it doesn't use the virtual storage
    /// then we can cache the scalars forever (for any query that doesn't use the virtual storage either), but if it does use the virtual
    /// storage then we can only keep the scalar result around while we are working with that source block
    /// You can find more details about this under ExecuteScalarSubqueriesMatcher::visit
    bool usesViewSource() const { return uses_view_source; }

    /// Add limits and quotas to query pipeline based on internal context and options.
    virtual void addLimitsAndQuotas(QueryPipeline & pipeline) const;
    static void addLimitsAndQuotas(QueryPipeline & pipeline, const Context & context, const SelectQueryOptions & options);

    static StorageLimits getStorageLimits(const Context & context, const SelectQueryOptions & options);

    virtual bool hasRemoteStorage() const = 0;

    void addStorageLimits(const StorageLimitsList & limits);

protected:
    ASTPtr query_ptr;
    ContextMutablePtr context;
    Block result_header;
    SelectQueryOptions options;
    StorageLimitsList storage_limits;

    size_t max_streams = 1;
    bool settings_limit_offset_needed = false;
    bool settings_limit_offset_done = false;
    bool uses_view_source = false;
};
}
