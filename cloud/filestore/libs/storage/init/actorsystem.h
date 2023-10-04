#pragma once

#include "public.h"

#include <cloud/filestore/libs/diagnostics/metrics/public.h>
#include <cloud/filestore/libs/diagnostics/public.h>
#include <cloud/filestore/libs/diagnostics/user_counter.h>
#include <cloud/filestore/libs/storage/core/public.h>

#include <cloud/storage/core/libs/common/public.h>
#include <cloud/storage/core/libs/diagnostics/public.h>
#include <cloud/storage/core/libs/kikimr/public.h>

#include <library/cpp/actors/core/defs.h>

namespace NCloud::NFileStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

struct TActorSystemArgs
{
    std::shared_ptr<NKikimr::TModuleFactories> ModuleFactories;

    ui32 NodeId = 0;
    NActors::TScopeId ScopeId;
    NKikimrConfig::TAppConfigPtr AppConfig;

    TStorageConfigPtr StorageConfig;
    IAsyncLoggerPtr AsyncLogger;
    IProfileLogPtr ProfileLog;
    ITraceSerializerPtr TraceSerializer;
    NMetrics::IMetricsServicePtr Metrics;

    std::shared_ptr<NCloud::NStorage::NUserStats::TUserCounterSupplier> UserCounters;

    NCloud::NStorage::ICgroupStatsFetcherPtr CgroupStatsFetcher;
};

////////////////////////////////////////////////////////////////////////////////

IActorSystemPtr CreateActorSystem(const TActorSystemArgs& args);

}   // namespace NCloud::NFileStore::NStorage