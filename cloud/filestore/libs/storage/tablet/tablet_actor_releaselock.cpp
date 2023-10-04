#include "tablet_actor.h"

namespace NCloud::NFileStore::NStorage {

using namespace NActors;

using namespace NKikimr;
using namespace NKikimr::NTabletFlatExecutor;

////////////////////////////////////////////////////////////////////////////////

void TIndexTabletActor::HandleReleaseLock(
    const TEvService::TEvReleaseLockRequest::TPtr& ev,
    const TActorContext& ctx)
{
    if (!AcceptRequest<TEvService::TReleaseLockMethod>(ev, ctx)) {
        return;
    }

    auto* msg = ev->Get();
    auto requestInfo = CreateRequestInfo(
        ev->Sender,
        ev->Cookie,
        msg->CallContext);

    ExecuteTx<TReleaseLock>(
        ctx,
        std::move(requestInfo),
        msg->Record);
}

////////////////////////////////////////////////////////////////////////////////

bool TIndexTabletActor::PrepareTx_ReleaseLock(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TReleaseLock& args)
{
    Y_UNUSED(ctx);
    Y_UNUSED(tx);

    FILESTORE_VALIDATE_TX_SESSION(ReleaseLock, args);

    return true;
}

void TIndexTabletActor::ExecuteTx_ReleaseLock(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TReleaseLock& args)
{
    Y_UNUSED(ctx);

    FILESTORE_VALIDATE_TX_ERROR(ReleaseLock, args);

    auto* session = FindSession(
        args.ClientId,
        args.SessionId,
        args.SessionSeqNo);
    TABLET_VERIFY(session);

    auto* handle = FindHandle(args.Request.GetHandle());
    if (!handle || handle->GetSessionId() != session->GetSessionId()) {
        args.Error = MakeError(E_FS_BADHANDLE, "invalid handle");
        return;
    }

    TLockRange range = {
        .NodeId = handle->GetNodeId(),
        .OwnerId = args.Request.GetOwner(),
        .Offset = args.Request.GetOffset(),
        .Length = args.Request.GetLength()
    };

    TIndexTabletDatabase db(tx.DB);
    ReleaseLock(db, session, range);
}

void TIndexTabletActor::CompleteTx_ReleaseLock(
    const TActorContext& ctx,
    TTxIndexTablet::TReleaseLock& args)
{
    auto response = std::make_unique<TEvService::TEvReleaseLockResponse>(args.Error);
    CompleteResponse<TEvService::TReleaseLockMethod>(
        response->Record,
        args.RequestInfo->CallContext,
        ctx);

    NCloud::Reply(ctx, *args.RequestInfo, std::move(response));
}

}   // namespace NCloud::NFileStore::NStorage