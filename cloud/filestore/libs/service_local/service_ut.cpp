#include "service.h"

#include "config.h"

#include <cloud/filestore/libs/service/context.h>
#include <cloud/filestore/libs/service/filestore.h>

#include <cloud/storage/core/libs/common/error.h>
#include <cloud/storage/core/libs/common/scheduler.h>
#include <cloud/storage/core/libs/common/task_queue.h>
#include <cloud/storage/core/libs/common/timer.h>
#include <cloud/storage/core/libs/diagnostics/logging.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/folder/path.h>
#include <util/random/random.h>

#include <algorithm>

namespace NCloud::NFileStore {

namespace {

////////////////////////////////////////////////////////////////////////////////

struct THeaders
{
    TString FileSystemId;
    TString ClientId;
    TString SessionId;
    ui64 SessionSeqNo = 0;

    template <typename T>
    void Fill(T& request) const
    {
        request.SetFileSystemId(FileSystemId);
        auto* headers = request.MutableHeaders();
        headers->SetClientId(ClientId);
        headers->SetSessionId(SessionId);
        headers->SetSessionSeqNo(SessionSeqNo);
    }
};

////////////////////////////////////////////////////////////////////////////////

enum class ENodeType
{
    Directory,
    File,
    Link,
    SymLink,
    Sock,
};

struct TCreateNodeArgs
{
    ui64 ParentNode = 0;
    TString Name;

    ENodeType NodeType;
    ui32 Mode = 0;

    ui64 TargetNode = 0;
    TString TargetPath;

    TCreateNodeArgs(ENodeType nodeType, ui64 parent, TString name)
        : ParentNode(parent)
        , Name(std::move(name))
        , NodeType(nodeType)
    {}

    void Fill(NProto::TCreateNodeRequest& request) const
    {
        request.SetNodeId(ParentNode);
        request.SetName(Name);

        switch (NodeType) {
            case ENodeType::Directory: {
                auto* dir = request.MutableDirectory();
                dir->SetMode(Mode);
                break;
            }

            case ENodeType::File: {
                auto* file = request.MutableFile();
                file->SetMode(Mode);
                break;
            }

            case ENodeType::Sock: {
                auto* socket = request.MutableSocket();
                socket->SetMode(Mode);
                break;
            }

            case ENodeType::Link: {
                auto* link = request.MutableLink();
                link->SetTargetNode(TargetNode);
                break;
            }

            case ENodeType::SymLink: {
                auto* link = request.MutableSymLink();
                link->SetTargetPath(TargetPath);
                break;
            }
        }
    }

    static TCreateNodeArgs Directory(ui64 parent, const TString& name, ui32 mode = 0)
    {
        TCreateNodeArgs args(ENodeType::Directory, parent, name);
        args.Mode = mode;
        return args;
    }

    static TCreateNodeArgs File(ui64 parent, const TString& name, ui32 mode = 0)
    {
        TCreateNodeArgs args(ENodeType::File, parent, name);
        args.Mode = mode;
        return args;
    }

    static TCreateNodeArgs Link(ui64 parent, const TString& name, ui64 targetNode)
    {
        TCreateNodeArgs args(ENodeType::Link, parent, name);
        args.TargetNode = targetNode;
        return args;
    }

    static TCreateNodeArgs SymLink(ui64 parent, const TString& name, const TString& targetPath)
    {
        TCreateNodeArgs args(ENodeType::SymLink, parent, name);
        args.TargetPath = targetPath;
        return args;
    }

    static TCreateNodeArgs Sock(ui64 parent, const TString& name, ui32 mode = 0)
    {
        TCreateNodeArgs args(ENodeType::Sock, parent, name);
        args.Mode = mode;
        return args;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TSetNodeAttrArgs
{
    ui64 Node;
    NProto::TSetNodeAttrRequest::EFlags Flags;
    NProto::TSetNodeAttrRequest::TUpdate Update;

    TSetNodeAttrArgs(ui64 node)
        : Node(node)
        , Flags(NProto::TSetNodeAttrRequest::F_NONE)
    {}

    void Fill(NProto::TSetNodeAttrRequest& request) const
    {
        request.SetNodeId(Node);
        request.SetFlags(Flags);
        request.MutableUpdate()->CopyFrom(Update);
    }

    void SetFlag(int value)
    {
        Flags = NProto::TSetNodeAttrRequest::EFlags(Flags | ProtoFlag(value));
    }

    TSetNodeAttrArgs& SetMode(ui32 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_MODE);
        Update.SetMode(value);
        return *this;
    }

    TSetNodeAttrArgs& SetUid(ui32 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_UID);
        Update.SetUid(value);
        return *this;
    }

    TSetNodeAttrArgs& SetGid(ui32 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_GID);
        Update.SetUid(value);
        return *this;
    }

    TSetNodeAttrArgs& SetSize(ui64 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_SIZE);
        Update.SetSize(value);
        return *this;
    }

    TSetNodeAttrArgs& SetATime(ui64 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_ATIME);
        Update.SetATime(value);
        return *this;
    }

    TSetNodeAttrArgs& SetMTime(ui64 value)
    {
        SetFlag(NProto::TSetNodeAttrRequest::F_SET_ATTR_MTIME);
        Update.SetMTime(value);
        return *this;
    }
};

////////////////////////////////////////////////////////////////////////////////

struct TCreateHandleArgs
{
    static constexpr ui32 RDNLY
        = ProtoFlag(NProto::TCreateHandleRequest::E_READ);

    static constexpr ui32 RDWR
        = ProtoFlag(NProto::TCreateHandleRequest::E_READ)
        | ProtoFlag(NProto::TCreateHandleRequest::E_WRITE);

    static constexpr ui32 CREATE
        = ProtoFlag(NProto::TCreateHandleRequest::E_CREATE)
        | ProtoFlag(NProto::TCreateHandleRequest::E_READ)
        | ProtoFlag(NProto::TCreateHandleRequest::E_WRITE);
};

////////////////////////////////////////////////////////////////////////////////

struct TTempDirectory
    : public TFsPath
{
    TTempDirectory()
        : TFsPath(MakeName())
    {
        MkDir();
    }

    ~TTempDirectory()
    {
        try {
            ForceDelete();
        } catch (...) {
        }
    }

    static TString MakeName()
    {
        TString name = "tmp_XXXXXXXX";
        for (size_t i = 4; i < name.size(); ++i) {
            name[i] = 'a' + RandomNumber(26u);
        }

        return name;
    }
};

using TTempDirectoryPtr = std::shared_ptr<TTempDirectory>;

////////////////////////////////////////////////////////////////////////////////

template <typename T>
concept HasSetLockType = requires(T t) {
    { t.SetLockType(std::declval<NProto::ELockType>()) } -> std::same_as<void>;
};

struct TTestBootstrap
{
    TIntrusivePtr<TCallContext> Ctx = MakeIntrusive<TCallContext>();
    ILoggingServicePtr Logging = CreateLoggingService("console", {TLOG_RESOURCES});
    ITimerPtr Timer = CreateWallClockTimer();
    ISchedulerPtr Scheduler = CreateScheduler();
    ITaskQueuePtr TaskQueue = CreateTaskQueueStub();

    TTempDirectoryPtr Cwd;
    IFileStoreServicePtr Store;

    THeaders Headers;

    static constexpr pid_t DefaultPid = 123;

    TTestBootstrap(const TTempDirectoryPtr& cwd = std::make_shared<TTempDirectory>())
        : Cwd(cwd)
    {
        Store = CreateLocalFileStore(
            CreateConfig(),
            Timer,
            Scheduler,
            Logging,
            TaskQueue);
        Store->Start();
    }

    TTestBootstrap(const TString& id, const TString& client = "client", const TString& session = {})
        : Cwd(std::make_shared<TTempDirectory>())
    {
        Store = CreateLocalFileStore(
            CreateConfig(),
            Timer,
            Scheduler,
            Logging,
            TaskQueue);
        Store->Start();

        CreateFileStore(id, "cloud", "folder", 100500, 500100);
        if (client) {
            Headers.SessionId = CreateSession(id, client, session).GetSession().GetSessionId();
        }
    }

    TLocalFileStoreConfigPtr CreateConfig()
    {
        NProto::TLocalServiceConfig config;
        config.SetRootPath(Cwd->GetName());

        return std::make_shared<TLocalFileStoreConfig>(config);
    }

    template<typename T>
    std::shared_ptr<T> CreateRequest()
    {
        auto request = std::make_shared<T>();
        Headers.Fill(*request);
        return request;
    }

    auto CreateCreateFileStoreRequest(
        const TString& id,
        const TString cloud,
        const TString folder,
        ui64 size,
        ui64 count)
    {
        auto request = CreateRequest<NProto::TCreateFileStoreRequest>();
        request->SetFileSystemId(id);
        request->SetCloudId(cloud);
        request->SetFolderId(folder);
        request->SetBlockSize(size);
        request->SetBlocksCount(count);
        return request;
    }

    auto CreateAlterFileStoreRequest(
        const TString& id,
        const TString cloud,
        const TString folder)
    {
        auto request = CreateRequest<NProto::TAlterFileStoreRequest>();
        request->SetFileSystemId(id);
        request->SetCloudId(cloud);
        request->SetFolderId(folder);
        return request;
    }

    auto CreateGetFileStoreInfoRequest(const TString& id)
    {
        auto request = CreateRequest<NProto::TGetFileStoreInfoRequest>();
        request->SetFileSystemId(id);
        return request;
    }

    auto CreateDestroyFileStoreRequest(const TString& id)
    {
        auto request = CreateRequest<NProto::TDestroyFileStoreRequest>();
        request->SetFileSystemId(id);
        return request;
    }

    void SwitchToSession(const THeaders& headers)
    {
        Headers = headers;
    }

    auto CreateCreateSessionRequest(
        const TString& id,
        const TString& clientId,
        const TString& sessionId,
        bool readOnly = false,
        ui64 seqNo = 0,
        bool restore = false)
    {
        Headers = {.FileSystemId = id, .ClientId = clientId, .SessionId = sessionId};
        auto request = CreateRequest<NProto::TCreateSessionRequest>();
        request->SetRestoreClientSession(restore);
        request->SetMountSeqNumber(seqNo);
        request->SetReadOnly(readOnly);

        return request;
    }

    auto CreateDestroySessionRequest()
    {
        return CreateRequest<NProto::TDestroySessionRequest>();
    }

    auto CreateResetSessionRequest(const TString& state)
    {
        auto request = CreateRequest<NProto::TResetSessionRequest>();
        request->SetSessionState(state);
        return request;
    }

    auto CreatePingSessionRequest()
    {
        auto request = CreateRequest<NProto::TPingSessionRequest>();
        return request;
    }

    auto CreateCreateNodeRequest(const TCreateNodeArgs& args)
    {
        auto request = CreateRequest<NProto::TCreateNodeRequest>();
        args.Fill(*request);
        Y_VERIFY(request->GetName());
        return request;
    }

    auto CreateUnlinkNodeRequest(ui64 parent, const TString& name, bool unlinkDirectory)
    {
        auto request = CreateRequest<NProto::TUnlinkNodeRequest>();
        request->SetNodeId(parent);
        request->SetName(name);
        request->SetUnlinkDirectory(unlinkDirectory);
        return request;
    }

    auto CreateRenameNodeRequest(
        ui64 parent,
        const TString& name,
        ui64 newparent,
        const TString& newname,
        ui32 flags)
    {
        auto request = CreateRequest<NProto::TRenameNodeRequest>();
        request->SetNodeId(parent);
        request->SetName(name);
        request->SetNewParentId(newparent);
        request->SetNewName(newname);
        request->SetFlags(flags);
        return request;
    }

    auto CreateAccessNodeRequest(ui64 node, int mask)
    {
        auto request = CreateRequest<NProto::TAccessNodeRequest>();
        request->SetNodeId(node);
        request->SetMask(mask);
        return request;
    }

    auto CreateReadLinkRequest(ui64 node)
    {
        auto request = CreateRequest<NProto::TReadLinkRequest>();
        request->SetNodeId(node);
        return request;
    }

    auto CreateListNodesRequest(ui64 node)
    {
        auto request = CreateRequest<NProto::TListNodesRequest>();
        request->SetNodeId(node);
        return request;
    }

    auto CreateGetNodeAttrRequest(ui64 node)
    {
        auto request = CreateRequest<NProto::TGetNodeAttrRequest>();
        request->SetNodeId(node);
        return request;
    }

    auto CreateGetNodeAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateRequest<NProto::TGetNodeAttrRequest>();
        request->SetNodeId(node);
        request->SetName(name);
        return request;
    }

    auto CreateSetNodeAttrRequest(const TSetNodeAttrArgs& args)
    {
        auto request = CreateRequest<NProto::TSetNodeAttrRequest>();
        args.Fill(*request);
        return request;
    }

    auto CreateListNodeXAttrRequest(ui64 node)
    {
        auto request = CreateRequest<NProto::TListNodeXAttrRequest>();
        request->SetNodeId(node);
        return request;
    }

    auto CreateGetNodeXAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateRequest<NProto::TGetNodeXAttrRequest>();
        request->SetNodeId(node);
        request->SetName(name);
        return request;
    }

    auto CreateSetNodeXAttrRequest(ui64 node, const TString& name, const TString& value)
    {
        auto request = CreateRequest<NProto::TSetNodeXAttrRequest>();
        request->SetNodeId(node);
        request->SetName(name);
        request->SetValue(value);
        return request;
    }

    auto CreateRemoveNodeXAttrRequest(ui64 node, const TString& name)
    {
        auto request = CreateRequest<NProto::TRemoveNodeXAttrRequest>();
        request->SetNodeId(node);
        request->SetName(name);
        return request;
    }

    auto CreateCreateHandleRequest(ui64 parent, const TString& name, ui32 flags)
    {
        auto request = CreateRequest<NProto::TCreateHandleRequest>();
        request->SetNodeId(parent);
        request->SetName(name);
        request->SetFlags(flags);
        return request;
    }

    auto CreateDestroyHandleRequest(ui64 handle)
    {
        auto request = CreateRequest<NProto::TDestroyHandleRequest>();
        request->SetHandle(handle);
        return request;
    }

    auto CreateWriteDataRequest(ui64 handle, ui64 offset, const TString& buffer)
    {
        auto request = CreateRequest<NProto::TWriteDataRequest>();
        request->SetHandle(handle);
        request->SetOffset(offset);
        request->SetBuffer(buffer);
        return request;
    }

    auto CreateReadDataRequest(ui64 handle, ui64 offset, ui32 len)
    {
        auto request = CreateRequest<NProto::TReadDataRequest>();
        request->SetHandle(handle);
        request->SetOffset(offset);
        request->SetLength(len);
        return request;
    }

    template<typename TRequestType>
    std::shared_ptr<TRequestType> CreateLockRequest(
        ui64 handle,
        ui64 offset,
        ui32 len,
        NProto::ELockOrigin origin = NProto::E_FCNTL,
        NProto::ELockType type = NProto::E_EXCLUSIVE,
        pid_t pid = DefaultPid)
    {
        auto request = CreateRequest<TRequestType>();
        request->SetHandle(handle);
        request->SetOffset(offset);
        request->SetLength(len);

        if constexpr (HasSetLockType<TRequestType>) {
            request->SetLockType(type);
        }

        request->SetLockOrigin(origin);
        request->SetPid(pid);
        return request;
    }

    auto CreateAcquireLockRequest(
        ui64 handle,
        ui64 offset,
        ui32 len,
        NProto::ELockType type = NProto::E_EXCLUSIVE,
        NProto::ELockOrigin origin = NProto::E_FCNTL)
    {
        return CreateLockRequest<NProto::TAcquireLockRequest>(
            handle,
            offset,
            len,
            origin,
            type);
    }

    auto CreateReleaseLockRequest(
        ui64 handle,
        ui64 offset,
        ui32 len,
        NProto::ELockOrigin origin = NProto::E_FCNTL)
    {
        return CreateLockRequest<NProto::TReleaseLockRequest>(
            handle,
            offset,
            len,
            origin);
    }

    auto CreateTestLockRequest(ui64 handle, ui64 offset, ui32 len)
    {
        return CreateLockRequest<NProto::TTestLockRequest>(handle, offset, len);
    }

    auto CreatePingRequest()
    {
        return std::make_shared<NProto::TPingRequest>();
    }

    auto CreateAllocateDataRequest(ui64 handle, ui64 offset, ui64 length, ui32 flags)
    {
        auto request = CreateRequest<NProto::TAllocateDataRequest>();
        request->SetHandle(handle);
        request->SetOffset(offset);
        request->SetLength(length);
        request->SetFlags(flags);
        return request;
    }

#define FILESTORE_DECLARE_METHOD(name, ns)                                         \
    template <typename... Args>                                                    \
    NProto::T##name##Response name(Args&&... args)                                 \
    {                                                                              \
        auto request = Create##name##Request(std::forward<Args>(args)...);         \
        auto dbg = request->ShortDebugString();                                    \
        auto response = Store->name(Ctx, std::move(request)).GetValueSync();       \
                                                                                   \
        UNIT_ASSERT_C(                                                             \
            SUCCEEDED(response.GetError().GetCode()),                              \
            response.GetError().GetMessage() + "@" + dbg);                         \
        return response;                                                           \
    }                                                                              \
                                                                                   \
    template <typename... Args>                                                    \
    NProto::T##name##Response Assert##name##Failed(Args&&... args)                 \
    {                                                                              \
        auto request = Create##name##Request(std::forward<Args>(args)...);         \
        auto dbg = request->ShortDebugString();                                    \
                                                                                   \
        auto response = Store->name(Ctx, std::move(request)).GetValueSync();       \
        UNIT_ASSERT_C(                                                             \
            FAILED(response.GetError().GetCode()),                                 \
            #name " has not failed as expected " + dbg);                           \
        return response;                                                           \
    }                                                                              \
// FILESTORE_DECLARE_METHOD

    FILESTORE_SERVICE(FILESTORE_DECLARE_METHOD)

#undef FILESTORE_DECLARE_METHOD
};

////////////////////////////////////////////////////////////////////////////////

ui64 CreateFile(TTestBootstrap& bootstrap, ui64 parent, const TString& name, int mode = 0)
{
    return bootstrap.CreateNode(TCreateNodeArgs::File(parent, name, mode)).GetNode().GetId();
}

ui64 CreateDirectory(TTestBootstrap& bootstrap, ui64 parent, const TString& name, int mode = 0)
{
    return bootstrap.CreateNode(TCreateNodeArgs::Directory(parent, name, mode)).GetNode().GetId();
}

ui64 CreateHardLink(TTestBootstrap& bootstrap, ui64 parent, const TString& name, ui64 target)
{
    return bootstrap.CreateNode(TCreateNodeArgs::Link(parent, name, target)).GetNode().GetId();
}

ui64 CreateSymLink(TTestBootstrap& bootstrap, ui64 parent, const TString& name, const TString& target)
{
    return bootstrap.CreateNode(TCreateNodeArgs::SymLink(parent, name, target)).GetNode().GetId();
}

ui64 CreateSock(TTestBootstrap& bootstrap, ui64 parent, const TString& name, int mode = 0)
{
    return bootstrap.CreateNode(TCreateNodeArgs::Sock(parent, name, mode)).GetNode().GetId();
}

TVector<TString> ListNames(TTestBootstrap& bootstrap, ui64 node)
{
    auto all = bootstrap.ListNodes(node).GetNames();

    TVector<TString> names;
    std::copy_if(begin(all), end(all), std::back_inserter(names),
        [] (const TString& name) {
            return name != "." && name != "..";
        });

    return names;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(LocalFileStore)
{
    Y_UNIT_TEST(ShouldCreateStore)
    {
        TTestBootstrap bootstrap;

        bootstrap.CreateFileStore("fs", "cloud", "folder", 100500, 500100);
        auto response = bootstrap.GetFileStoreInfo("fs");

        const auto& store = response.GetFileStore();
        UNIT_ASSERT_VALUES_EQUAL(store.GetFileSystemId(), "fs");
        UNIT_ASSERT_VALUES_EQUAL(store.GetCloudId(), "cloud");
        UNIT_ASSERT_VALUES_EQUAL(store.GetFolderId(), "folder");
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlockSize(), 100500);
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlocksCount(), 500100);
    }

    Y_UNIT_TEST(ShouldAlterStore)
    {
        TTestBootstrap bootstrap;

        bootstrap.CreateFileStore("fs", "cloud", "folder", 100500, 500100);
        bootstrap.AlterFileStore("fs", "xxxx", "yyyy");

        auto response = bootstrap.GetFileStoreInfo("fs");
        const auto& store = response.GetFileStore();
        UNIT_ASSERT_VALUES_EQUAL(store.GetFileSystemId(), "fs");
        UNIT_ASSERT_VALUES_EQUAL(store.GetCloudId(), "xxxx");
        UNIT_ASSERT_VALUES_EQUAL(store.GetFolderId(), "yyyy");
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlockSize(), 100500);
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlocksCount(), 500100);
    }

    Y_UNIT_TEST(ShouldCreateSession)
    {
        TTestBootstrap bootstrap;
        bootstrap.CreateFileStore("fs", "cloud", "folder", 100500, 500100);

        auto response = bootstrap.CreateSession("fs", "client", "");
        auto id = response.GetSession().GetSessionId();
        UNIT_ASSERT(id != "");

        THeaders session {
            .FileSystemId = "fs",
            .ClientId = "client",
            .SessionId = id};

        THeaders badClientSession {
            .FileSystemId = "fs",
            .ClientId = "client-xxx",
            .SessionId = id};

        THeaders badIdSession {
            .FileSystemId = "fs",
            .ClientId = "client",
            .SessionId = "random session"};

        // restore
        bootstrap.CreateSession("fs", "client", id);
        bootstrap.AssertCreateSessionFailed("fs", "client-xxx", id);

        bootstrap.SwitchToSession(session);
        bootstrap.PingSession();

        bootstrap.SwitchToSession(badClientSession);
        bootstrap.AssertPingSessionFailed();

        bootstrap.SwitchToSession(badIdSession);
        bootstrap.AssertPingSessionFailed();

        bootstrap.SwitchToSession(session);
        bootstrap.DestroySession();
    }

    Y_UNIT_TEST(ShouldNotCreateTheSameStore)
    {
        TTestBootstrap bootstrap("fs");
        bootstrap.AssertCreateFileStoreFailed("fs", "cloud", "filestore", 100, 500);
    }

    Y_UNIT_TEST(ShouldDestroyStore)
    {
        TTestBootstrap bootstrap("fs");
        bootstrap.DestroyFileStore("fs");
        // intentionally
        bootstrap.DestroyFileStore("fs");
    }

    Y_UNIT_TEST(ShouldRecoverLocalStores)
    {
        TTestBootstrap bootstrap;
        bootstrap.CreateFileStore("fs", "cloud", "folder", 100500, 500100);

        TTestBootstrap other(bootstrap.Cwd);
        auto store = other.GetFileStoreInfo("fs").GetFileStore();

        UNIT_ASSERT_VALUES_EQUAL(store.GetFileSystemId(), "fs");
        UNIT_ASSERT_VALUES_EQUAL(store.GetFolderId(), "folder");
        UNIT_ASSERT_VALUES_EQUAL(store.GetCloudId(), "cloud");
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlockSize(), 100500);
        UNIT_ASSERT_VALUES_EQUAL(store.GetBlocksCount(), 500100);
    }

    Y_UNIT_TEST(ShouldCreateFileNode)
    {
        TTestBootstrap bootstrap("fs");
        auto ctx = MakeIntrusive<TCallContext>();

        CreateFile(bootstrap, RootNodeId, "file1", 0755);
        CreateFile(bootstrap, RootNodeId, "file2", 0555);

        auto nodes = ListNames(bootstrap, RootNodeId);
        Sort(nodes);

        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(nodes[0], "file1");
        UNIT_ASSERT_VALUES_EQUAL(nodes[1], "file2");
    }

    Y_UNIT_TEST(ShouldCreateSockNode)
    {
        TTestBootstrap bootstrap("fs");
        auto ctx = MakeIntrusive<TCallContext>();

        CreateSock(bootstrap, RootNodeId, "file1", 0755);

        auto nodes = bootstrap.ListNodes(RootNodeId);

        UNIT_ASSERT_VALUES_EQUAL(nodes.GetNames().size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(nodes.GetNames(0), "file1");
        UNIT_ASSERT_VALUES_EQUAL(nodes.GetNodes(0).GetType(), (ui32)NProto::E_SOCK_NODE);
    }

    Y_UNIT_TEST(ShouldCreateDirNode)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateDirectory(bootstrap, RootNodeId, "dir1");
        CreateFile(bootstrap, id, "file1", 0555);

        auto nodes = ListNames(bootstrap, id);
        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(nodes[0], "file1");
    }

    Y_UNIT_TEST(ShouldCreateHardLinkNode)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateDirectory(bootstrap, RootNodeId, "dir");

        auto id1 = CreateFile(bootstrap, id, "file", 0777);
        auto id2 = CreateHardLink(bootstrap, RootNodeId, "link", id1);

        UNIT_ASSERT_VALUES_EQUAL(id1, id2);

        // cannot hard link directories
        bootstrap.AssertCreateNodeFailed(TCreateNodeArgs::Link(RootNodeId, "newdir", id));
    }

    Y_UNIT_TEST(ShouldCreateSymLinkNode)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateSymLink(bootstrap, RootNodeId, "link", "/xxxxxxx");

        auto response = bootstrap.ReadLink(id);
        UNIT_ASSERT_VALUES_EQUAL(response.GetSymLink(), "/xxxxxxx");
    }

    Y_UNIT_TEST(ShouldRemoveFileNode)
    {
        TTestBootstrap bootstrap("fs");
        auto ctx = MakeIntrusive<TCallContext>();

        CreateFile(bootstrap, RootNodeId, "file1");
        CreateSymLink(bootstrap, RootNodeId, "link", "/file1");
        bootstrap.UnlinkNode(RootNodeId, "file1", false);

        auto nodes = ListNames(bootstrap, RootNodeId);
        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(nodes[0], "link");
    }

    Y_UNIT_TEST(ShouldRemoveDirectoryNode)
    {
        TTestBootstrap bootstrap("fs");

        auto id1 = CreateDirectory(bootstrap, RootNodeId, "dir");
        auto id2 = CreateDirectory(bootstrap, id1, "dir");

        bootstrap.AssertUnlinkNodeFailed(id1, "file", false);
        bootstrap.AssertUnlinkNodeFailed(id2, "file", true);

        // not empty
        bootstrap.AssertUnlinkNodeFailed(RootNodeId, "dir", false);
        bootstrap.AssertUnlinkNodeFailed(RootNodeId, "dir", true);


        bootstrap.AssertUnlinkNodeFailed(id1, "dir", false);
        bootstrap.UnlinkNode(id1, "dir", true);
        bootstrap.UnlinkNode(RootNodeId, "dir", true);

        auto nodes = ListNames(bootstrap, RootNodeId);
        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 0);
    }

    Y_UNIT_TEST(ShouldRemoveSymLinkNode)
    {
        TTestBootstrap bootstrap("fs");
        CreateFile(bootstrap, RootNodeId, "file");
        CreateSymLink(bootstrap, RootNodeId, "link", "/file");
        bootstrap.UnlinkNode(RootNodeId, "link", false);

        auto nodes = ListNames(bootstrap, RootNodeId);
        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(nodes[0], "file");
    }

    Y_UNIT_TEST(ShouldOpenFileHandle)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateFile(bootstrap, RootNodeId, "file");
        auto handle = bootstrap.CreateHandle(id, "", TCreateHandleArgs::RDNLY).GetHandle();

        bootstrap.DestroyHandle(handle);
    }

    Y_UNIT_TEST(ShouldCreateFileHandle)
    {
        TTestBootstrap bootstrap("fs");

        bootstrap.AssertCreateHandleFailed(RootNodeId, "file", TCreateHandleArgs::RDNLY);

        bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::CREATE);

        auto nodes = ListNames(bootstrap, RootNodeId);
        UNIT_ASSERT_VALUES_EQUAL(nodes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(nodes[0], "file");
    }

    Y_UNIT_TEST(ShouldReadAndWriteData)
    {
        TTestBootstrap bootstrap("fs");

        ui64 handle = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::CREATE).GetHandle();
        auto data = bootstrap.ReadData(handle, 0, 100).GetBuffer();
        UNIT_ASSERT_VALUES_EQUAL(data.size(), 0);

        data = "aaaabbbbcccccdddddeeee";
        bootstrap.WriteData(handle, 0, data);

        auto buffer = bootstrap.ReadData(handle, 0, 100).GetBuffer();
        UNIT_ASSERT_VALUES_EQUAL(buffer, data);

        buffer = bootstrap.ReadData(handle, 0, 4).GetBuffer();
        UNIT_ASSERT_VALUES_EQUAL(buffer, "aaaa");

        buffer = bootstrap.ReadData(handle, 4, 4).GetBuffer();
        UNIT_ASSERT_VALUES_EQUAL(buffer, "bbbb");
    }

    Y_UNIT_TEST(ShouldAllocateData)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateFile(bootstrap, RootNodeId, "file");
        auto handle = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();

        bootstrap.AllocateData(handle, 0, 1024, 0);
        auto stat = bootstrap.GetNodeAttr(id).GetNode();
        UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 1024);

        bootstrap.AllocateData(handle, 0, 512, 0);
        stat = bootstrap.GetNodeAttr(id).GetNode();
        // Should not change size, because new_length < old_length
        UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 1024);

        bootstrap.AllocateData(handle, 256, 1280, 0);
        stat = bootstrap.GetNodeAttr(id).GetNode();
        UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 1536);
    }

    Y_UNIT_TEST(ShouldAllocateDataWithFlags)
    {
        TTestBootstrap bootstrap("fs");

        auto id = CreateFile(bootstrap, RootNodeId, "file");
        auto handle = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();

        TString data = "aaaabbbbccccddddeeee";
        bootstrap.WriteData(handle, 0, data);

        {
            const ui32 flags =
                ProtoFlag(NProto::TAllocateDataRequest::F_ZERO_RANGE);
            bootstrap.AllocateData(handle, 4, 4, flags);

            std::fill(data.begin() + 4, data.begin() + 8, 0);
            UNIT_ASSERT_VALUES_EQUAL(
                data,
                bootstrap.ReadData(handle, 0, 100).GetBuffer());

            auto stat = bootstrap.GetNodeAttr(id).GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 20);
        }

        {
            const ui32 flags =
                ProtoFlag(NProto::TAllocateDataRequest::F_PUNCH_HOLE) |
                ProtoFlag(NProto::TAllocateDataRequest::F_KEEP_SIZE);
            bootstrap.AllocateData(handle, 18, 4, flags);

            std::fill(data.begin() + 18, data.begin() + 20, 0);
            UNIT_ASSERT_VALUES_EQUAL(
                data,
                bootstrap.ReadData(handle, 0, 100).GetBuffer());

            auto stat = bootstrap.GetNodeAttr(id).GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 20);
        }

        {
            const ui32 flags =
                ProtoFlag(NProto::TAllocateDataRequest::F_PUNCH_HOLE);
            const auto response = bootstrap.AssertAllocateDataFailed(handle, 0, 4, flags);
            UNIT_ASSERT_VALUES_EQUAL(E_FS_NOTSUPP, response.GetError().GetCode());

            UNIT_ASSERT_VALUES_EQUAL(
                data,
                bootstrap.ReadData(handle, 0, 100).GetBuffer());

            auto stat = bootstrap.GetNodeAttr(id).GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetSize(), 20);
        }
    }

    Y_UNIT_TEST(ShouldLock)
    {
        TTestBootstrap bootstrap("fs");

        CreateFile(bootstrap, RootNodeId, "file", 0777);

        auto handle1 = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();
        bootstrap.AllocateData(handle1, 0, 1024, 0);
        bootstrap.AcquireLock(handle1, 0, 128);

        auto oldSession = bootstrap.Headers;

        auto id = bootstrap.CreateSession("fs", "client2", "").GetSession().GetSessionId();
        bootstrap.Headers.SessionId = id;

        auto handle2 = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();
        bootstrap.AssertTestLockFailed(handle2, 0, 128);
        bootstrap.AssertTestLockFailed(handle2, 32, 64);
        bootstrap.AssertTestLockFailed(handle2, 64, 128);

        bootstrap.TestLock(handle2, 128, 128);
        // over the file range?
        bootstrap.TestLock(handle2, 128, 1024);

        bootstrap.AssertAcquireLockFailed(handle2, 0, 128);
        bootstrap.AssertAcquireLockFailed(handle2, 32, 64);
        bootstrap.AssertAcquireLockFailed(handle2, 64,128);

        bootstrap.AcquireLock(handle2, 128, 128);

        std::swap(bootstrap.Headers, oldSession);
        bootstrap.ReleaseLock(handle1, 0, 128);

        std::swap(bootstrap.Headers, oldSession);
        bootstrap.AcquireLock(handle2, 0, 128);
    }

    Y_UNIT_TEST(ShouldSharedLock)
    {
        TTestBootstrap bootstrap("fs");

        CreateFile(bootstrap, RootNodeId, "file", 0777);

        auto handle1 = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();
        bootstrap.AllocateData(handle1, 0, 1024, 0);
        bootstrap.AcquireLock(handle1, 0, 128, NProto::E_SHARED);

        auto handle2 = bootstrap.CreateHandle(RootNodeId, "file", TCreateHandleArgs::RDWR).GetHandle();
        bootstrap.AcquireLock(handle2, 0, 128, NProto::E_SHARED);
    }

    Y_UNIT_TEST(ShouldHandleXAttrsForDirectory)
    {
        TTestBootstrap bootstrap("fs");
        auto id = CreateDirectory(bootstrap, RootNodeId, "folder");

        // no attributes

        auto attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 0);

        // 1 attribute
        bootstrap.SetNodeXAttr(id, "user.xattr1", "");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        auto val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "");

        bootstrap.SetNodeXAttr(id, "user.xattr1", "valueeee1");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        // 2 attributes
        bootstrap.SetNodeXAttr(id, "user.xattr2", "valueeee2");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");
        UNIT_ASSERT_VALUES_EQUAL(attributes[1], "user.xattr2");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr2").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee2");

        bootstrap.SetNodeXAttr(id, "user.xattr2", "valueeee");
        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");
        UNIT_ASSERT_VALUES_EQUAL(attributes[1], "user.xattr2");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr2").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee");

        // 1 attrribute
        bootstrap.RemoveNodeXAttr(id, "user.xattr2");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        // no attrributes
        bootstrap.RemoveNodeXAttr(id, "user.xattr1");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 0);
    }

    Y_UNIT_TEST(ShouldHandleXAttrsForFiles)
    {
        TTestBootstrap bootstrap("fs");
        auto id = CreateFile(bootstrap, RootNodeId, "file");

        // no attributes

        auto attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 0);

        // 1 attribute
        bootstrap.SetNodeXAttr(id, "user.xattr1", "");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        auto val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "");

        bootstrap.SetNodeXAttr(id, "user.xattr1", "valueeee1");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        // 2 attributes
        bootstrap.SetNodeXAttr(id, "user.xattr2", "valueeee2");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");
        UNIT_ASSERT_VALUES_EQUAL(attributes[1], "user.xattr2");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr2").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee2");

        bootstrap.SetNodeXAttr(id, "user.xattr2", "valueeee");
        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");
        UNIT_ASSERT_VALUES_EQUAL(attributes[1], "user.xattr2");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr2").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee");

        // 1 attrribute
        bootstrap.RemoveNodeXAttr(id, "user.xattr2");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(attributes[0], "user.xattr1");

        val = bootstrap.GetNodeXAttr(id, "user.xattr1").GetValue();
        UNIT_ASSERT_VALUES_EQUAL(val, "valueeee1");

        // no attrributes
        bootstrap.RemoveNodeXAttr(id, "user.xattr1");

        attributes = bootstrap.ListNodeXAttr(id).GetNames();
        UNIT_ASSERT_VALUES_EQUAL(attributes.size(), 0);
    }

    // TODO: should it work Y_UNIT_TEST(ShouldHandleXAttrsForSymLinks)

    Y_UNIT_TEST(ShouldHandleInproperXAttrsRequests)
    {
        TTestBootstrap bootstrap("fs");
        auto id = CreateFile(bootstrap, RootNodeId, "file");
        auto nonexistent = id + 100500;

        bootstrap.AssertListNodeXAttrFailed(nonexistent);

        bootstrap.AssertSetNodeXAttrFailed(id, "invalid", "value");
        bootstrap.AssertSetNodeXAttrFailed(nonexistent, "user.xattr", "value");

        bootstrap.AssertGetNodeXAttrFailed(id, "user.xattr");
        bootstrap.AssertGetNodeXAttrFailed(nonexistent, "user.xattr");

        bootstrap.AssertRemoveNodeXAttrFailed(nonexistent, "user.xattr");
        bootstrap.AssertRemoveNodeXAttrFailed(id, "user.xattr");
    }

    Y_UNIT_TEST(ShouldResetSessionStateAndRestoreByClient)
    {
        const TString state = "abcde";

        TTestBootstrap bootstrap("fs");
        bootstrap.ResetSession(state);

        auto response = bootstrap.CreateSession("fs", "client", "", true);
        UNIT_ASSERT_VALUES_EQUAL(response.GetSession().GetSessionState(), state);
    }

    Y_UNIT_TEST(ShouldSupportMigration)
    {
        TTestBootstrap bootstrap("fs");

        auto response = bootstrap.CreateSession("fs", "client", "", false, 0);
        auto sourceId = response.GetSession().GetSessionId();
        UNIT_ASSERT(sourceId != "");

        THeaders source {
            .FileSystemId = "fs",
            .ClientId = "client",
            .SessionId = sourceId,
            .SessionSeqNo = 0
        };

        bootstrap.SwitchToSession(source);
        bootstrap.PingSession();

        bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "file"));
        bootstrap.ListNodes(1);

        response = bootstrap.CreateSession("fs", "client", "", true, 1);
        auto targetId = response.GetSession().GetSessionId();
        UNIT_ASSERT(targetId == sourceId);

        THeaders target {
            .FileSystemId = "fs",
            .ClientId = "client",
            .SessionId = targetId,
            .SessionSeqNo = 1
        };

        bootstrap.SwitchToSession(target);
        bootstrap.PingSession();

        response = bootstrap.CreateSession("fs", "client", "", false, 1);
        UNIT_ASSERT(response.GetSession().GetSessionId() == targetId);

        bootstrap.SwitchToSession(target);
        bootstrap.PingSession();
        bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "file2"));
        bootstrap.ListNodes(1);

        bootstrap.SwitchToSession(source);
        bootstrap.DestroySession();

        bootstrap.SwitchToSession(target);
        bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "file3"));
    }

    Y_UNIT_TEST(ShouldRenameDirNodes)
    {
        TTestBootstrap bootstrap("fs");

        bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "xxx"));
        bootstrap.CreateNode(TCreateNodeArgs::SymLink(RootNodeId, "yyy", "zzz"));
        auto id1 = bootstrap
            .CreateNode(TCreateNodeArgs::Directory(RootNodeId, "a"))
            .GetNode()
            .GetId();
        auto id2 = bootstrap
            .CreateNode(TCreateNodeArgs::Directory(RootNodeId, "non-empty"))
            .GetNode()
            .GetId();
        bootstrap.CreateNode(TCreateNodeArgs::File(id2, "file"));

        // existing -> non-existing
        bootstrap.RenameNode(RootNodeId, "a", RootNodeId, "empty", 0);

        const auto response = bootstrap.ListNodes(RootNodeId);
        auto names = response.GetNames();
        std::sort(names.begin(), names.end());
        UNIT_ASSERT_VALUES_EQUAL(names.size(), 4);
        UNIT_ASSERT_VALUES_EQUAL(names[0], "empty");
        UNIT_ASSERT_VALUES_EQUAL(names[1], "non-empty");
        UNIT_ASSERT_VALUES_EQUAL(names[2], "xxx");
        UNIT_ASSERT_VALUES_EQUAL(names[3], "yyy");

        // file -> dir
        {
            const auto response = bootstrap
                .AssertRenameNodeFailed(RootNodeId, "xxx", RootNodeId, "empty", 0);
            const auto& error = response.GetError();
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NProto::E_FS_ISDIR),
                STATUS_FROM_CODE(error.GetCode()));
        }

        // dir -> file
        {
            const auto response = bootstrap
                .AssertRenameNodeFailed(RootNodeId, "empty", RootNodeId, "xxx", 0);
            const auto& error = response.GetError();
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NProto::E_FS_NOTDIR),
                STATUS_FROM_CODE(error.GetCode()));
        }

        // link -> dir
        {
            const auto response = bootstrap
                .AssertRenameNodeFailed(RootNodeId, "yyy", RootNodeId, "empty", 0);
            const auto& error = response.GetError();
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NProto::E_FS_ISDIR),
                STATUS_FROM_CODE(error.GetCode()));
        }

        // dir -> link
        {
            const auto response = bootstrap
                .AssertRenameNodeFailed(RootNodeId, "empty", RootNodeId, "yyy", 0);
            const auto& error = response.GetError();
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NProto::E_FS_NOTDIR),
                STATUS_FROM_CODE(error.GetCode()));
        }

        // existing -> existing not empty
        {
            const auto response = bootstrap
                .AssertRenameNodeFailed(RootNodeId, "empty", RootNodeId, "non-empty", 0);
            const auto& error = response.GetError();
            UNIT_ASSERT_VALUES_EQUAL(
                static_cast<ui32>(NProto::E_FS_NOTEMPTY),
                STATUS_FROM_CODE(error.GetCode()));
        }

        // existing -> existing empty
        bootstrap.RenameNode(RootNodeId, "non-empty", RootNodeId, "empty", 0);
        bootstrap.AssertAccessNodeFailed(id1, 0);

        {
            const auto response = bootstrap.ListNodes(RootNodeId);
            auto names = response.GetNames();
            std::sort(names.begin(), names.end());
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "empty");
            UNIT_ASSERT_VALUES_EQUAL(names[1], "xxx");
            UNIT_ASSERT_VALUES_EQUAL(names[2], "yyy");
        }

        {
            const auto response = bootstrap.ListNodes(id2);
            const auto& names = response.GetNames();
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "file");
        }
    }

    Y_UNIT_TEST(ShouldRenameFileNodes)
    {
        TTestBootstrap bootstrap("fs");

        auto id1 = bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "a")).GetNode().GetId();
        bootstrap.CreateNode(TCreateNodeArgs::Link(RootNodeId, "b", id1));

        // If oldpath and newpath are existing hard links to the same file, then rename() does nothing
        bootstrap.RenameNode(RootNodeId, "a", RootNodeId, "b", 0);
        {
            auto response = bootstrap.ListNodes(RootNodeId);
            auto names = response.GetNames();
            std::sort(names.begin(), names.end());
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "a");
            UNIT_ASSERT_VALUES_EQUAL(names[1], "b");
        }

        // a b -> b c
        bootstrap.RenameNode(RootNodeId, "a", RootNodeId, "c", 0);
        {
            auto response = bootstrap.ListNodes(RootNodeId);
            auto names = response.GetNames();
            std::sort(names.begin(), names.end());
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "b");
            UNIT_ASSERT_VALUES_EQUAL(names[1], "c");
        }

        // b c d -> b c
        auto id2 = bootstrap.CreateNode(TCreateNodeArgs::SymLink(RootNodeId, "d", "b")).GetNode().GetId();
        bootstrap.RenameNode(RootNodeId, "d", RootNodeId, "b", 0);
        {
            auto response = bootstrap.ReadLink(id2);
            UNIT_ASSERT_VALUES_EQUAL(response.GetSymLink(), "b");

            auto stat = bootstrap.GetNodeAttr(id1).GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id1);
            UNIT_ASSERT_VALUES_EQUAL(stat.GetLinks(), 1);
        }

        // b c -> c
        bootstrap.CreateHandle(id1, "", TCreateHandleArgs::RDWR);
        bootstrap.RenameNode(RootNodeId, "b", RootNodeId, "c", 0);
        {
            auto response = bootstrap.ListNodes(RootNodeId);
            const auto& names = response.GetNames();
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "c");

            auto link = bootstrap.ReadLink(id2);
            UNIT_ASSERT_VALUES_EQUAL(link.GetSymLink(), "b");

            auto stat = bootstrap.AssertGetNodeAttrFailed(id1);
        }

        // non-existing to existing
        bootstrap.AssertRenameNodeFailed(RootNodeId, "", RootNodeId, "c", 0);
        // existing to invalid
        bootstrap.AssertRenameNodeFailed(RootNodeId, "c", RootNodeId + 100, "c", 0);

        // rename into different dir
        auto id3 = bootstrap.CreateNode(TCreateNodeArgs::Directory(RootNodeId, "dir")).GetNode().GetId();
        bootstrap.RenameNode(RootNodeId, "c", id3, "c", 0);
        {
            auto response = bootstrap.ListNodes(RootNodeId);
            auto names = response.GetNames();
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "dir");

            response = bootstrap.ListNodes(id3);
            names = response.GetNames();
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "c");

            auto link = bootstrap.ReadLink(id2);
            UNIT_ASSERT_VALUES_EQUAL(link.GetSymLink(), "b");
        }
    }

    Y_UNIT_TEST(ShouldRenameAccordingToFlags)
    {
        const ui32 noreplace = ProtoFlag(NProto::TRenameNodeRequest::F_NOREPLACE);
        const ui32 exchange = ProtoFlag(NProto::TRenameNodeRequest::F_EXCHANGE);

        TTestBootstrap bootstrap("fs");

        auto id1 = bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "a")).GetNode().GetId();
        auto id2 = bootstrap.CreateNode(TCreateNodeArgs::File(RootNodeId, "b")).GetNode().GetId();

        // if newpath exists we should not perform
        bootstrap.AssertRenameNodeFailed(RootNodeId, "a", RootNodeId, "b", noreplace);
        // both flags is invalid
        bootstrap.AssertRenameNodeFailed(RootNodeId, "a", RootNodeId, "b", noreplace | exchange);
        // target should exist for exchange
        bootstrap.AssertRenameNodeFailed(RootNodeId, "a", RootNodeId, "c", exchange);

        // should keep both
        bootstrap.RenameNode(RootNodeId, "a", RootNodeId, "b", exchange);
        {
            const auto response = bootstrap.ListNodes(RootNodeId);
            auto names = response.GetNames();
            std::sort(names.begin(), names.end());
            UNIT_ASSERT_VALUES_EQUAL(names.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(names[0], "a");
            UNIT_ASSERT_VALUES_EQUAL(names[1], "b");

            auto stat = bootstrap.GetNodeAttr(RootNodeId, "a").GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id2);
            UNIT_ASSERT_VALUES_EQUAL(stat.GetLinks(), 1);

            stat = bootstrap.GetNodeAttr(RootNodeId, "b").GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id1);
            UNIT_ASSERT_VALUES_EQUAL(stat.GetLinks(), 1);
        }

        // non empty dir
        auto id3 = bootstrap.CreateNode(TCreateNodeArgs::Directory(RootNodeId, "c")).GetNode().GetId();
        bootstrap.CreateNode(TCreateNodeArgs::Directory(id3, "d"));
        // different fs implementations can explicitly hardlink directories via . or ..
        // thus creating subdirectories will increase links count
        ui32 expectedLinks = 0;
        {
            const auto stat = bootstrap.GetNodeAttr(RootNodeId, "c").GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id3);
            expectedLinks = stat.GetLinks();
        }

        // should allow to rename any nodes
        bootstrap.RenameNode(RootNodeId, "a", RootNodeId, "c", exchange);
        {
            auto response = bootstrap.ListNodes(RootNodeId);
            auto names1 = response.GetNames();
            std::sort(names1.begin(), names1.end());
            UNIT_ASSERT_VALUES_EQUAL(names1.size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(names1[0], "a");
            UNIT_ASSERT_VALUES_EQUAL(names1[1], "b");
            UNIT_ASSERT_VALUES_EQUAL(names1[2], "c");

            auto stat = bootstrap.GetNodeAttr(RootNodeId, "c").GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id2);
            UNIT_ASSERT_VALUES_EQUAL(stat.GetLinks(), 1);

            stat = bootstrap.GetNodeAttr(RootNodeId, "a").GetNode();
            UNIT_ASSERT_VALUES_EQUAL(stat.GetId(), id3);
            UNIT_ASSERT_VALUES_EQUAL(stat.GetLinks(), expectedLinks);

            response = bootstrap.ListNodes(id3);
            const auto& names2 = response.GetNames();
            UNIT_ASSERT_VALUES_EQUAL(names2.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(names2[0], "d");
        }
    }
};

}   // namespace NCloud::NFileStore