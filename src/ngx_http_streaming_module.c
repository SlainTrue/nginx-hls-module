/*******************************************************************************
 ngx_http_streaming - An Nginx module for streaming MPEG4 files

 For licensing see the LICENSE file
******************************************************************************/

#include "ngx_http_streaming_module.h"
#include "mp4_io.h"
#include "mp4_reader.h"
#include "moov.h"
#include "output_bucket.h"
#include "view_count.h"
#include "output_m3u8.h"
#include "output_ts.h"
#include "mod_streaming_export.h"

static void *ngx_http_hls_create_conf(ngx_conf_t *cf) {
    hls_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(hls_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->hash = { NULL };
     *     conf->server_names = 0;
     *     conf->keys = NULL;
     */

    conf->length = NGX_CONF_UNSET_UINT;
    conf->relative = NGX_CONF_UNSET;
    conf->buffer_size = NGX_CONF_UNSET_SIZE;
    conf->max_buffer_size = NGX_CONF_UNSET_SIZE;

    return conf;
}

static char *ngx_http_hls_merge_conf(ngx_conf_t *cf, void *parent, void *child) {
    hls_conf_t *prev = parent;
    hls_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->length, prev->length, 8);
    ngx_conf_merge_value(conf->relative, prev->relative, 1);
    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size, 512 * 1024);
    ngx_conf_merge_size_value(conf->max_buffer_size, prev->max_buffer_size,
                              10 * 1024 * 1024);

    if(conf->length < 1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "ngx_length must be equal or more than 1");
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t ngx_streaming_handler(ngx_http_request_t *r) {
  size_t                      root;
  ngx_int_t                   rc;
  ngx_uint_t                  level;
  ngx_str_t                   path;
  ngx_open_file_info_t        of;
  ngx_http_core_loc_conf_t    *clcf;

  if(!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD)))
    return NGX_HTTP_NOT_ALLOWED;

  if(r->uri.data[r->uri.len - 1] == '/')
    return NGX_DECLINED;

  rc = ngx_http_discard_request_body(r);

  if(rc != NGX_OK)
    return rc;

  mp4_split_options_t *options = mp4_split_options_init(r);

  if(r->args.len && !mp4_split_options_set(r, options, (const char *)r->args.data, r->args.len)) {
    mp4_split_options_exit(r, options);
    return NGX_DECLINED;
  }

  if(!options) return NGX_DECLINED;

  if(!ngx_http_map_uri_to_path(r, &path, &root, 1)) {
    mp4_split_options_exit(r, options);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  ngx_log_t *nlog = r->connection->log;

  u_int m3u8 = 0;

  struct bucket_t *bucket = bucket_init(r);
  int result = 0;
  {
    if(ngx_strstr(path.data, "m3u8")) m3u8 = 1;
    char *ext = strrchr((const char *)path.data, '.');
    strcpy(ext, ".mp4");
    path.len = ((u_char *)ext - path.data) + 4;
    // ngx_open_and_stat_file in ngx_open_cached_file expects the name to be zero-terminated.
    path.data[path.len] = '\0';
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, nlog, 0, "http mp4 filename: \"%s\"", path.data);

  clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

  ngx_memzero(&of, sizeof(ngx_open_file_info_t));

  of.read_ahead = clcf->read_ahead;
  of.directio = NGX_MAX_OFF_T_VALUE;
  of.valid = clcf->open_file_cache_valid;
  of.min_uses = clcf->open_file_cache_min_uses;
  of.errors = clcf->open_file_cache_errors;
  of.events = clcf->open_file_cache_events;

  if(ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool) != NGX_OK) {
    mp4_split_options_exit(r, options);
    switch(of.err) {
    case 0:
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    case NGX_ENOENT:
    case NGX_ENOTDIR:
    case NGX_ENAMETOOLONG:
      level = NGX_LOG_ERR;
      rc = NGX_HTTP_NOT_FOUND;
      break;
    case NGX_EACCES:
      level = NGX_LOG_ERR;
      rc = NGX_HTTP_FORBIDDEN;
      break;
    default:
      level = NGX_LOG_CRIT;
      rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
      break;
    }

    if(rc != NGX_HTTP_NOT_FOUND || clcf->log_not_found) {
      ngx_log_error(level, nlog, of.err,
                    ngx_open_file_n " \"%s\" failed", path.data);
    }

    return rc;
  }

  if(!of.is_file) {
    mp4_split_options_exit(r, options);
    if(ngx_close_file(of.fd) == NGX_FILE_ERROR) {
      ngx_log_error(NGX_LOG_ALERT, nlog, ngx_errno,
                    ngx_close_file_n " \"%s\" failed", path.data);
    }
    return NGX_DECLINED;
  }

  ngx_file_t *file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
  if(file == NULL) {
    mp4_split_options_exit(r, options);
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  file->fd = of.fd;
  file->name = path;
  file->log = nlog;

  mp4_context_t *mp4_context = mp4_open(r, file, of.size, MP4_OPEN_MOOV);
  if(!mp4_context) {
    mp4_split_options_exit(r, options);
    ngx_log_error(NGX_LOG_ALERT, nlog, ngx_errno, "mp4_open failed");
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  mp4_context->root = root;
  if(m3u8) {
    if((result = mp4_create_m3u8(mp4_context, bucket))) {
      char action[50];
      sprintf(action, "ios_playlist&segments=%d", result);
      view_count(mp4_context, (char *)path.data, options ? options->hash : NULL, action);
    }
    r->allow_ranges = 0;
    r->headers_out.content_type.data = "application/vnd.apple.mpegurl";
    r->headers_out.content_type.len = 29;
    r->headers_out.content_type_len = r->headers_out.content_type.len;
  } else {
    result = output_ts(mp4_context, bucket, options);
    if(!options || !result) {
      mp4_close(mp4_context);
      ngx_log_error(NGX_LOG_ALERT, nlog, ngx_errno, "output_ts failed");
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    char action[50] = "ios_view";
    view_count(mp4_context, (char *)path.data, options->hash, action);
    r->allow_ranges = 1;
  }

  mp4_close(mp4_context);
  mp4_split_options_exit(r, options);

  result = result == 0 ? 415 : 200;

  r->root_tested = !r->error_page;

  if(result && bucket) {
    nlog->action = "sending mp4 to client";

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, nlog, 0, "content_length: %d", bucket->content_length);
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = bucket->content_length;
    r->headers_out.last_modified_time = of.mtime;

    ngx_table_elt_t *h = ngx_list_push(&r->headers_out.headers);
    if(h == NULL) return NGX_HTTP_INTERNAL_SERVER_ERROR;

    h->hash = 1;

    h->key.len = sizeof(X_MOD_HLS_KEY) - 1;
    h->key.data = (u_char *)X_MOD_HLS_KEY;
    h->value.len = sizeof(X_MOD_HLS_VERSION) - 1;
    h->value.data = (u_char *)X_MOD_HLS_VERSION;

    rc = ngx_http_send_header(r);

    if(rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
      ngx_log_error(NGX_LOG_ALERT, nlog, ngx_errno, ngx_close_file_n "ngx_http_send_header failed");
      return rc;
    }

    return ngx_http_output_filter(r, bucket->first);
  } else return NGX_HTTP_UNSUPPORTED_MEDIA_TYPE;
}

static char *ngx_streaming(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
  ngx_http_core_loc_conf_t *clcf =
    ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

  clcf->handler = ngx_streaming_handler;

  return NGX_CONF_OK;
}

// End Of File

