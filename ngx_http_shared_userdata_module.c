#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#define HELLO_WORLD "i love nginx modules"

static ngx_uint_t ngx_http_upstream_fair_shm_size;
static char *ngx_http_shared_userdata(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_init_shared_userdata(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_shared_userdata_handler(ngx_http_request_t *r);

typedef struct {
    ngx_uint_t                          nreq;
    ngx_uint_t                          total_req;
    ngx_uint_t                          last_req_id;
    ngx_uint_t                          fails;
    ngx_uint_t                          current_weight;
} ngx_http_upstream_fair_shared_t;

typedef struct {
    ngx_rbtree_node_t                   node;
    ngx_uint_t                          generation;
    uintptr_t                           peers;      /* forms a unique cookie together with generation */
    ngx_uint_t                          total_nreq;
    ngx_uint_t                          total_requests;
    ngx_atomic_t                        lock;
    ngx_http_upstream_fair_shared_t     stats[1];
} ngx_http_upstream_fair_shm_block_t;

/* ngx_spinlock is defined without a matching unlock primitive */
#define ngx_spinlock_unlock(lock)       (void) ngx_atomic_cmp_set(lock, ngx_pid, 0)

#define NGX_HTTP_UPSTREAM_FAIR_NO_RR            (1<<26)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_IDLE (1<<27)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_PEAK (1<<28)
#define NGX_HTTP_UPSTREAM_FAIR_WEIGHT_MODE_MASK ((1<<27) | (1<<28))

enum { WM_DEFAULT = 0, WM_IDLE, WM_PEAK };

#define NGX_PEER_INVALID (~0UL)

static ngx_shm_zone_t * ngx_http_upstream_fair_shm_zone;

#define NGX_BITVECTOR_ELT_SIZE (sizeof(uintptr_t) * 8)

static ngx_int_t
ngx_http_upstream_fair_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t                *shpool;

	ngx_str_t *msg;

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    msg = ngx_slab_alloc(shpool, 10);
	msg->len = 10;
	msg->data = (u_char *) "0123456789";

	if(shm_zone->data == NULL) {
		shm_zone->data = msg;
	}

    return NGX_OK;
}

/* vim: set et ts=4 sw=4: */

/**
 * This module provided directive: hello world.
 *
 */
static ngx_command_t ngx_http_shared_userdata_commands[] = {

    { ngx_string("shared_userdata"), /* directive */
	  // {ngx_string("mruby_init"), NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE12, ngx_http_mruby_init_phase,}
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS, /* location context and takes
                                            no arguments*/
      ngx_http_shared_userdata, /* configuration setup function */
      0, /* No offset. Only one context is supported. */
      0, /* No offset when storing the module configuration on struct. */
      NULL},
    { ngx_string("init_shared_userdata"), /* directive */
	  // {ngx_string("mruby_init"), NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE12, ngx_http_mruby_init_phase,}
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1, /* location context and takes
                                            no arguments*/
      ngx_http_init_shared_userdata, /* configuration setup function */
      0, /* No offset. Only one context is supported. */
      0, /* No offset when storing the module configuration on struct. */
      NULL},

    ngx_null_command /* command termination */
};

/* The hello world string. */
/* static u_char ngx_shared_userdata[] = HELLO_WORLD; */

/* The module context. */
static ngx_http_module_t ngx_http_shared_userdata_module_ctx = {
    NULL, /* preconfiguration */
    NULL, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    NULL, /* create location configuration */
    NULL /* merge location configuration */
};

/* Module definition. */
// mruby-shared-userdata-module
ngx_module_t ngx_http_shared_userdata_module = {
    NGX_MODULE_V1,
    &ngx_http_shared_userdata_module_ctx, /* module context */
    ngx_http_shared_userdata_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

/**
 * Content handler.
 *
 * @param r
 *   Pointer to the request structure. See http_request.h.
 * @return
 *   The status of the response generation.
 */
static ngx_int_t ngx_http_shared_userdata_handler(ngx_http_request_t *r)
{
    ngx_buf_t *b;
    ngx_chain_t out;

	sleep(1);

    /* Set the Content-Type header. */
    r->headers_out.content_type.len = sizeof("text/plain") - 1;
    r->headers_out.content_type.data = (u_char *) "text/plain";

    /* Allocate a new buffer for sending out the reply. */
    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));

    /* Insertion in the buffer chain. */
    out.buf = b;
    out.next = NULL; /* just one buffer */

	// FIXME: dereference of pointers
	ngx_str_t *hello;
	hello = (ngx_str_t *)ngx_http_upstream_fair_shm_zone->data;

    b->pos = hello->data; /* first position in memory of the data */
    b->last = hello->data + 5; /* last position in memory of the data */
    b->memory = 1; /* content is in read-only memory */
    b->last_buf = 1; /* there will be no more buffers in the request */

    /* Sending the headers for the reply. */
    r->headers_out.status = NGX_HTTP_OK; /* 200 status code */
    /* Get the content length of the body. */
    r->headers_out.content_length_n = 5;
    ngx_http_send_header(r); /* Send the headers */

    /* Send the body, and return the status code of the output filter chain. */
    return ngx_http_output_filter(r, &out);
} /* ngx_http_hello_world_handler */

/**
 * Configuration setup function that installs the content handler.
 *
 * @param cf
 *   Module configuration structure pointer.
 * @param cmd
 *   Module directives structure pointer.
 * @param conf
 *   Module configuration structure pointer.
 * @return string
 *   Status of the configuration setup.
 */
static char *ngx_http_shared_userdata(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf; /* pointer to core location configuration */

    /* Install the hello world handler. */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_shared_userdata_handler;

    return NGX_CONF_OK;
} /* ngx_http_hello_world */

static char *ngx_http_init_shared_userdata(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf; /* pointer to core location configuration */
	ngx_str_t                      *value;

    /* Install the hello world handler. */
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_shared_userdata_handler;
	value = cf->args->elts;

	/*
	 * create a shared memory pool
	 * */
	ngx_str_t                          *shm_name;
	shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
	shm_name->len = sizeof("upstream_fair") - 1;
	shm_name->data = (unsigned char *) "upstream_fair";

	ngx_http_upstream_fair_shm_size = ngx_parse_size(&value[1]);
	ngx_http_upstream_fair_shm_zone = ngx_shared_memory_add(cf, shm_name, ngx_http_upstream_fair_shm_size, &ngx_http_shared_userdata_module);
	ngx_http_upstream_fair_shm_zone->init = ngx_http_upstream_fair_init_shm_zone;


    return NGX_CONF_OK;
} /* ngx_http_hello_world */
