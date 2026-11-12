/* F2: nn.account -- one fixed local user, no network service account. */
#include <string.h>
#include <stdlib.h>
#include "f2_nn.h"

const nn_uid_t f2_nn_uid = { 0x0F2F2F2F00000001ull, 0x0000000000000010ull };

static int is_user(uint64_t d0, uint64_t d1) { return d0 == f2_nn_uid.d0 && d1 == f2_nn_uid.d1; }

void nn_account_Initialize(void) { F2_ONCE("F2: nn account.Initialize"); }

nn_result_t nn_account_GetUserCount(int32_t *out)
{
    F2_ONCE("F2: nn account.GetUserCount -> 1");
    if (out) *out = 1;
    return NN_OK;
}

nn_result_t nn_account_GetUserExistence(uint8_t *out, uint64_t d0, uint64_t d1)
{
    F2_ONCE("F2: nn account.GetUserExistence");
    if (out) *out = is_user(d0, d1);
    return NN_OK;
}

/* ListAllUsers / ListOpenUsers (int *outCount, Uid *out, int count) */
nn_result_t nn_account_ListUsers(int32_t *out_count, nn_uid_t *out, int32_t count)
{
    F2_ONCE("F2: nn account.ListAllUsers/ListOpenUsers -> 1 user");
    if (out && count > 0) {
        memset(out, 0, sizeof(*out) * (size_t)count);
        out[0] = f2_nn_uid;
    }
    if (out_count) *out_count = count > 0 ? 1 : 0;
    return NN_OK;
}

nn_result_t nn_account_GetLastOpenedUser(nn_uid_t *out)
{
    F2_ONCE("F2: nn account.GetLastOpenedUser");
    if (out) *out = f2_nn_uid;
    return NN_OK;
}

/* Nickname is marshalled as char[33] */
nn_result_t nn_account_GetNickname(char *out, uint64_t d0, uint64_t d1)
{
    F2_ONCE("F2: nn account.GetNickname -> \"Player\"");
    (void)d0; (void)d1;
    if (out) { memset(out, 0, 33); strcpy(out, "Player"); }
    return NN_OK;
}

nn_result_t nn_account_LoadProfileImage(uint64_t *out_size, void *buf, uint64_t size, uint64_t d0, uint64_t d1)
{
    F2_ONCE("F2: nn account.LoadProfileImage -> empty");
    (void)buf; (void)size; (void)d0; (void)d1;
    if (out_size) *out_size = 0;
    return NN_OK;
}

static void fill_handle(nn_user_handle_t *h)
{
    h->d0 = f2_nn_uid.d0; h->d1 = f2_nn_uid.d1; h->context = 1;
}

nn_result_t nn_account_OpenUser(nn_user_handle_t *out, uint64_t d0, uint64_t d1)
{
    F2_ONCE("F2: nn account.OpenUser");
    (void)d0; (void)d1;
    if (out) fill_handle(out);
    return NN_OK;
}

nn_result_t nn_account_OpenPreselectedUser(nn_user_handle_t *out)
{
    F2_ONCE("F2: nn account.OpenPreselectedUser -> user %016llx%016llx",
            (unsigned long long)f2_nn_uid.d1, (unsigned long long)f2_nn_uid.d0);
    if (out) fill_handle(out);
    return NN_OK;
}

void nn_account_CloseUser(const nn_user_handle_t *h) { (void)h; F2_ONCE("F2: nn account.CloseUser"); }

nn_result_t nn_account_GetUserId(nn_uid_t *out, const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.GetUserId");
    if (out) {
        if (h && (h->d0 | h->d1)) { out->d0 = h->d0; out->d1 = h->d1; }
        else *out = f2_nn_uid;
    }
    return NN_OK;
}

nn_result_t nn_account_StoreSaveDataThumbnailImage(uint64_t d0, uint64_t d1, const void *img, uint64_t size)
{
    F2_ONCE("F2: nn account.StoreSaveDataThumbnailImage (ignored, %llu bytes)", (unsigned long long)size);
    (void)d0; (void)d1; (void)img;
    return NN_OK;
}

nn_result_t nn_account_DeleteSaveDataThumbnailImage(uint64_t d0, uint64_t d1)
{
    F2_ONCE("F2: nn account.DeleteSaveDataThumbnailImage");
    (void)d0; (void)d1;
    return NN_OK;
}

/* ---- network service account: never available ---- */
nn_result_t nn_account_nsa_EnsureAvailable(const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.NetworkServiceAccount.EnsureAvailable -> unavailable");
    (void)h;
    return NN_ACCOUNT_NSA_UNAVAILABLE;
}

nn_result_t nn_account_nsa_IsAvailable(uint8_t *out, const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.NetworkServiceAccount.IsAvailable -> false");
    (void)h;
    if (out) *out = 0;
    return NN_OK;
}

nn_result_t nn_account_nsa_GetId(uint64_t *out, const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.NetworkServiceAccount.GetId -> unavailable");
    (void)h;
    if (out) *out = 0;
    return NN_ACCOUNT_NSA_UNAVAILABLE;
}

nn_result_t nn_account_nsa_EnsureIdTokenCacheAsync(void *ctx, const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.NetworkServiceAccount.EnsureIdTokenCacheAsync -> unavailable");
    (void)ctx; (void)h;
    return NN_ACCOUNT_NSA_UNAVAILABLE;
}

nn_result_t nn_account_nsa_LoadIdTokenCache(uint64_t *out_size, char *buf, uint64_t size, const nn_user_handle_t *h)
{
    F2_ONCE("F2: nn account.NetworkServiceAccount.LoadIdTokenCache -> unavailable");
    (void)h;
    if (out_size) *out_size = 0;
    if (buf && size) buf[0] = 0;
    return NN_ACCOUNT_NSA_UNAVAILABLE;
}

/* ---- AsyncContext: always already done, with the "unavailable" result (a waiting loop must end) ---- */
void *nn_account_AsyncContext_ctor(void)
{
    F2_ONCE("F2: nn account.AsyncContext.ctor");
    return calloc(1, 0x20);
}

void nn_account_AsyncContext_Dispose(void *ctx)
{
    F2_ONCE("F2: nn account.AsyncContext.Dispose");
    free(ctx);
}

nn_result_t nn_account_AsyncContext_Cancel(void *ctx) { (void)ctx; F2_ONCE("F2: nn account.AsyncContext.Cancel"); return NN_OK; }

nn_result_t nn_account_AsyncContext_HasDone(void *ctx, uint8_t *out)
{
    F2_ONCE("F2: nn account.AsyncContext.HasDone -> true");
    (void)ctx;
    if (out) *out = 1;
    return NN_OK;
}

nn_result_t nn_account_AsyncContext_GetResult(void *ctx)
{
    F2_ONCE("F2: nn account.AsyncContext.GetResult -> unavailable");
    (void)ctx;
    return NN_ACCOUNT_NSA_UNAVAILABLE;
}

/* both overloads: (Uid *out, settings) / (Uid *out) */
nn_result_t nn_account_ShowUserSelector(nn_uid_t *out)
{
    F2_ONCE("F2: nn account.ShowUserSelector -> the local user (no applet)");
    if (out) *out = f2_nn_uid;
    return NN_OK;
}

nn_result_t nn_account_ShowUserCreator(void) { F2_ONCE("F2: nn account.ShowUserCreator (no applet)"); return NN_OK; }
