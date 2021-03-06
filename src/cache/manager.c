/*
 * Cache manager functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <types/applet.h>
#include <types/cli.h>
#include <types/global.h>
#include <types/cache.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/sample.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/acl.h>
#include <proto/proxy.h>
#include <proto/cache.h>

#include <import/xxhash.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

static const char *cache_msgs[NUSTER_CACHE_MSG_SIZE] = {
    [NUSTER_CACHE_200] =
        "HTTP/1.0 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "OK\n",

    [NUSTER_CACHE_400] =
        "HTTP/1.0 400 Bad request\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Bad request\n",

    [NUSTER_CACHE_404] =
        "HTTP/1.0 404 Not Found\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Not Found\n",

    [NUSTER_CACHE_500] =
        "HTTP/1.0 500 Server Error\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Server Error\n",
};

struct chunk cache_msg_chunks[NUSTER_CACHE_MSG_SIZE];

/*
 * purge cache by key
 */
int cache_purge_by_key(const char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;
    int ret;

    nuster_shctx_lock(&cache->dict[0]);
    entry = cache_dict_get(key, hash);
    if(entry && entry->state == CACHE_ENTRY_STATE_VALID) {
        entry->state         = CACHE_ENTRY_STATE_EXPIRED;
        entry->data->invalid = 1;
        entry->data          = NULL;
        entry->expire        = 0;
        ret                  = 200;
    } else {
        ret = 404;
    }
    nuster_shctx_unlock(&cache->dict[0]);

    return ret;
}

void cache_response(struct stream *s, struct chunk *msg) {
    s->txn->flags &= ~TX_WAIT_NEXT_RQ;
    stream_int_retnclose(&s->si[0], msg);
    if(!(s->flags & SF_ERR_MASK)) {
        s->flags |= SF_ERR_LOCAL;
    }
}

int cache_purge(struct stream *s, struct channel *req, struct proxy *px) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;


    if(txn->meth == HTTP_METH_OTHER &&
            memcmp(msg->chn->buf->p, global.cache.purge_method, strlen(global.cache.purge_method)) == 0) {

        char *key = cache_build_purge_key(s, msg);
        if(!key) {
            txn->status = 500;
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_500]);
        } else {
            uint64_t hash = cache_hash_key(key);
            txn->status = cache_purge_by_key(key, hash);
            if(txn->status == 200) {
                cache_response(s, &cache_msg_chunks[NUSTER_CACHE_200]);
            } else {
                cache_response(s, &cache_msg_chunks[NUSTER_CACHE_404]);
            }
        }
        return 1;
    }
    return 0;
}

int cache_manager_state_ttl(struct stream *s, struct channel *req, struct proxy *px, int state, int ttl) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;
    int found, mode      = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
    struct hdr_ctx ctx;
    struct proxy *p;

    if(state == -1 && ttl == -1) {
        return 400;
    }

    ctx.idx = 0;
    if(http_find_header2("name", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        if(ctx.vlen == 1 && !memcmp(ctx.line + ctx.val, "*", 1)) {
            found = 1;
            mode  = NUSTER_CACHE_PURGE_MODE_NAME_ALL;
        }
        p = proxy;
        while(p) {
            struct cache_rule *rule = NULL;

            if(mode != NUSTER_CACHE_PURGE_MODE_NAME_ALL && strlen(p->id) == ctx.vlen && !memcmp(ctx.line + ctx.val, p->id, ctx.vlen)) {
                found = 1;
                mode  = NUSTER_CACHE_PURGE_MODE_NAME_PROXY;
            }

            list_for_each_entry(rule, &p->cache_rules, list) {
                if(mode != NUSTER_CACHE_PURGE_MODE_NAME_RULE) {
                    *rule->state = state == -1 ? *rule->state : state;
                    *rule->ttl   = ttl   == -1 ? *rule->ttl   : ttl;
                } else if(strlen(rule->name) == ctx.vlen && !memcmp(ctx.line + ctx.val, rule->name, ctx.vlen)) {
                    *rule->state = state == -1 ? *rule->state : state;
                    *rule->ttl   = ttl   == -1 ? *rule->ttl   : ttl;
                    found        = 1;
                }
            }
            if(mode == NUSTER_CACHE_PURGE_MODE_NAME_PROXY) {
                break;
            }
            p = p->next;
        }
        if(found) {
            return 200;
        } else {
            return 404;
        }
    }

    return 400;
}

static inline int cache_manager_purge_method(struct http_txn *txn, struct http_msg *msg) {
    return txn->meth == HTTP_METH_OTHER &&
            memcmp(msg->chn->buf->p, global.cache.purge_method, strlen(global.cache.purge_method)) == 0;
}

int cache_manager_purge(struct stream *s, struct channel *req, struct proxy *px) {
    struct stream_interface *si = &s->si[1];
    struct http_txn *txn        = s->txn;
    struct http_msg *msg        = &txn->req;
    struct appctx *appctx       = NULL;
    int mode                    = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
    int st1                     = 0;
    char *host                  = NULL;
    char *path                  = NULL;
    struct my_regex *regex      = NULL;
    char *error                 = NULL;
    char *regex_str             = NULL;
    int host_len                = 0;
    int path_len                = 0;
    struct hdr_ctx ctx;
    struct proxy *p;

    ctx.idx = 0;
    if(http_find_header2("x-host", 6, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        host     = ctx.line + ctx.val;
        host_len = ctx.vlen;
    }

    ctx.idx = 0;
    if(http_find_header2("name", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        if(ctx.vlen == 1 && !memcmp(ctx.line + ctx.val, "*", 1)) {
            mode = NUSTER_CACHE_PURGE_MODE_NAME_ALL;
            goto purge;
        }

        p = proxy;
        while(p) {
            struct cache_rule *rule = NULL;

            if(mode != NUSTER_CACHE_PURGE_MODE_NAME_ALL && strlen(p->id) == ctx.vlen && !memcmp(ctx.line + ctx.val, p->id, ctx.vlen)) {
                mode = NUSTER_CACHE_PURGE_MODE_NAME_PROXY;
                st1  = p->uuid;
                goto purge;
            }

            list_for_each_entry(rule, &p->cache_rules, list) {
                if(strlen(rule->name) == ctx.vlen && !memcmp(ctx.line + ctx.val, rule->name, ctx.vlen)) {
                    mode = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
                    st1  = rule->id;
                    goto purge;
                }
            }
            p = p->next;
        }

        goto notfound;
    } else if(http_find_header2("path", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        path      = ctx.line + ctx.val;
        path_len  = ctx.vlen;
        mode      = host ? NUSTER_CACHE_PURGE_MODE_PATH_HOST : NUSTER_CACHE_PURGE_MODE_PATH;
    } else if(http_find_header2("regex", 5, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        regex_str = malloc(ctx.vlen + 1);
        regex     = calloc(1, sizeof(*regex));
        if(!regex_str || !regex) {
            goto err;
        }

        memcpy(regex_str, ctx.line + ctx.val, ctx.vlen);
        regex_str[ctx.vlen] = '\0';

        if (!regex_comp(regex_str, regex, 1, 0, &error)) {
            goto err;
        }
        free(regex_str);

        mode = host ? NUSTER_CACHE_PURGE_MODE_REGEX_HOST : NUSTER_CACHE_PURGE_MODE_REGEX;
    } else if(host) {
        mode = NUSTER_CACHE_PURGE_MODE_HOST;
    } else {
        goto badreq;
    }

purge:
    s->target = &cache_manager_applet.obj_type;
    if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
        goto err;
    } else {
        appctx      = si_appctx(si);
        memset(&appctx->ctx.cache_manager, 0, sizeof(appctx->ctx.cache_manager));
        appctx->st0 = mode;
        appctx->st1 = st1;
        appctx->st2 = 0;

        if(mode == NUSTER_CACHE_PURGE_MODE_HOST ||
                mode == NUSTER_CACHE_PURGE_MODE_PATH_HOST ||
                mode == NUSTER_CACHE_PURGE_MODE_REGEX_HOST) {
            appctx->ctx.cache_manager.host     = nuster_memory_alloc(global.cache.memory, host_len);
            appctx->ctx.cache_manager.host_len = host_len;
            if(!appctx->ctx.cache_manager.host) {
                goto err;
            }
            memcpy(appctx->ctx.cache_manager.host, host, host_len);
        }

        if(mode == NUSTER_CACHE_PURGE_MODE_PATH ||
                mode == NUSTER_CACHE_PURGE_MODE_PATH_HOST) {
            appctx->ctx.cache_manager.path     = nuster_memory_alloc(global.cache.memory, path_len);
            appctx->ctx.cache_manager.path_len = path_len;
            if(!appctx->ctx.cache_manager.path) {
                goto err;
            }
            memcpy(appctx->ctx.cache_manager.path, path, path_len);
        } else if(mode == NUSTER_CACHE_PURGE_MODE_REGEX ||
                mode == NUSTER_CACHE_PURGE_MODE_REGEX_HOST) {
            appctx->ctx.cache_manager.regex = regex;
        }

        req->analysers &= (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;
        req->analysers |= AN_REQ_HTTP_XFER_BODY;
    }

    return 0;
notfound:
    return 404;
err:
    free(error);
    free(regex_str);
    if(regex) {
        regex_free(regex);
    }
    return 500;
badreq:
    return 400;
}

/*
 * return 1 if the request is done, otherwise 0
 */
int cache_manager(struct stream *s, struct channel *req, struct proxy *px) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;
    int state            = -1;
    int ttl              = -1;
    struct hdr_ctx ctx;

    if(global.cache.status != CACHE_STATUS_ON) {
        return 0;
    }

    if(txn->meth == HTTP_METH_POST) {
        /* POST */
        if(cache_check_uri(msg)) {
            /* manager uri */
            ctx.idx = 0;
            if(http_find_header2("state", 5, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
                if(ctx.vlen == 6 && !memcmp(ctx.line + ctx.val, "enable", 6)) {
                    state = CACHE_RULE_ENABLED;
                } else if(ctx.vlen == 7 && !memcmp(ctx.line + ctx.val, "disable", 7)) {
                    state = CACHE_RULE_DISABLED;
                }
            }
            ctx.idx = 0;
            if(http_find_header2("ttl", 3, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
                cache_parse_time(ctx.line + ctx.val, ctx.vlen, (unsigned *)&ttl);
            }

            txn->status = cache_manager_state_ttl(s, req, px, state, ttl);
        } else {
            return 0;
        }
    } else if(cache_manager_purge_method(txn, msg)) {
        /* purge */
        if(cache_check_uri(msg)) {
            /* manager uri */
            txn->status = cache_manager_purge(s, req, px);
            if(txn->status == 0) {
                return 0;
            }
        } else {
            /* single uri */
            return cache_purge(s, req, px);
        }
    } else {
        return 0;
    }

    switch(txn->status) {
        case 200:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_200]);
            break;
        case 400:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_400]);
            break;
        case 404:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_404]);
            break;
        case 500:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_500]);
            break;
        default:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_400]);
    }
    return 1;
}


static int _cache_manager_should_purge(struct cache_entry *entry, struct appctx *appctx) {
    int ret = 0;
    switch(appctx->st0) {
        case NUSTER_CACHE_PURGE_MODE_NAME_ALL:
            ret = 1;
            break;
        case NUSTER_CACHE_PURGE_MODE_NAME_PROXY:
            ret = entry->pid == appctx->st1;
            break;
        case NUSTER_CACHE_PURGE_MODE_NAME_RULE:
            ret = entry->rule->id == appctx->st1;
            break;
        case NUSTER_CACHE_PURGE_MODE_PATH:
            ret = entry->path.len == appctx->ctx.cache_manager.path_len &&
                !memcmp(entry->path.data, appctx->ctx.cache_manager.path, entry->path.len);
            break;
        case NUSTER_CACHE_PURGE_MODE_REGEX:
            ret = regex_exec(appctx->ctx.cache_manager.regex, entry->path.data);
            break;
        case NUSTER_CACHE_PURGE_MODE_HOST:
            ret = entry->host.len == appctx->ctx.cache_manager.host_len &&
                !memcmp(entry->host.data, appctx->ctx.cache_manager.host, entry->host.len);
            break;
        case NUSTER_CACHE_PURGE_MODE_PATH_HOST:
            ret = entry->path.len == appctx->ctx.cache_manager.path_len &&
                entry->host.len == appctx->ctx.cache_manager.host_len &&
                !memcmp(entry->path.data, appctx->ctx.cache_manager.path, entry->path.len) &&
                !memcmp(entry->host.data, appctx->ctx.cache_manager.host, entry->host.len);
            break;
        case NUSTER_CACHE_PURGE_MODE_REGEX_HOST:
            ret = entry->host.len == appctx->ctx.cache_manager.host_len &&
                !memcmp(entry->host.data, appctx->ctx.cache_manager.host, entry->host.len) &&
                regex_exec(appctx->ctx.cache_manager.regex, entry->path.data);
            break;
    }
    return ret;
}

static void cache_manager_handler(struct appctx *appctx) {
    struct stream_interface *si = appctx->owner;
    struct channel *res         = si_ic(si);
    struct stream *s            = si_strm(si);
    struct cache_entry *entry   = NULL;
    int max                     = 1000;
    uint64_t start              = get_current_timestamp();

    while(1) {
        nuster_shctx_lock(&cache->dict[0]);
        while(appctx->st2 < cache->dict[0].size && max--) {
            entry = cache->dict[0].entry[appctx->st2];
            while(entry) {
                if(entry->state == CACHE_ENTRY_STATE_VALID && _cache_manager_should_purge(entry, appctx)) {
                    entry->state         = CACHE_ENTRY_STATE_INVALID;
                    entry->data->invalid = 1;
                    entry->data          = NULL;
                    entry->expire        = 0;
                }
                entry = entry->next;
            }
            appctx->st2++;
        }
        nuster_shctx_unlock(&cache->dict[0]);
        if(get_current_timestamp() - start > 1) break;
        max = 1000;
    }
    task_wakeup(s->task, TASK_WOKEN_OTHER);

    if(appctx->st2 == cache->dict[0].size) {
        bi_putblk(res, cache_msgs[NUSTER_CACHE_200], strlen(cache_msgs[NUSTER_CACHE_200]));
        bo_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
    }
}

static void cache_manager_release_handler(struct appctx *appctx) {
    if(appctx->ctx.cache_manager.regex) {
        regex_free(appctx->ctx.cache_manager.regex);
        free(appctx->ctx.cache_manager.regex);
    }
    if(appctx->ctx.cache_manager.host) {
        nuster_memory_free(global.cache.memory, appctx->ctx.cache_manager.host);
    }
    if(appctx->ctx.cache_manager.path) {
        nuster_memory_free(global.cache.memory, appctx->ctx.cache_manager.path);
    }
}

struct applet cache_manager_applet = {
    .obj_type = OBJ_TYPE_APPLET,
    .name = "<CACHE-MANAGER>",
    .fct = cache_manager_handler,
    .release = cache_manager_release_handler,
};

