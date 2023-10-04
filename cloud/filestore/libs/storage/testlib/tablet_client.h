#pragma once

#include "helpers.h"
#include "test_env.h"

#include <cloud/filestore/libs/storage/api/service.h>
#include <cloud/filestore/libs/storage/api/tablet.h>
#include <cloud/filestore/libs/storage/core/config.h>
#include <cloud/filestore/libs/storage/model/channel_data_kind.h>
#include <cloud/filestore/libs/storage/tablet/tablet_private.h>

#include <ydb/core/testlib/actors/test_runtime.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/core/filestore/core/filestore.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NCloud::NFileStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

namespace NImpl {

template <typename T>
concept HasRecord = requires (T v)
{
    {v.Record};
};

template <typename T>
TString DebugDump(const T& v)
{
    if constexpr (HasRecord<T>) {
        return v.Record.ShortDebugString();
    }

    return typeid(T).name();
}

using TEvRemoteHttpInfoRequest = NActors::NMon::TEvRemoteHttpInfo;
using TEvRemoteHttpInfoResponse = NActors::NMon::TEvRemoteHttpInfoRes;

}   // namespace NImpl

////////////////////////////////////////////////////////////////////////////////

struct TPerformanceProfile
{
    bool ThrottlingEnabled = false;

    ui32 MaxReadIops = 0;
    ui32 MaxWriteIops = 0;
    ui32 MaxReadBandwidth = 0;
    ui32 MaxWriteBandwidth = 0;

    ui32 BoostTime = 0;
    ui32 BoostRefillTime = 0;
    ui32 BoostPercentage = 0;

    ui32 MaxPostponedWeight = 0;
    ui32 MaxWriteCostMultiplier = 0;
    ui32 MaxPostponedTime = 0;
    ui32 MaxPostponedCount = 0;

    ui32 BurstPercentage = 0;
    ui32 DefaultPostponedRequestWeight = 0;
};

struct TFileSystemConfig
{
    TString FileSystemId = "test";
    TString CloudId = "test";
    TString FolderId = "test";
    ui32 BlockSize = DefaultBlockSize;
    ui64 BlockCount = DefaultBlockCount;
    ui32 NodeCount = MaxNodes;
    ui32 ChannelCount = DefaultChannelCount;
    ui32 StorageMediaKind = 0;
    TPerformanceProfile PerformanceProfile;
};

////////////////////////////////////////////////////////////////////////////////

const ui32 RangeIdHasherType = 1;

inline ui32 GetMixedRangeIndex(ui64 nodeId, ui32 blockIndex)
{
    static auto hasher = CreateRangeIdHasher(RangeIdHasherType);
    return hasher->Calc(nodeId, blockIndex);
}

////////////////////////////////////////////////////////////////////////////////

class TIndexTabletClient
{
private:
    NKikimr::TTestActorRuntime& Runtime;
    ui32 NodeIdx;
    ui64 TabletId;

    NActors::TActorId Sender;
    NActors::TActorId PipeClient;

    THeaders Headers;

public:
    TIndexTabletClient(
            NKikimr::TTestActorRuntime& runtime,
            ui32 nodeIdx,
            ui64 tabletId,
            const TFileSystemConfig& config = {})
        : Runtime(runtime)
        , NodeIdx(nodeIdx)
        , TabletId(tabletId)
    {
        Sender = Runtime.AllocateEdgeActor(NodeIdx);

        ReconnectPipe();

        WaitReady();
        UpdateConfig(config);
    }

    TIndexTabletClient& WithSessionSeqNo(ui64 seqNo)
    {
        Headers.SessionSeqNo = seqNo;
        return *this;
    }

    void RebootTablet()
    {
        TVector<ui64> tablets = { TabletId };
        auto guard = NKikimr::CreateTabletScheduledEventsGuard(
            tablets,
            Runtime,
            Sender);

        NKikimr::RebootTablet(Runtime, TabletId, Sender, NodeIdx);

        ReconnectPipe();
    }

    template <typename TRequest>
    void SendRequest(std::unique_ptr<TRequest> request, ui64 cookie = 0)
    {
        Runtime.SendToPipe(
            PipeClient,
            Sender,
            request.release(),
            NodeIdx,
            cookie);
    }

    template <typename TResponse>
    auto RecvResponse()
    {
        TAutoPtr<NActors::IEventHandle> handle;
        Runtime.GrabEdgeEventRethrow<TResponse>(handle);
        return std::unique_ptr<TResponse>(handle->Release<TResponse>().Release());
    }

    void RecoverSession()
    {
        CreateSession(Headers.ClientId, Headers.SessionId, TString(), 0, false, true);
    }

    auto InitSession(
        const TString& clientId,
        const TString& sessionId,
        const TString& checkpointId = {},
        ui64 sessionSeqNo = 0,
        bool readOnly = false,
        bool restoreClientSession = false)
    {
        auto response = CreateSession(
            clientId,
            sessionId,
            checkpointId,
            sessionSeqNo,
            readOnly,
            restoreClientSession);
        SetHeaders(clientId, sessionId, sessionSeqNo);

        return response;
    }

    void SetHeaders(const TString& clientId, const TString& sessionId, ui64 sessionSeqNo)
    {
        Headers = { "test", clientId, sessionId, sessionSeqNo};
    }

    template <typename T>
    auto CreateSessionRequest()
    {
        auto request = std::make_unique<T>();
        Headers.Fill(request->Record);
        return request;
    }

    //
    // TEvIndexTablet
    //

    auto CreateWaitReadyRequest()
    {
        return std::make_unique<TEvIndexTablet::TEvWaitReadyRequest>();
    }

    auto CreateGetStorageStatsRequest()
    {
        return std::make_unique<TEvIndexTablet::TEvGetStorageStatsRequest>();
    }

    auto CreateGetFileSystemConfigRequest()
    {
        return std::make_unique<TEvIndexTablet::TEvGetFileSystemConfigRequest>();
    }

    auto CreateCreateSessionRequest(
        const TString& clientId,
        const TString& sessionId,
        const TString& checkpointId = {},
        ui64 mountSeqNo = 0,
        bool readOnly = false,
        bool restoreClientSession = false)
    {
        auto request = std::make_unique<TEvIndexTablet::TEvCreateSessionRequest>();
        request->Record.SetCheckpointId(checkpointId);
        request->Record.SetMountSeqNumber(mountSeqNo);
        request->Record.SetReadOnly(readOnly);
        request->Record.SetRestoreClientSession(restoreClientSession);

        auto* headers = request->Record.MutableHeaders();
        headers->SetClientId(clientId);
        headers->SetSessionId(sessionId);
        return request;
    }

    auto CreateResetSessionRequest(
        const TString& clientId,
        const TString& sessionId,
        const ui64 sessionSeqNo,
        const TString& state)
    {
        auto request = std::make_unique<TEvService::TEvResetSessionRequest>();
        request->Record.SetSessionState(state);

        auto* headers = request->Record.MutableHeaders();
        headers->SetClientId(clientId);
        headers->SetSessionId(sessionId);
        headers->SetSessionSeqNo(sessionSeqNo);
        return request;
    }

    auto CreateResetSessionRequest(const TString& state)
    {
        auto request = std::make_unique<TEvService::TEvResetSessionRequest>();
        request->Record.SetSessionState(state);

        Headers.Fill(request->Record);
        return request;
    }

    auto CreateDestroySessionRequest(ui64 seqNo = 0)
    {
        auto request = std::make_unique<TEvIndexTablet::TEvDestroySessionRequest>();
        Headers.Fill(request->Record);
        if (seqNo) {
            request->Record.MutableHeaders()->SetSessionSeqNo(seqNo);
        }
        return request;
    }

    //
    // TEvIndexTabletPrivate
    //

    auto CreateFlushRequest()
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvFlushRequest>();
    }

    auto CreateFlushBytesRequest()
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvFlushBytesRequest>();
    }

    auto CreateCleanupRequest(ui32 rangeId)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvCleanupRequest>(rangeId);
    }

    auto CreateCompactionRequest(ui32 rangeId, bool filter = false)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvCompactionRequest>(rangeId, filter);
    }

    auto CreateForcedCompactionRequest(TVector<ui32> ranges)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvForcedCompactionRequest>(
            std::move(ranges));
    }

    auto CreateCollectGarbageRequest()
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvCollectGarbageRequest>();
    }

    auto CreateTruncateRangeRequest(ui64 node, ui64 offset, ui32 length)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvTruncateRangeRequest>(
            node,
            TByteRange(offset, length, DefaultBlockSize));
    }

    auto CreateZeroRangeRequest(ui64 node, ui64 offset, ui32 length)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvZeroRangeRequest>(
            node,
            TByteRange(offset, length, DefaultBlockSize));
    }

    auto CreateDeleteGarbageRequest(
        ui64 collectGarbageId,
        TVector<TPartialBlobId> newBlobs,
        TVector<TPartialBlobId> garbageBlobs)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvDeleteGarbageRequest>(
            collectGarbageId,
            std::move(newBlobs),
            std::move(garbageBlobs));
    }

    auto CreateFilterAliveNodesRequest(TStackVec<ui64, 16> nodes)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvFilterAliveNodesRequest>(
            std::move(nodes));
    }

    auto CreateDumpCompactionRangeRequest(ui32 rangeId)
    {
        return std::make_unique<TEvIndexTabletPrivate::TEvDumpCompactionRangeRequest>(
            rangeId);
    }

    //
    // TEvService
    //

    auto CreateCreateNodeRequest(const TCreateNodeArgs& args)
    {
        auto request = CreateSessionRequest<TEvService::TEvCreateNodeRequest>();
        Headers.Fill(request->Record);
        args.Fill(request->Record);
        return request;
    }

    auto CreateUnlinkNodeRequest(ui64 parent, const TString& name, bool unlinkDirectory)
    {
        auto request = CreateSessionRequest<TEvService::TEvUnlinkNodeRequest>();
        request->Record.SetNodeId(parent);
        request->Record.SetName(name);
        request->Record.SetUnlinkDirectory(unlinkDirectory);
        return request;
    }

    auto CreateRenameNodeRequest(
        ui64 parent,
        const TString& name,
        ui64 newparent,
        const TString& newname,
        ui32 flags = 0)
    {
        auto request = CreateSessionRequest<TEvService::TEvRenameNodeRequest>();
        request->Record.SetNodeId(parent);
        request->Record.SetName(name);
        request->Record.SetNewParentId(newparent);
        request->Record.SetNewName(newname);
        request->Record.SetFlags(flags);
        return request;
    }

    auto CreateAccessNodeRequest(ui64 node)
    {
        auto request = CreateSessionRequest<TEvService::TEvAccessNodeRequest>();
        request->Record.SetNodeId(node);
        return request;
    }

    auto CreateReadLinkRequest(ui64 node)
    {
        auto request = CreateSessionRequest<TEvService::TEvReadLinkRequest>();
        request->Record.SetNodeId(node);
        return request;
    }

    auto CreateListNodesRequest(ui64 node)
    {
        auto request = CreateSessionRequest<TEvService::TEvListNodesRequest>();
        request->Record.SetNodeId(node);
        return request;
    }

    auto CreateListNodesRequest(ui64 node, ui32 bytes, TString cookie)
    {
        auto request = CreateSessionRequest<TEvService::TEvListNodesRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetMaxBytes(bytes);
        request->Record.SetCookie(std::move(cookie));
        return request;
    }

    auto CreateGetNodeAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateSessionRequest<TEvService::TEvGetNodeAttrRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetName(name);
        return request;
    }

    auto CreateGetNodeAttrRequest(ui64 node)
    {
        auto request = CreateSessionRequest<TEvService::TEvGetNodeAttrRequest>();
        request->Record.SetNodeId(node);
        return request;
    }

    auto CreateSetNodeAttrRequest(const TSetNodeAttrArgs& args)
    {
        auto request = CreateSessionRequest<TEvService::TEvSetNodeAttrRequest>();
        args.Fill(request->Record);
        return request;
    }

    auto CreateListNodeXAttrRequest(ui64 node)
    {
        auto request = CreateSessionRequest<TEvService::TEvListNodeXAttrRequest>();
        request->Record.SetNodeId(node);
        return request;
    }

    auto CreateGetNodeXAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateSessionRequest<TEvService::TEvGetNodeXAttrRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetName(name);
        return request;
    }

    auto CreateSetNodeXAttrRequest(ui64 node, const TString& name, const TString& value, ui64 flags = 0)
    {
        auto request = CreateSessionRequest<TEvService::TEvSetNodeXAttrRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetName(name);
        request->Record.SetValue(value);
        request->Record.SetFlags(flags);
        return request;
    }

    auto CreateRemoveNodeXAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateSessionRequest<TEvService::TEvRemoveNodeXAttrRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetName(name);
        return request;
    }

    auto CreateCreateHandleRequest(ui64 node, ui32 flags)
    {
        auto request = CreateSessionRequest<TEvService::TEvCreateHandleRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetFlags(flags);
        return request;
    }

    auto CreateCreateHandleRequest(ui64 node, const TString& name, ui32 flags)
    {
        auto request = CreateSessionRequest<TEvService::TEvCreateHandleRequest>();
        request->Record.SetNodeId(node);
        request->Record.SetFlags(flags);
        request->Record.SetName(name);
        return request;
    }

    auto CreateDestroyHandleRequest(ui64 handle)
    {
        auto request = CreateSessionRequest<TEvService::TEvDestroyHandleRequest>();
        request->Record.SetHandle(handle);
        return request;
    }

    auto CreateAllocateDataRequest(ui64 handle, ui64 offset, ui64 length, ui32 flags)
    {
        auto request = CreateSessionRequest<TEvService::TEvAllocateDataRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOffset(offset);
        request->Record.SetLength(length);
        request->Record.SetFlags(flags);

        return request;
    }

    auto CreateWriteDataRequest(ui64 handle, ui64 offset, ui32 len, char fill)
    {
        auto request = CreateSessionRequest<TEvService::TEvWriteDataRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOffset(offset);
        request->Record.SetBuffer(CreateBuffer(len, fill));
        return request;
    }

    auto CreateWriteDataRequest(ui64 handle, ui64 offset, ui32 len, const char* data)
    {
        auto request = CreateSessionRequest<TEvService::TEvWriteDataRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOffset(offset);
        request->Record.MutableBuffer()->assign(data, len);
        return request;
    }

    auto CreateReadDataRequest(ui64 handle, ui64 offset, ui32 len)
    {
        auto request = CreateSessionRequest<TEvService::TEvReadDataRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOffset(offset);
        request->Record.SetLength(len);
        return request;
    }

    auto CreateAcquireLockRequest(
        ui64 handle,
        ui64 owner,
        ui64 offset,
        ui32 len,
        NProto::ELockType type = NProto::E_EXCLUSIVE)
    {
        auto request = CreateSessionRequest<TEvService::TEvAcquireLockRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOwner(owner);
        request->Record.SetOffset(offset);
        request->Record.SetLength(len);
        request->Record.SetLockType(type);

        return request;
    }

    auto CreateReleaseLockRequest(ui64 handle, ui64 owner, ui64 offset, ui32 len)
    {
        auto request = CreateSessionRequest<TEvService::TEvReleaseLockRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOwner(owner);
        request->Record.SetOffset(offset);
        request->Record.SetLength(len);
        return request;
    }

    auto CreateTestLockRequest(
        ui64 handle,
        ui64 owner,
        ui64 offset,
        ui32 len,
        NProto::ELockType type = NProto::E_EXCLUSIVE)
    {
        auto request = CreateSessionRequest<TEvService::TEvTestLockRequest>();
        request->Record.SetHandle(handle);
        request->Record.SetOwner(owner);
        request->Record.SetOffset(offset);
        request->Record.SetLength(len);
        request->Record.SetLockType(type);

        return request;
    }

    auto CreateCreateCheckpointRequest(const TString& checkpointId, ui64 nodeId = RootNodeId)
    {
        auto request = CreateSessionRequest<TEvService::TEvCreateCheckpointRequest>();
        request->Record.SetCheckpointId(checkpointId);
        request->Record.SetNodeId(nodeId);
        return request;
    }

    auto CreateDestroyCheckpointRequest(const TString& checkpointId)
    {
        auto request = CreateSessionRequest<TEvService::TEvDestroyCheckpointRequest>();
        request->Record.SetCheckpointId(checkpointId);
        return request;
    }

    auto CreateSubscribeSessionRequest()
    {
        auto request = CreateSessionRequest<TEvService::TEvSubscribeSessionRequest>();
        return request;
    }

    //
    // Monitoring Http Info
    //

    void SendRemoteHttpInfoRequest(const TString& query = {})
    {
        auto request = std::make_unique<NImpl::TEvRemoteHttpInfoRequest>(
            "/app?" + query);
        SendRequest(std::move(request));
    }

    std::unique_ptr<NImpl::TEvRemoteHttpInfoResponse> RecvRemoteHttpInfoResponse()
    {
        return RecvResponse<NImpl::TEvRemoteHttpInfoResponse>();
    }

    std::unique_ptr<NImpl::TEvRemoteHttpInfoResponse> GetRemoteHttpInfo(TString query = {})
    {
        SendRemoteHttpInfoRequest(std::move(query));
        return RecvRemoteHttpInfoResponse();
    }

    //
    // Time based functions
    //

    void AdvanceTime(TDuration duration)
    {
        Runtime.AdvanceCurrentTime(duration);
    }

    void DispatchEvents()
    {
        Runtime.DispatchEvents({}, TDuration::Zero());
    }

#define FILESTORE_DECLARE_METHOD(name, ns)                                     \
    template <typename... Args>                                                \
    void Send##name##Request(Args&&... args)                                   \
    {                                                                          \
        auto request = Create##name##Request(std::forward<Args>(args)...);     \
        SendRequest(std::move(request));                                       \
    }                                                                          \
                                                                               \
    std::unique_ptr<ns::TEv##name##Response> Recv##name##Response()            \
    {                                                                          \
        return RecvResponse<ns::TEv##name##Response>();                        \
    }                                                                          \
                                                                               \
    template <typename... Args>                                                \
    std::unique_ptr<ns::TEv##name##Response> name(Args&&... args)              \
    {                                                                          \
        auto request = Create##name##Request(std::forward<Args>(args)...);     \
        SendRequest(std::move(request));                                       \
                                                                               \
        auto response = Recv##name##Response();                                \
        UNIT_ASSERT_C(                                                         \
            SUCCEEDED(response->GetStatus()),                                  \
            response->GetErrorReason());                                       \
        return response;                                                       \
    }                                                                          \
                                                                               \
    template <typename... Args>                                                \
    std::unique_ptr<ns::TEv##name##Response> Assert##name##Failed(             \
        Args&&... args)                                                        \
    {                                                                          \
        auto request = Create##name##Request(std::forward<Args>(args)...);     \
        auto dbg = NImpl::DebugDump(*request);                                 \
        SendRequest(std::move(request));                                       \
                                                                               \
        auto response = Recv##name##Response();                                \
        UNIT_ASSERT_C(                                                         \
            FAILED(response->GetStatus()),                                     \
            #name " has not failed as expected " + dbg);                       \
        return response;                                                       \
    }                                                                          \
                                                                               \
    std::unique_ptr<ns::TEv##name##Response> Assert##name##QuickResponse(      \
        ui32 status)                                                           \
    {                                                                          \
        DispatchEvents();                                                      \
        auto evList = Runtime.CaptureEvents();                                 \
                                                                               \
        std::unique_ptr<NActors::IEventHandle> handle;                         \
        for (auto& ev : evList) {                                              \
            if (ev->GetTypeRewrite() == ns::Ev##name##Response) {              \
                handle.reset(ev.Release());                                    \
                break;                                                         \
            }                                                                  \
        }                                                                      \
        UNIT_ASSERT(handle);                                                   \
        auto response = handle->Release<ns::TEv##name##Response>();            \
        UNIT_ASSERT_VALUES_EQUAL(status, response->GetStatus());               \
        Runtime.PushEventsFront(evList);                                       \
                                                                               \
        return std::unique_ptr<ns::TEv##name##Response>(response.Release());   \
    }                                                                          \
                                                                               \
    std::unique_ptr<ns::TEv##name##Response> Assert##name##Response(           \
        ui32 status)                                                           \
    {                                                                          \
        auto response = Recv##name##Response();                                \
        UNIT_ASSERT(response);                                                 \
        UNIT_ASSERT_VALUES_EQUAL(status, response->GetStatus());               \
        return response;                                                       \
    }                                                                          \
                                                                               \
    void Assert##name##NoResponse()                                            \
    {                                                                          \
        DispatchEvents();                                                      \
        auto evList = Runtime.CaptureEvents();                                 \
                                                                               \
        for (auto& ev: evList) {                                               \
            UNIT_ASSERT(                                                       \
                ev->GetTypeRewrite() != ns::TEv##name##Response::EventType     \
            );                                                                 \
        }                                                                      \
                                                                               \
        Runtime.PushEventsFront(evList);                                       \
    }                                                                          \
// FILESTORE_DECLARE_METHOD

    FILESTORE_SERVICE_REQUESTS_FWD(FILESTORE_DECLARE_METHOD, TEvService)

    FILESTORE_TABLET_REQUESTS(FILESTORE_DECLARE_METHOD, TEvIndexTablet)
    FILESTORE_TABLET_REQUESTS_PRIVATE(FILESTORE_DECLARE_METHOD, TEvIndexTabletPrivate)

#undef FILESTORE_DECLARE_METHOD

    auto CreateUpdateConfigRequest(const TFileSystemConfig& config)
    {
        auto request = std::make_unique<NKikimr::TEvFileStore::TEvUpdateConfig>();
        auto* c = request->Record.MutableConfig();
        c->SetFileSystemId(config.FileSystemId);
        c->SetCloudId(config.CloudId);
        c->SetFolderId(config.FolderId);
        c->SetBlockSize(config.BlockSize);
        c->SetBlocksCount(config.BlockCount);
        c->SetNodesCount(config.NodeCount);
        c->SetRangeIdHasherType(RangeIdHasherType);
        c->SetStorageMediaKind(config.StorageMediaKind);

        c->SetPerformanceProfileThrottlingEnabled(
            config.PerformanceProfile.ThrottlingEnabled);
        c->SetPerformanceProfileMaxReadIops(
            config.PerformanceProfile.MaxReadIops);
        c->SetPerformanceProfileMaxWriteIops(
            config.PerformanceProfile.MaxWriteIops);
        c->SetPerformanceProfileMaxReadBandwidth(
            config.PerformanceProfile.MaxReadBandwidth);
        c->SetPerformanceProfileMaxWriteBandwidth(
            config.PerformanceProfile.MaxWriteBandwidth);
        c->SetPerformanceProfileBoostTime(
            config.PerformanceProfile.BoostTime);
        c->SetPerformanceProfileBoostRefillTime(
            config.PerformanceProfile.BoostRefillTime);
        c->SetPerformanceProfileBoostPercentage(
            config.PerformanceProfile.BoostPercentage);
        c->SetPerformanceProfileMaxPostponedWeight(
            config.PerformanceProfile.MaxPostponedWeight);
        c->SetPerformanceProfileMaxWriteCostMultiplier(
            config.PerformanceProfile.MaxWriteCostMultiplier);
        c->SetPerformanceProfileMaxPostponedTime(
            config.PerformanceProfile.MaxPostponedTime);
        c->SetPerformanceProfileMaxPostponedCount(
            config.PerformanceProfile.MaxPostponedCount);
        c->SetPerformanceProfileBurstPercentage(
            config.PerformanceProfile.BurstPercentage);
        c->SetPerformanceProfileDefaultPostponedRequestWeight(
            config.PerformanceProfile.DefaultPostponedRequestWeight);

        Y_VERIFY(config.ChannelCount >= 4);

        // system [0]
        {
            auto* ecp = c->MutableExplicitChannelProfiles()->Add();
            ecp->SetPoolKind("pool-kind-1");
            ecp->SetDataKind(static_cast<ui32>(EChannelDataKind::System));
        }

        // index [1]
        {
            auto* ecp = c->MutableExplicitChannelProfiles()->Add();
            ecp->SetPoolKind("pool-kind-1");
            ecp->SetDataKind(static_cast<ui32>(EChannelDataKind::Index));
        }

        // fresh [2]
        {
            auto* ecp = c->MutableExplicitChannelProfiles()->Add();
            ecp->SetPoolKind("pool-kind-1");
            ecp->SetDataKind(static_cast<ui32>(EChannelDataKind::Fresh));

        }

        // mixed [3+]
        for (ui32 channel = 3; channel < config.ChannelCount; ++channel) {
            auto* ecp = c->MutableExplicitChannelProfiles()->Add();
            ecp->SetPoolKind("pool-kind-1");
            ecp->SetDataKind(static_cast<ui32>(EChannelDataKind::Mixed));
        }

        return request;
    }

    std::unique_ptr<NKikimr::TEvFileStore::TEvUpdateConfigResponse> UpdateConfig(
        const TFileSystemConfig& config)
    {
        auto request = CreateUpdateConfigRequest(config);
        SendRequest(std::move(request));

        auto response = RecvResponse<NKikimr::TEvFileStore::TEvUpdateConfigResponse>();
        UNIT_ASSERT(response->Record.GetStatus() == NKikimrFileStore::OK);

        return response;
    }

    void ReconnectPipe()
    {
        PipeClient = Runtime.ConnectToPipe(
            TabletId,
            Sender,
            NodeIdx,
            NKikimr::GetPipeConfigWithRetries());
    }
};

////////////////////////////////////////////////////////////////////////////////

inline ui64 CreateNode(TIndexTabletClient& tablet, const TCreateNodeArgs& args)
{
    auto response = tablet.CreateNode(args);
    return response->Record.GetNode().GetId();
}

inline ui64 CreateHandle(
    TIndexTabletClient& tablet,
    ui64 node,
    const TString& name = {},
    ui32 flags = TCreateHandleArgs::RDWR)
{
    auto response = tablet.CreateHandle(node, name, flags);
    return response->Record.GetHandle();
}

inline NProto::TNodeAttr GetNodeAttrs(TIndexTabletClient& tablet, ui64 node)
{
    return tablet.GetNodeAttr(node)->Record.GetNode();
}

inline NProtoPrivate::TStorageStats GetStorageStats(TIndexTabletClient& tablet)
{
    return tablet.GetStorageStats()->Record.GetStats();
}

inline NCloud::NProto::EStorageMediaKind GetStorageMediaKind(TIndexTabletClient& tablet)
{
    return tablet.GetStorageStats()->Record.GetMediaKind();
}

inline NProtoPrivate::TFileSystemConfig GetFileSystemConfig(TIndexTabletClient& tablet)
{
    return tablet.GetFileSystemConfig()->Record.GetConfig();
}

inline TString ReadData(TIndexTabletClient& tablet, ui64 handle, ui64 size, ui64 offset = 0)
{
    auto response = tablet.ReadData(handle, offset, size);
    return response->Record.GetBuffer();
}

}   // namespace NCloud::NFileStore::NStorage