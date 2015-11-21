#include "shaco.h"
#include "shaco_malloc.h"
#include "shaco_module.h"
#include "shaco_handle.h"
#include "shaco_log.h"
#include <stdlib.h>

struct shaco_context {
    struct shaco_module *module;
    const char *name;
    uint32_t handle;
    void *instance;
    shaco_cb cb;
    void *ud;
};

struct shaco_context *
shaco_context_create(const char *name) {
    const char *dlname;
    if (name[0] == '.') {
        dlname = "lua";
    } else {
        dlname = name;
    }
    struct shaco_module *dl = shaco_module_query(dlname);
    if (dl == NULL) {
        shaco_error("Context `%s` create fail: no module `%s`", name, dlname);
        return NULL;
    }
    struct shaco_context *ctx = shaco_malloc(sizeof(*ctx));
    ctx->module = dl;
    ctx->name = shaco_strdup(name);
    ctx->cb = NULL;
    ctx->ud = NULL;
    ctx->instance = shaco_module_instance_create(dl);
    ctx->handle = shaco_handle_register(ctx);
    if (ctx->module->init) {
        ctx->module->init(ctx, ctx->instance, NULL); // todo NULL -> args
    }
    return ctx;
}

void 
shaco_context_free(struct shaco_context *ctx) {
    //if (ctx) {
    //    shaco_free(ctx->name);
    //    shaco_module_instance_free(ctx, ctx->instance);
    //    ctx->name = NULL;
    //    shaco_free(ctx);
    //}
}

void 
shaco_context_send(struct shaco_context *ctx, int source, int session, int type, const void *msg, int sz) {
    int result = ctx->cb(ctx, ctx->ud, source, session, type, msg, sz);
    if (result !=0 ) {
        shaco_error(
        "Context callback fail:%d : %0x->%s:%0x session:%d type:%d sz:%d", 
        result, source, ctx->module->name, ctx->handle, session, type, sz);
    }
}

void 
shaco_set_callback(struct shaco_context *ctx, shaco_cb cb, void *ud) {
    ctx->cb = cb;
    ctx->ud = ud;
}
