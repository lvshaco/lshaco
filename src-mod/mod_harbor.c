#include "shaco.h"
#include "shaco_harbor.h"
#include "shaco_socket.h"
#include "socket.h"
#include "socket_buffer.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>

#define NODE_MAX 256

#pragma pack(1)
struct package_header {
    uint16_t source;
    uint16_t dest;
    int session;
    int type;
};
#pragma pack()

#define HEADSZ 8

struct slave {
    int id;
    int sock;
    struct socket_buffer sb;
};

struct harbor {
    uint32_t slave_handle;
    int slaveid;
    struct slave slaves[NODE_MAX];
};

//
static inline void
_to_bigendian16(uint16_t n, uint8_t *buffer) {
    buffer[0] = (n >> 8) & 0xff;
    buffer[1] = (n) & 0xff;
}

static inline void
_to_bigendian32(uint32_t n, uint8_t *buffer) {
    buffer[0] = (n >> 24) & 0xff;
    buffer[1] = (n >> 16) & 0xff;
    buffer[2] = (n >> 8)  & 0xff;
    buffer[3] = (n) & 0xff;
}

static inline uint16_t 
_from_bigendian16(const uint8_t *buffer) {
    return buffer[0] << 8 | buffer[1];
}

static inline uint32_t 
_from_bigendian32(const uint8_t *buffer) {
    return buffer[0] << 24 | buffer[1] << 16 | buffer[2] << 8 | buffer[3];
}

static inline void *
_readheader(uint8_t *p, struct package_header *header) {
    header->source = _from_bigendian16(p);
    header->dest   = p[2];
    header->type   = p[3];
    header->session= _from_bigendian32(p+4);
    return p+HEADSZ;
}

struct harbor *
harbor_create() {
    struct harbor *self = shaco_malloc(sizeof(*self));
    memset(self, 0, sizeof(*self));
    return self;
}

void
harbor_free(struct harbor *self) {
    shaco_free(self);
}

static struct slave *
_find_slave_bysock(struct harbor *self, int sock) {
    int i;
    for (i=0; i<NODE_MAX; ++i) {
        if (self->slaves[i].sock == sock)
            return &self->slaves[i];
    }
    return NULL;
}

static inline struct slave *
_index_slave(struct harbor *self, int slaveid) {
    if (slaveid > 0 && slaveid < NODE_MAX)
        return &self->slaves[slaveid];
    return NULL;
}

static int
_sock_error(struct shaco_context *ctx, struct harbor *self, int sock, const char *err) {
    struct slave *s = _find_slave_bysock(self, sock);
    if (s) {
        int slaveid = s->id;
        shaco_info(ctx, "Slave %02x exit: %s", slaveid, err);
        s->id = 0;
        s->sock = 0;
        sb_fini(&s->sb);
        char tmp[64];
        int n = sprintf(tmp, "D %d", slaveid);
        return shaco_send(ctx, self->slave_handle, 0, SHACO_TTEXT, tmp, n);
    } else {
        shaco_info(ctx, "Unknown slave socket=%d error: %s", sock, err); 
        return 1;
    }
}

static int
_handle_package(struct shaco_context *ctx, struct harbor *self, struct slave *s) {
    for (;;) {
        struct socket_pack package;
        if (sb_pop(&s->sb, &package)) 
            break;
        if (package.sz <= HEADSZ) {
            shaco_free(package.p);
            _sock_error(ctx, self, s->sock, "package head too small");
            break;
        }
        struct package_header header;
        void *p = _readheader(package.p, &header);
        shaco_handle_send(header.dest, header.source, 
                header.session, header.type, p, package.sz-HEADSZ);
        shaco_free(package.p);
    } 
    return 0;
}

static int
_sock_read(struct shaco_context *ctx, struct harbor *self, int sock, void *data, int size) {
    struct slave *s = _find_slave_bysock(self, sock);
    if (s == NULL)
        return 1;
    sb_push(&s->sb, data, size);
    _handle_package(ctx, self, s);
    return 0;
}

static int
_dosock(struct shaco_context *ctx, struct harbor *self, struct socket_message *event){
    switch (event->type) {
    case SOCKET_TYPE_DATA:
        return _sock_read(ctx, self, event->id, event->data, event->size);
    case SOCKET_TYPE_SOCKERR:
        return _sock_error(ctx, self, event->id, "socket error");
    default:
        shaco_error(ctx, "Invalid socket message type %d", event->type);
        return 1;
    }
}

static int
_toremote(struct shaco_context *ctx, struct harbor *self, int session, int source, int dest, int type, const void *msg, int sz) {
    int slaveid = (dest>>8)&0xff;
    struct slave *s= _index_slave(self, slaveid);
    if (s == NULL) {
        shaco_error(ctx, "Slave %0x no found", slaveid);
        return 1;
    }
    int len = sz+HEADSZ+4;
    source &= 0x00ff;
    source |= (self->slaveid << 8);
    uint8_t *tmp = shaco_malloc(len);
    _to_bigendian32(len-4, tmp);
    _to_bigendian16(source, tmp+4);
    tmp[6] = (uint8_t)dest&0xff;
    tmp[7] = (uint8_t)type&0xff;
    _to_bigendian32(session, tmp+8);
    memcpy(tmp+HEADSZ+4, msg, sz);
    return shaco_socket_psend(ctx, s->sock, tmp, len) >= 0 ? 0:1;
}

static int
_command(struct shaco_context *ctx, struct harbor *self, int source, int session, const void *msg, int sz) {
    if (sz <= 0)
        return 1;

    char tmp[sz+1];
    memcpy(tmp, msg, sz);
    tmp[sz] = '\0';

    switch(tmp[0]) {
    case 'S': {
        int sock, slaveid, bufsz;
        char addr[sz+1], bufp[sz+1];
        // todo drop addr field
        sscanf(tmp+1, " %d %d %s %s %d", &sock, &slaveid, addr, bufp, &bufsz);
        struct slave *s = _index_slave(self, slaveid);
        if (s == NULL) {
            shaco_error(ctx, "Invalid slave id %d", slaveid);
            return 1;
        }
        if (s->id != 0) {
            shaco_error(ctx, "Slave %02x already exist", slaveid);
            shaco_socket_close(sock, 1);
            return 0;
        }
        shaco_socket_start(ctx, sock);
        s->id = slaveid;
        s->sock = sock;
        sb_init(&s->sb);
        uint64_t p = strtoll(bufp, NULL, 16);
        if (p && bufsz) {
            sb_push(&s->sb, (void*)p, bufsz);
            _handle_package(ctx, self, s);
        }
        break;}
    default:
        shaco_error(ctx, "Invalid harbor command %s", tmp);
        return 1;
    }
    return 0;
}

static int 
cb(struct shaco_context *ctx, void *ud, int source, int session, int type, const void *msg, int sz) {
    struct harbor *self = (struct harbor *)ud;
    switch (type) {
    case SHACO_TSOCKET: {
        struct socket_message *event = (struct socket_message *)msg;
        assert(sizeof(*event)==sz);
        return _dosock(ctx, self, event);
        }
    case SHACO_TREMOTE: {
        struct shaco_remote_message *rmsg = (struct shaco_remote_message *)msg;
        assert(sizeof(*rmsg)==sz);
        return _toremote(ctx, self, session, source, 
                rmsg->dest, rmsg->type, rmsg->msg, rmsg->sz);
                        }
    case SHACO_TTEXT:
        return _command(ctx, self, source, session, msg, sz);
    default:
        return 1;
    }
    return 0;
}

int
harbor_init(struct shaco_context *ctx, struct harbor *self, const char *args) {
    if (args)
        self->slave_handle = strtol(args, NULL, 16);
    if (self->slave_handle == 0) {
        shaco_exit(ctx, "Slave handle is none");
    }
    self->slaveid = shaco_optint("slaveid", 0);
    if (self->slaveid == 0){
        shaco_exit(ctx, "Slaveid = 0");
    }
    shaco_callback(ctx, cb, self);
    shaco_harbor_start(ctx);
    return 0;
}
