/* Mojo::MT::KeyDB::XS - Tier-1 thread-safe C/NEON KeyDB/RESP2/RESP3 client.
 * Compiled with -DPERL_XS_THREADSAFE=1 and PERL_NO_GET_CONTEXT.
 * Zero mutable global state: all per-worker connections and buffers live in MY_CXT.
 */

#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "mt_resp.h"

#define MY_CXT_KEY "Mojo::MT::KeyDB::XS::_guts" XS_VERSION

typedef struct {
  mt_keydb_pool_t pool;
} my_cxt_t;

START_MY_CXT

static inline void mt_extract_endpoint(pTHX_ SV *host_sv, SV *port_sv, SV *path_sv,
                                       SV *db_sv, SV *pass_sv,
                                       const char **host, int *port,
                                       const char **path, int *db,
                                       const char **pass, int *has_pass) {
  *host = (host_sv && SvOK(host_sv)) ? SvPV_nolen(host_sv) : "127.0.0.1";
  *port = (port_sv && SvOK(port_sv)) ? (int)SvIV(port_sv)  : 6379;
  *path = (path_sv && SvOK(path_sv)) ? SvPV_nolen(path_sv) : "";
  *db   = (db_sv   && SvOK(db_sv))   ? (int)SvIV(db_sv)    : 0;
  if (pass_sv && SvOK(pass_sv)) {
    STRLEN plen = 0;
    *pass     = SvPV_const(pass_sv, plen);
    *has_pass = plen > 0 ? 1 : 0;
  } else {
    *pass     = "";
    *has_pass = 0;
  }
}

MODULE = Mojo::MT::KeyDB::XS    PACKAGE = Mojo::MT::KeyDB::XS

PROTOTYPES: DISABLE

BOOT:
{
  MY_CXT_INIT;
  mt_keydb_pool_init(&MY_CXT.pool);
}

#ifdef USE_ITHREADS

void
CLONE(...)
  CODE:
    MY_CXT_CLONE;
    /* Do NOT close parent fds: child worker starts with a clean empty pool */
    mt_keydb_pool_init(&MY_CXT.pool);
    PERL_UNUSED_VAR(items);

#endif

SV *
encode_cmd(...)
  CODE:
    if (items < 1) croak("encode_cmd requires at least 1 argument");
    mt_resp_buf_t buf;
    mt_resp_buf_init(&buf);
    SV *stack_svs[32];
    SV **svs = stack_svs;
    if (items > 32) {
      svs = (SV **)malloc((size_t)items * sizeof(SV *));
      if (!svs) croak("out of memory in encode_cmd");
    }
    for (int i = 0; i < items; i++) svs[i] = ST(i);
    int ok = resp_encode_sv_cmd(aTHX_ &buf, svs, items);
    if (items > 32) free(svs);
    if (!ok) {
      mt_resp_buf_free(&buf);
      croak("out of memory in encode_cmd");
    }
    RETVAL = newSVpvn(buf.data, (STRLEN)buf.len);
    mt_resp_buf_free(&buf);
  OUTPUT:
    RETVAL

void
decode_reply(buf_sv)
    SV *buf_sv
  PPCODE:
    STRLEN blen = 0;
    const char *bptr = SvOK(buf_sv) ? SvPV_const(buf_sv, blen) : "";
    size_t pos = 0;
    SV *val = NULL;
    int is_err = 0;
    char errbuf[256];
    errbuf[0] = '\0';
    int rc = resp_parse_reply(aTHX_ bptr, (size_t)blen, &pos, &val, &is_err, 0,
                              errbuf, sizeof(errbuf), 0);
    if (rc < 0) {
      croak("RESP protocol error: %s", errbuf);
    }
    if (GIMME_V == G_ARRAY) {
      if (rc == 0) {
        XPUSHs(&PL_sv_undef);
        XPUSHs(sv_2mortal(newSViv(0)));
        XPUSHs(sv_2mortal(newSViv(0)));
      } else {
        XPUSHs(sv_2mortal(val));
        XPUSHs(sv_2mortal(newSVuv((UV)pos)));
        XPUSHs(sv_2mortal(newSViv(is_err)));
      }
    } else {
      if (rc == 0) {
        XPUSHs(&PL_sv_undef);
      } else {
        XPUSHs(sv_2mortal(val));
      }
    }

IV
connect(host_sv, port_sv, path_sv, timeout_sv, db_sv, pass_sv)
    SV *host_sv
    SV *port_sv
    SV *path_sv
    SV *timeout_sv
    SV *db_sv
    SV *pass_sv
  CODE:
    dMY_CXT;
    const char *host, *path, *pass;
    int port, db, has_pass;
    double timeout = (timeout_sv && SvOK(timeout_sv)) ? (double)SvNV(timeout_sv) : 5.0;
    mt_extract_endpoint(aTHX_ host_sv, port_sv, path_sv, db_sv, pass_sv,
                        &host, &port, &path, &db, &pass, &has_pass);
    mt_keydb_conn_t *c = mt_keydb_pool_acquire(&MY_CXT.pool, host, port, path,
                                               timeout, db, pass, has_pass);
    char errbuf[256];
    errbuf[0] = '\0';
    if (!mt_keydb_ensure_connected(aTHX_ c, errbuf, sizeof(errbuf))) {
      croak("Mojo::MT::KeyDB connect failed: %s", errbuf);
    }
    RETVAL = 1;
  OUTPUT:
    RETVAL

void
disconnect(host_sv, port_sv, path_sv, db_sv, pass_sv)
    SV *host_sv
    SV *port_sv
    SV *path_sv
    SV *db_sv
    SV *pass_sv
  CODE:
    dMY_CXT;
    const char *host, *path, *pass;
    int port, db, has_pass;
    mt_extract_endpoint(aTHX_ host_sv, port_sv, path_sv, db_sv, pass_sv,
                        &host, &port, &path, &db, &pass, &has_pass);
    mt_keydb_conn_t *c = mt_keydb_pool_find(&MY_CXT.pool, host, port, path,
                                            db, pass, has_pass);
    if (c) mt_keydb_conn_reset(c);

void
disconnect_all()
  CODE:
    dMY_CXT;
    mt_keydb_pool_destroy(&MY_CXT.pool);

IV
is_connected(host_sv, port_sv, path_sv, db_sv, pass_sv)
    SV *host_sv
    SV *port_sv
    SV *path_sv
    SV *db_sv
    SV *pass_sv
  CODE:
    dMY_CXT;
    const char *host, *path, *pass;
    int port, db, has_pass;
    mt_extract_endpoint(aTHX_ host_sv, port_sv, path_sv, db_sv, pass_sv,
                        &host, &port, &path, &db, &pass, &has_pass);
    mt_keydb_conn_t *c = mt_keydb_pool_find(&MY_CXT.pool, host, port, path,
                                            db, pass, has_pass);
    RETVAL = (c && c->fd >= 0) ? 1 : 0;
  OUTPUT:
    RETVAL

SV *
keydb_cmd(host_sv, port_sv, path_sv, timeout_sv, db_sv, pass_sv, ...)
    SV *host_sv
    SV *port_sv
    SV *path_sv
    SV *timeout_sv
    SV *db_sv
    SV *pass_sv
  CODE:
    dMY_CXT;
    int argc = items - 6;
    if (argc <= 0) croak("keydb_cmd requires at least one command argument");
    const char *host, *path, *pass;
    int port, db, has_pass;
    double timeout = (timeout_sv && SvOK(timeout_sv)) ? (double)SvNV(timeout_sv) : 5.0;
    mt_extract_endpoint(aTHX_ host_sv, port_sv, path_sv, db_sv, pass_sv,
                        &host, &port, &path, &db, &pass, &has_pass);
    mt_keydb_conn_t *c = mt_keydb_pool_acquire(&MY_CXT.pool, host, port, path,
                                               timeout, db, pass, has_pass);

    SV *stack_svs[32];
    SV **svs = stack_svs;
    if (argc > 32) {
      svs = (SV **)malloc((size_t)argc * sizeof(SV *));
      if (!svs) croak("out of memory in keydb_cmd");
    }
    for (int i = 0; i < argc; i++) svs[i] = ST(6 + i);

    SV *reply = NULL;
    int is_err = 0;
    char errbuf[256];
    errbuf[0] = '\0';
    int ok = mt_keydb_cmd(aTHX_ c, svs, argc, &reply, &is_err, errbuf, sizeof(errbuf));
    if (argc > 32) free(svs);

    if (!ok) {
      croak("Mojo::MT::KeyDB command failed: %s", errbuf);
    }
    if (is_err) {
      STRLEN elen = 0;
      const char *eptr = SvPV_const(reply, elen);
      SV *err_copy = sv_2mortal(newSVpvn(eptr, elen));
      SvREFCNT_dec(reply);
      croak("KeyDB error: %s", SvPV_nolen(err_copy));
    }
    RETVAL = reply;
  OUTPUT:
    RETVAL

SV *
keydb_pipeline(host_sv, port_sv, path_sv, timeout_sv, db_sv, pass_sv, cmds_sv)
    SV *host_sv
    SV *port_sv
    SV *path_sv
    SV *timeout_sv
    SV *db_sv
    SV *pass_sv
    SV *cmds_sv
  CODE:
    dMY_CXT;
    if (!SvROK(cmds_sv) || SvTYPE(SvRV(cmds_sv)) != SVt_PVAV) {
      croak("keydb_pipeline expects an arrayref of commands");
    }
    AV *cmds = (AV *)SvRV(cmds_sv);
    const char *host, *path, *pass;
    int port, db, has_pass;
    double timeout = (timeout_sv && SvOK(timeout_sv)) ? (double)SvNV(timeout_sv) : 5.0;
    mt_extract_endpoint(aTHX_ host_sv, port_sv, path_sv, db_sv, pass_sv,
                        &host, &port, &path, &db, &pass, &has_pass);
    mt_keydb_conn_t *c = mt_keydb_pool_acquire(&MY_CXT.pool, host, port, path,
                                               timeout, db, pass, has_pass);

    AV *res = NULL;
    char errbuf[256];
    errbuf[0] = '\0';
    if (!mt_keydb_pipeline(aTHX_ c, cmds, &res, errbuf, sizeof(errbuf))) {
      croak("Mojo::MT::KeyDB pipeline failed: %s", errbuf);
    }
    RETVAL = newRV_noinc((SV *)res);
  OUTPUT:
    RETVAL
