/* Mojo::MT::Accel - Tier-1 thread-safe C/NEON accelerator for mojolicious-mt.
 * Compiled with -DPERL_XS_THREADSAFE=1 and PERL_NO_GET_CONTEXT.
 * Zero mutable global state: all per-interpreter state lives in MY_CXT.
 */

#define PERL_NO_GET_CONTEXT
#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "mt_util.h"
#include "mt_http.h"
#include "mt_json.h"
#include "mt_reactor.h"

#define MY_CXT_KEY "Mojo::MT::Accel::_guts" XS_VERSION

typedef struct {
  SV *json_true;
  SV *json_false;
  HV *json_bool_stash;
} my_cxt_t;

START_MY_CXT

static void mt_init_json_cxt(pTHX_ my_cxt_t *cxt) {
  if (!cxt->json_true) {
    cxt->json_true  = SvREFCNT_inc(eval_pv("require JSON::PP; JSON::PP::true()", TRUE));
    cxt->json_false = SvREFCNT_inc(eval_pv("JSON::PP::false()", TRUE));
    cxt->json_bool_stash = gv_stashpvs("JSON::PP::Boolean", 0);
  }
}

MODULE = Mojo::MT::Accel    PACKAGE = Mojo::MT::Accel

PROTOTYPES: DISABLE

BOOT:
{
  MY_CXT_INIT;
  MY_CXT.json_true       = NULL;
  MY_CXT.json_false      = NULL;
  MY_CXT.json_bool_stash = NULL;
}

#ifdef USE_ITHREADS

void
CLONE(...)
  CODE:
    MY_CXT_CLONE;
    MY_CXT.json_true       = NULL;
    MY_CXT.json_false      = NULL;
    MY_CXT.json_bool_stash = NULL;
    PERL_UNUSED_VAR(items);

#endif

# ============================================================================
# Monotonic clock
# ============================================================================

NV
monotonic_time()
  CODE:
    RETVAL = (NV)mt_monotonic_time();
  OUTPUT:
    RETVAL

# ============================================================================
# Mojo::Reactor::MT native kqueue/epoll reactor bindings
# ============================================================================

IV
reactor_new()
  CODE:
    mt_reactor_t *r = mt_reactor_new();
    if (!r) croak("Mojo::Reactor::MT: failed to create native reactor");
    RETVAL = PTR2IV(r);
  OUTPUT:
    RETVAL

void
reactor_destroy(handle)
    IV handle
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (r) mt_reactor_destroy(aTHX_ r);

void
reactor_reset(handle)
    IV handle
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (r) mt_reactor_reset(aTHX_ r);

void
reactor_wake(handle)
    IV handle
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (r) mt_reactor_wake(r);

IV
reactor_has_watchers(handle)
    IV handle
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    RETVAL = (r && (r->io_count > 0 || r->timer_count > 0)) ? 1 : 0;
  OUTPUT:
    RETVAL

IV
reactor_io(handle, fd, cb)
    IV handle
    int fd
    SV *cb
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    RETVAL = r ? mt_reactor_io(aTHX_ r, fd, cb) : 0;
  OUTPUT:
    RETVAL

IV
reactor_watch(handle, fd, read_flag, write_flag)
    IV handle
    int fd
    int read_flag
    int write_flag
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    RETVAL = r ? mt_reactor_watch(r, fd, read_flag, write_flag) : 0;
  OUTPUT:
    RETVAL

IV
reactor_remove_io(handle, fd)
    IV handle
    int fd
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    RETVAL = r ? mt_reactor_remove_io(aTHX_ r, fd) : 0;
  OUTPUT:
    RETVAL

SV *
reactor_io_cb(handle, gen, fd, is_write)
    IV handle
    UV gen
    int fd
    int is_write
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    SV *cb = r ? mt_reactor_get_io_cb(aTHX_ r, (uint64_t)gen, fd, is_write) : NULL;
    RETVAL = cb ? cb : &PL_sv_undef;
  OUTPUT:
    RETVAL

UV
reactor_timer(handle, recurring, after, cb)
    IV handle
    int recurring
    NV after
    SV *cb
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    RETVAL = r ? (UV)mt_reactor_add_timer(aTHX_ r, recurring, (double)after, cb) : 0;
  OUTPUT:
    RETVAL

IV
reactor_again(handle, id_sv, ...)
    IV handle
    SV *id_sv
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (!r || !id_sv || !SvOK(id_sv)) {
      RETVAL = 0;
    } else {
      UV id = 0;
      int valid = 0;
      if (SvIOK(id_sv)) {
        id = SvUVX(id_sv);
        valid = 1;
      } else if (looks_like_number(id_sv)) {
        id = SvUV(id_sv);
        valid = 1;
      }
      if (!valid || id == 0) {
        RETVAL = 0;
      } else {
        int has_after = (items >= 3 && SvOK(ST(2)));
        double after  = has_after ? (double)SvNV(ST(2)) : 0.0;
        RETVAL = mt_reactor_again(r, (uint64_t)id, has_after, after);
      }
    }
  OUTPUT:
    RETVAL

IV
reactor_remove_timer(handle, id_sv)
    IV handle
    SV *id_sv
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (!r || !id_sv || !SvOK(id_sv)) {
      RETVAL = 0;
    } else if (SvIOK(id_sv)) {
      UV id = SvUVX(id_sv);
      RETVAL = id ? mt_reactor_remove_timer(aTHX_ r, (uint64_t)id) : 0;
    } else if (looks_like_number(id_sv)) {
      UV id = SvUV(id_sv);
      RETVAL = id ? mt_reactor_remove_timer(aTHX_ r, (uint64_t)id) : 0;
    } else {
      RETVAL = 0;
    }
  OUTPUT:
    RETVAL

void
reactor_wait(handle, ready_av_ref)
    IV handle
    SV *ready_av_ref
  PPCODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (!r || (r->io_count == 0 && r->timer_count == 0)
     || !SvROK(ready_av_ref) || SvTYPE(SvRV(ready_av_ref)) != SVt_PVAV) {
      XPUSHs(sv_2mortal(newSViv(0)));
      XPUSHs(sv_2mortal(newSViv(0)));
      XPUSHs(sv_2mortal(newSViv(0)));
      XSRETURN(3);
    }
    uint64_t gen = r->generation;
    int woke = 0, due = 0;
    mt_reactor_wait(aTHX_ r, (AV *)SvRV(ready_av_ref), &woke, &due);
    XPUSHs(sv_2mortal(newSVuv((UV)gen)));
    XPUSHs(sv_2mortal(newSViv(woke)));
    XPUSHs(sv_2mortal(newSViv(due)));
    XSRETURN(3);

SV *
reactor_pop_timer(handle, gen, now, reinsert_av_ref)
    IV handle
    UV gen
    NV now
    SV *reinsert_av_ref
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    AV *reinsert_av = (SvROK(reinsert_av_ref) && SvTYPE(SvRV(reinsert_av_ref)) == SVt_PVAV)
      ? (AV *)SvRV(reinsert_av_ref) : NULL;
    SV *cb = (r && reinsert_av)
      ? mt_reactor_pop_timer(aTHX_ r, (uint64_t)gen, (double)now, reinsert_av)
      : NULL;
    RETVAL = cb ? cb : &PL_sv_undef;
  OUTPUT:
    RETVAL

void
reactor_reinsert_timers(handle, gen, reinsert_av_ref)
    IV handle
    UV gen
    SV *reinsert_av_ref
  CODE:
    mt_reactor_t *r = INT2PTR(mt_reactor_t *, handle);
    if (r && SvROK(reinsert_av_ref) && SvTYPE(SvRV(reinsert_av_ref)) == SVt_PVAV) {
      mt_reactor_reinsert_timers(aTHX_ r, (uint64_t)gen, (AV *)SvRV(reinsert_av_ref));
    }

# ============================================================================
# HTTP/1.1 C/NEON Parser
# ============================================================================

IV
http_parse_headers(headers_ref, names_ref, buf, max_line_size, max_lines)
    SV *headers_ref
    SV *names_ref
    SV *buf
    IV max_line_size
    IV max_lines
  CODE:
    if (!SvROK(headers_ref) || SvTYPE(SvRV(headers_ref)) != SVt_PVHV
     || !SvROK(names_ref)   || SvTYPE(SvRV(names_ref))   != SVt_PVHV) {
      RETVAL = -1;
    } else {
      RETVAL = mt_http_parse_headers(
        aTHX_ (HV *)SvRV(headers_ref), (HV *)SvRV(names_ref),
        buf, max_line_size, max_lines
      );
    }
  OUTPUT:
    RETVAL

void
http_parse_start_line(buf)
    SV *buf
  PPCODE:
    SV *method_sv = NULL, *target_sv = NULL, *version_sv = NULL;
    IV consumed = mt_http_parse_start_line(aTHX_ buf, &method_sv, &target_sv, &version_sv);
    if (consumed >= 0) {
      XPUSHs(sv_2mortal(newSViv(consumed)));
      XPUSHs(sv_2mortal(method_sv));
      XPUSHs(sv_2mortal(target_sv));
      XPUSHs(sv_2mortal(version_sv));
      XSRETURN(4);
    } else {
      XPUSHs(sv_2mortal(newSViv(consumed)));
      XSRETURN(1);
    }

SV *
build_http_response(code, msg, headers_ref, body, date_str, keep_alive, ...)
    IV code
    SV *msg
    SV *headers_ref
    SV *body
    SV *date_str
    int keep_alive
  CODE:
    HV *headers_hv = (headers_ref && SvROK(headers_ref) && SvTYPE(SvRV(headers_ref)) == SVt_PVHV)
      ? (HV *)SvRV(headers_ref) : NULL;
    HV *names_hv = (items >= 7 && ST(6) && SvROK(ST(6)) && SvTYPE(SvRV(ST(6))) == SVt_PVHV)
      ? (HV *)SvRV(ST(6)) : NULL;
    SV *version_sv = (items >= 8) ? ST(7) : NULL;
    SV *out = mt_http_build_response(aTHX_ code, msg, headers_hv, body, date_str, keep_alive, names_hv, version_sv);
    RETVAL = out ? out : &PL_sv_undef;
  OUTPUT:
    RETVAL

# ============================================================================
# JSON C/NEON Encoder & Decoder
# ============================================================================

SV *
json_encode(val, as_utf8_bytes)
    SV *val
    int as_utf8_bytes
  PREINIT:
    dMY_CXT;
  CODE:
    mt_init_json_cxt(aTHX_ &MY_CXT);
    SV *out = mt_json_encode(aTHX_ val, as_utf8_bytes, MY_CXT.json_bool_stash);
    RETVAL = out ? out : &PL_sv_undef;
  OUTPUT:
    RETVAL

IV
json_decode(input, is_chars, out_ref)
    SV *input
    int is_chars
    SV *out_ref
  PREINIT:
    dMY_CXT;
  CODE:
    mt_init_json_cxt(aTHX_ &MY_CXT);
    SV *decoded = NULL;
    if (SvROK(out_ref) && mt_json_decode(aTHX_ input, is_chars, MY_CXT.json_true, MY_CXT.json_false, &decoded)) {
      sv_setsv(SvRV(out_ref), decoded);
      SvREFCNT_dec(decoded);
      RETVAL = 1;
    } else {
      RETVAL = 0;
    }
  OUTPUT:
    RETVAL

# ============================================================================
# Mojo::Util C/NEON Accelerators
# ============================================================================

SV *
xml_escape(str)
    SV *str
  CODE:
    RETVAL = mt_xml_escape(aTHX_ str);
  OUTPUT:
    RETVAL

SV *
url_escape(str)
    SV *str
  CODE:
    SV *out = mt_url_escape(aTHX_ str);
    RETVAL = out ? out : &PL_sv_undef;
  OUTPUT:
    RETVAL

SV *
url_unescape(str)
    SV *str
  CODE:
    RETVAL = mt_url_unescape(aTHX_ str);
  OUTPUT:
    RETVAL

SV *
b64_encode(bytes, ...)
    SV *bytes
  CODE:
    SV *eol_sv = (items >= 2) ? ST(1) : NULL;
    SV *out = mt_b64_encode(aTHX_ bytes, eol_sv);
    RETVAL = out ? out : &PL_sv_undef;
  OUTPUT:
    RETVAL

SV *
b64_decode(b64)
    SV *b64
  CODE:
    RETVAL = mt_b64_decode(aTHX_ b64);
  OUTPUT:
    RETVAL
