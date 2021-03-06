
/*
 * Copyright (C) YoungJoo Kim (vozlt)
 */


#include "ngx_http_vhost_traffic_status_module.h"
#include "ngx_http_vhost_traffic_status_node.h"


ngx_int_t
ngx_http_vhost_traffic_status_node_generate_key(ngx_pool_t *pool,
    ngx_str_t *buf, ngx_str_t *dst, unsigned type)
{
    size_t   len;
    u_char  *p;

    len = ngx_strlen(ngx_http_vhost_traffic_status_group_to_string(type));

    buf->len = len + sizeof("@") - 1 + dst->len;
    buf->data = ngx_pcalloc(pool, buf->len);
    if (buf->data == NULL) {
        *buf = *dst;
        return NGX_ERROR;
    }

    p = buf->data;

    p = ngx_cpymem(p, ngx_http_vhost_traffic_status_group_to_string(type), len);
    *p++ = NGX_HTTP_VHOST_TRAFFIC_STATUS_KEY_SEPARATOR;
    p = ngx_cpymem(p, dst->data, dst->len);

    return NGX_OK;
}


ngx_int_t
ngx_http_vhost_traffic_status_node_position_key(ngx_str_t *buf, size_t pos)
{
    size_t   n, c, len;
    u_char  *p, *s;

    n = buf->len + 1;
    c = len = 0;
    p = s = buf->data;

    while (--n) {
        if (*p == NGX_HTTP_VHOST_TRAFFIC_STATUS_KEY_SEPARATOR) {
            if (pos == c) {
                break;
            }
            s = (p + 1);
            c++;
        }
        p++;
        len = (p - s);
    }

    if (pos > c || len == 0) {
        return NGX_ERROR;
    }

    buf->data = s;
    buf->len = len;

    return NGX_OK;
}


ngx_int_t
ngx_http_vhost_traffic_status_node_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                                    *p;
    unsigned                                   type;
    ngx_int_t                                  rc;
    ngx_str_t                                  key, dst;
    ngx_slab_pool_t                           *shpool;
    ngx_rbtree_node_t                         *node;
    ngx_http_vhost_traffic_status_node_t      *vtsn;
    ngx_http_vhost_traffic_status_loc_conf_t  *vtscf;

    vtscf = ngx_http_get_module_loc_conf(r, ngx_http_vhost_traffic_status_module);

    ngx_http_vhost_traffic_status_find_name(r, &dst);

    type = NGX_HTTP_VHOST_TRAFFIC_STATUS_UPSTREAM_NO;

    rc = ngx_http_vhost_traffic_status_node_generate_key(r->pool, &key, &dst, type);
    if (rc != NGX_OK) {
        return NGX_ERROR;
    }

    if (key.len == 0) {
        return NGX_ERROR;
    }

    shpool = (ngx_slab_pool_t *) vtscf->shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);

    node = ngx_http_vhost_traffic_status_find_node(r, &key, type, 0);

    if (node == NULL) {
        goto not_found;
    }

    p = ngx_pnalloc(r->pool, NGX_ATOMIC_T_LEN);
    if (p == NULL) {
        goto not_found;
    }

    vtsn = (ngx_http_vhost_traffic_status_node_t *) &node->color;

    v->len = ngx_sprintf(p, "%uA", *((ngx_atomic_t *) ((char *) vtsn + data))) - p;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;
    v->data = p;

    goto done;

not_found:

    v->not_found = 1;

done:

    vtscf->node_caches[type] = node;

    ngx_shmtx_unlock(&shpool->mutex);

    return NGX_OK;
}


void
ngx_http_vhost_traffic_status_find_name(ngx_http_request_t *r,
    ngx_str_t *buf)
{
    ngx_http_core_srv_conf_t                  *cscf;
    ngx_http_vhost_traffic_status_loc_conf_t  *vtscf;

    cscf = ngx_http_get_module_srv_conf(r, ngx_http_core_module);
    vtscf = ngx_http_get_module_loc_conf(r, ngx_http_vhost_traffic_status_module);

    if (vtscf->filter && vtscf->filter_host && r->headers_in.server.len) {
        /* set the key by host header */
        *buf = r->headers_in.server;

    } else {
        /* set the key by server_name variable */
        *buf = cscf->server_name;

        if (buf->len == 0) {
            buf->len = 1;
            buf->data = (u_char *) "_";
        }
    }
}


ngx_rbtree_node_t *
ngx_http_vhost_traffic_status_find_node(ngx_http_request_t *r,
    ngx_str_t *key, unsigned type, uint32_t key_hash)
{
    uint32_t                                   hash;
    ngx_rbtree_node_t                         *node;
    ngx_http_vhost_traffic_status_ctx_t       *ctx;
    ngx_http_vhost_traffic_status_loc_conf_t  *vtscf;

    ctx = ngx_http_get_module_main_conf(r, ngx_http_vhost_traffic_status_module);
    vtscf = ngx_http_get_module_loc_conf(r, ngx_http_vhost_traffic_status_module);

    hash = key_hash;

    if (hash == 0) {
        hash = ngx_crc32_short(key->data, key->len);
    }

    if (vtscf->node_caches[type] != NULL) {
        if (vtscf->node_caches[type]->key == hash) {
            node = vtscf->node_caches[type];
            goto found;
        }
    }

    node = ngx_http_vhost_traffic_status_node_lookup(ctx->rbtree, key, hash);

found:

    return node;
}


ngx_rbtree_node_t *
ngx_http_vhost_traffic_status_node_lookup(ngx_rbtree_t *rbtree, ngx_str_t *key,
    uint32_t hash)
{
    ngx_int_t                              rc;
    ngx_rbtree_node_t                     *node, *sentinel;
    ngx_http_vhost_traffic_status_node_t  *vtsn;

    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        /* hash == node->key */

        vtsn = (ngx_http_vhost_traffic_status_node_t *) &node->color;

        rc = ngx_memn2cmp(key->data, vtsn->data, key->len, (size_t) vtsn->len);
        if (rc == 0) {
            return node;
        }

        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}


void
ngx_http_vhost_traffic_status_node_zero(ngx_http_vhost_traffic_status_node_t *vtsn)
{
    vtsn->stat_request_counter = 0;
    vtsn->stat_in_bytes = 0;
    vtsn->stat_out_bytes = 0;
    vtsn->stat_1xx_counter = 0;
    vtsn->stat_2xx_counter = 0;
    vtsn->stat_3xx_counter = 0;
    vtsn->stat_4xx_counter = 0;
    vtsn->stat_5xx_counter = 0;

    vtsn->stat_request_time = 0;

    vtsn->stat_request_counter_oc = 0;
    vtsn->stat_in_bytes_oc = 0;
    vtsn->stat_out_bytes_oc = 0;
    vtsn->stat_1xx_counter_oc = 0;
    vtsn->stat_2xx_counter_oc = 0;
    vtsn->stat_3xx_counter_oc = 0;
    vtsn->stat_4xx_counter_oc = 0;
    vtsn->stat_5xx_counter_oc = 0;

#if (NGX_HTTP_CACHE)
    vtsn->stat_cache_miss_counter = 0;
    vtsn->stat_cache_bypass_counter = 0;
    vtsn->stat_cache_expired_counter = 0;
    vtsn->stat_cache_stale_counter = 0;
    vtsn->stat_cache_updating_counter = 0;
    vtsn->stat_cache_revalidated_counter = 0;
    vtsn->stat_cache_hit_counter = 0;
    vtsn->stat_cache_scarce_counter = 0;

    vtsn->stat_cache_miss_counter_oc = 0;
    vtsn->stat_cache_bypass_counter_oc = 0;
    vtsn->stat_cache_expired_counter_oc = 0;
    vtsn->stat_cache_stale_counter_oc = 0;
    vtsn->stat_cache_updating_counter_oc = 0;
    vtsn->stat_cache_revalidated_counter_oc = 0;
    vtsn->stat_cache_hit_counter_oc = 0;
    vtsn->stat_cache_scarce_counter_oc = 0;
#endif

    vtsn->stat_upstream.rtms = 0;
}


void
ngx_http_vhost_traffic_status_node_init(ngx_http_request_t *r,
    ngx_http_vhost_traffic_status_node_t *vtsn)
{
    ngx_uint_t status = r->headers_out.status;

    ngx_http_vhost_traffic_status_node_zero(vtsn);

    vtsn->stat_upstream.type = NGX_HTTP_VHOST_TRAFFIC_STATUS_UPSTREAM_NO;
    vtsn->stat_request_counter = 1;
    vtsn->stat_in_bytes = (ngx_atomic_uint_t) r->request_length;
    vtsn->stat_out_bytes = (ngx_atomic_uint_t) r->connection->sent;

    ngx_http_vhost_traffic_status_add_rc(status, vtsn);

    vtsn->stat_request_time = (ngx_msec_t) ngx_http_vhost_traffic_status_request_time(r);

#if (NGX_HTTP_CACHE)
    if (r->upstream != NULL && r->upstream->cache_status != 0) {
        ngx_http_vhost_traffic_status_add_cc(r->upstream->cache_status, vtsn);
    }
#endif

}


void
ngx_http_vhost_traffic_status_node_set(ngx_http_request_t *r,
    ngx_http_vhost_traffic_status_node_t *vtsn)
{
    ngx_uint_t                            status;
    ngx_msec_int_t                        ms;
    ngx_http_vhost_traffic_status_node_t  ovtsn;

    status = r->headers_out.status;
    ovtsn = *vtsn;

    vtsn->stat_request_counter++;
    vtsn->stat_in_bytes += (ngx_atomic_uint_t) r->request_length;
    vtsn->stat_out_bytes += (ngx_atomic_uint_t) r->connection->sent;

    ngx_http_vhost_traffic_status_add_rc(status, vtsn);

    ms = ngx_http_vhost_traffic_status_request_time(r);

    vtsn->stat_request_time = (ngx_msec_t)
                                  (((ngx_msec_int_t) vtsn->stat_request_time + ms) / 2
                                  + ((ngx_msec_int_t) vtsn->stat_request_time + ms) % 2);

#if (NGX_HTTP_CACHE)
    if (r->upstream != NULL && r->upstream->cache_status != 0) {
        ngx_http_vhost_traffic_status_add_cc(r->upstream->cache_status, vtsn);
    }
#endif

    ngx_http_vhost_traffic_status_add_oc((&ovtsn), vtsn);
}


ngx_int_t
ngx_http_vhost_traffic_status_node_member_cmp(ngx_str_t *member, const char *name)
{
    if (member->len == ngx_strlen(name) && ngx_strncmp(name, member->data, member->len) == 0) {
        return 0;
    }

    return 1;
}


ngx_atomic_uint_t
ngx_http_vhost_traffic_status_node_member(ngx_http_vhost_traffic_status_node_t *vtsn,
    ngx_str_t *member)
{
    if (ngx_http_vhost_traffic_status_node_member_cmp(member, "request") == 0)
    {
        return vtsn->stat_request_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "in") == 0)
    {
        return vtsn->stat_in_bytes;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "out") == 0)
    {
        return vtsn->stat_out_bytes;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "1xx") == 0)
    {
        return vtsn->stat_1xx_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "2xx") == 0)
    {
        return vtsn->stat_2xx_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "3xx") == 0)
    {
        return vtsn->stat_3xx_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "4xx") == 0)
    {
        return vtsn->stat_4xx_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "5xx") == 0)
    {
        return vtsn->stat_5xx_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_miss") == 0)
    {
        return vtsn->stat_cache_miss_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_bypass") == 0)
    {
        return vtsn->stat_cache_bypass_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_expired") == 0)
    {
        return vtsn->stat_cache_expired_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_stale") == 0)
    {
        return vtsn->stat_cache_stale_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_updating") == 0)
    {
        return vtsn->stat_cache_updating_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_revalidated") == 0)
    {
        return vtsn->stat_cache_revalidated_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_hit") == 0)
    {
        return vtsn->stat_cache_hit_counter;
    }
    else if (ngx_http_vhost_traffic_status_node_member_cmp(member, "cache_scarce") == 0)
    {
        return vtsn->stat_cache_scarce_counter;
    }

    return 0;
}

/* vi:set ft=c ts=4 sw=4 et fdm=marker: */
