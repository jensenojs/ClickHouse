#include <Databases/PostgreSQL/DatabasePostgreSQLReplica.h>

#if USE_LIBPQXX

#include <Storages/PostgreSQL/PostgreSQLConnection.h>
#include <Storages/PostgreSQL/StoragePostgreSQLReplica.h>

#include <DataTypes/DataTypeNullable.h>
#include <DataTypes/DataTypeArray.h>
#include <Databases/DatabaseOrdinary.h>
#include <Databases/DatabaseAtomic.h>
#include <Storages/StoragePostgreSQL.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTCreateQuery.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ParserCreateQuery.h>
#include <Parsers/parseQuery.h>
#include <Parsers/queryToString.h>
#include <Common/escapeForFileName.h>
#include <Poco/DirectoryIterator.h>
#include <Poco/File.h>
#include <Databases/PostgreSQL/fetchPostgreSQLTableStructure.h>
#include <Common/Macros.h>
#include <common/logger_useful.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int NOT_IMPLEMENTED;
    extern const int UNKNOWN_TABLE;
    extern const int TABLE_IS_DROPPED;
    extern const int TABLE_ALREADY_EXISTS;
}


static const auto METADATA_SUFFIX = ".postgresql_replica_metadata";


template<>
DatabasePostgreSQLReplica<DatabaseOrdinary>::DatabasePostgreSQLReplica(
        const Context & context,
        const String & metadata_path_,
        UUID /* uuid */,
        const ASTStorage * database_engine_define_,
        const String & database_name_,
        const String & postgres_database_name,
        PostgreSQLConnectionPtr connection_,
        std::unique_ptr<PostgreSQLReplicaSettings> settings_)
    : DatabaseOrdinary(
            database_name_, metadata_path_, "data/" + escapeForFileName(database_name_) + "/",
            "DatabasePostgreSQLReplica<Ordinary> (" + database_name_ + ")", context)
    , log(&Poco::Logger::get("PostgreSQLReplicaDatabaseEngine"))
    , global_context(context.getGlobalContext())
    , metadata_path(metadata_path_)
    , database_engine_define(database_engine_define_->clone())
    , database_name(database_name_)
    , remote_database_name(postgres_database_name)
    , connection(std::move(connection_))
    , settings(std::move(settings_))
{
}


template<>
DatabasePostgreSQLReplica<DatabaseAtomic>::DatabasePostgreSQLReplica(
        const Context & context,
        const String & metadata_path_,
        UUID uuid,
        const ASTStorage * database_engine_define_,
        const String & database_name_,
        const String & postgres_database_name,
        PostgreSQLConnectionPtr connection_,
        std::unique_ptr<PostgreSQLReplicaSettings> settings_)
    : DatabaseAtomic(database_name_, metadata_path_, uuid, "DatabasePostgreSQLReplica<Atomic> (" + database_name_ + ")", context)
    , global_context(context.getGlobalContext())
    , metadata_path(metadata_path_)
    , database_engine_define(database_engine_define_->clone())
    , remote_database_name(postgres_database_name)
    , connection(std::move(connection_))
    , settings(std::move(settings_))
{
}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::startSynchronization()
{
    replication_handler = std::make_unique<PostgreSQLReplicationHandler>(
            remote_database_name,
            connection->conn_str(),
            metadata_path + METADATA_SUFFIX,
            std::make_shared<Context>(global_context),
            settings->postgresql_max_block_size.changed
                     ? settings->postgresql_max_block_size.value
                     : (global_context.getSettingsRef().max_insert_block_size.value),
            global_context.getMacros()->expand(settings->postgresql_tables_list.value));

    std::unordered_set<std::string> tables_to_replicate = replication_handler->fetchRequiredTables(connection->conn());

    for (const auto & table_name : tables_to_replicate)
    {
        auto storage = getStorage(table_name);

        if (storage)
        {
            replication_handler->addStorage(table_name, storage.get()->template as<StoragePostgreSQLReplica>());
            tables[table_name] = storage;
        }
    }

    LOG_TRACE(log, "Loaded {} tables. Starting synchronization", tables.size());
    replication_handler->startup();
}


template<typename Base>
StoragePtr DatabasePostgreSQLReplica<Base>::getStorage(const String & name)
{
    auto storage = tryGetTable(name, global_context);

    if (storage)
        return storage;

    return StoragePostgreSQLReplica::create(StorageID(database_name, name), StoragePtr{}, global_context);
}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::shutdown()
{
    if (replication_handler)
        replication_handler->shutdown();
}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::loadStoredObjects(
        Context & context, bool has_force_restore_data_flag, bool force_attach)
{
    Base::loadStoredObjects(context, has_force_restore_data_flag, force_attach);
    startSynchronization();

}


template<typename Base>
StoragePtr DatabasePostgreSQLReplica<Base>::tryGetTable(const String & name, const Context & context) const
{
    if (context.hasQueryContext())
    {
        auto storage_set = context.getQueryContext().getQueryFactoriesInfo().storages;
        if (storage_set.find("ReplacingMergeTree") != storage_set.end())
        {
            return Base::tryGetTable(name, context);
        }
    }

    auto table = tables.find(name);
    if (table != tables.end() && table->second->as<StoragePostgreSQLReplica>()->isNestedLoaded())
        return table->second;

    return StoragePtr{};

}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::createTable(const Context & context, const String & name, const StoragePtr & table, const ASTPtr & query)
{
    if (context.hasQueryContext())
    {
        auto storage_set = context.getQueryContext().getQueryFactoriesInfo().storages;
        if (storage_set.find("ReplacingMergeTree") != storage_set.end())
        {
            Base::createTable(context, name, table, query);
            return;
        }
    }

    LOG_WARNING(log, "Create table query allowed only for ReplacingMergeTree engine and from synchronization thread");
}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::dropTable(const Context & context, const String & name, bool no_delay)
{
    Base::dropTable(context, name, no_delay);
}


template<typename Base>
void DatabasePostgreSQLReplica<Base>::drop(const Context & context)
{
    if (replication_handler)
    {
        replication_handler->shutdown();
        replication_handler->shutdownFinal();
    }

    /// Remove metadata
    Poco::File metadata(Base::getMetadataPath() + METADATA_SUFFIX);

    if (metadata.exists())
        metadata.remove(false);

    Base::drop(context);
}


template<typename Base>
DatabaseTablesIteratorPtr DatabasePostgreSQLReplica<Base>::getTablesIterator(
        const Context & /* context */, const DatabaseOnDisk::FilterByNameFunction & /* filter_by_table_name */)
{
    Tables nested_tables;
    for (const auto & [table_name, storage] : tables)
    {
        auto nested_storage = storage->as<StoragePostgreSQLReplica>()->tryGetNested();

        if (nested_storage)
            nested_tables[table_name] = nested_storage;
    }

    return std::make_unique<DatabaseTablesSnapshotIterator>(nested_tables, database_name);
}

}

#endif
