# mojolicious-mt

`mojolicious-mt` is an experimental community fork of
[Mojolicious](https://mojolicious.org) built to explore what happens when a
Mojolicious application runs on
[`perl5-mt`](https://github.com/stratum42/perl5-mt)'s shared-memory worker
threads and optional thread-safe XS extensions, while keeping existing
Mojolicious code and stock Perl compatibility intact.

First and foremost, this work rests on the shoulders of upstream
[Mojolicious](https://mojolicious.org) (Sebastian Riedel and the Mojolicious
contributors) and [Perl 5](https://www.perl.org) (Larry Wall and the Perl
community).  Mojolicious's clean separation between `Mojo::Reactor`,
`Mojo::IOLoop`, `Mojo::Server`, and HTTP parsing made it possible to experiment
with a single-process multi-worker runtime without distorting the framework's
public API.  The original upstream Mojolicious README is preserved in
[UPSTREAM_README.md](UPSTREAM_README.md).

## Compatibility With Stock Perl

Backward compatibility with stock Perl is a hard requirement of this fork:

- On stock Perl, the compile-time constants in `Mojo::MT` (`HAS_MT`,
  `HAS_MT_ACCEL`, `HAS_MT_FIBER`, `HAS_MT_ASYNC_IO`, `HAS_MT_SHARED_HASH`) all
  fold to `0`, and every module falls back to Mojolicious's pure-Perl
  implementations (and `Mojo::Server::MT` falls back to `Mojo::Server::Daemon`).
- Under `perl5-mt`, you can disable the native paths at runtime for comparison
  or debugging with `MOJO_NO_MT_ACCEL=1` (disables `Mojo::MT::Accel` while
  keeping worker threads) or `MOJO_NO_MT=1` (disables all `perl5-mt` paths).

## What This Fork Adds

- `Mojo::MT` (`lib/Mojo/MT.pm`): Compile-time feature detection for `perl5-mt`
  and `Mojo::MT::Accel`, plus `prepare_for_freeze()` to clear boot-time
  singletons and weakref registries before `MT::Image::freeze()`.
- `Mojo::Server::MT` (`lib/Mojo/Server/MT.pm`): A single-process multi-worker
  HTTP/1.1 and WebSocket server that preloads modules, seals the compiled
  interpreter with `MT::Image::freeze()`, and runs one `Mojo::IOLoop` per
  `MT::Worker` thread.
- `Mojolicious::Command::mt` (`lib/Mojolicious/Command/mt.pm`): CLI entry point
  (`./myapp.pl mt -w 8 -l http://127.0.0.1:8080`) mirroring the familiar
  `daemon` and `prefork` flags.
- `Mojo::Reactor::MT` (`lib/Mojo/Reactor/MT.pm`): A thread-safe C event reactor
  backed by `kqueue` (macOS/BSD), `epoll` (Linux), `pollset` (AIX), or POSIX
  `poll` (fallback for IBM i PASE, z/OS USS, Solaris, and other POSIX systems),
  with a C binary min-heap timer queue and cross-thread wakeups.
- `Mojo::MT::Accel` (`xs/`): An optional thread-safe (`PERL_XS_THREADSAFE=1`)
  XS extension with multi-architecture SIMD fast paths (`amd64` AVX2/SSE2,
  `arm64` NEON, POWER VSX/AltiVec LE/BE, `s390x` VX) and an exact scalar twin
  (also selected under `PERL_FORCE_SCALAR` or EBCDIC) for HTTP/1.1
  request/header parsing and response serialization, `Mojo::JSON`
  encode/decode, `Mojo::Util` string helpers (`xml_escape`, `url_escape`,
  `url_unescape`, `b64_encode`, `b64_decode`), and `Mojo::Reactor::MT`.  Any
  edge case (such as obs-fold headers, UTF-8 header keys, or invalid JSON
  diagnostics) delegates back to pure Perl so behavior matches upstream.
- `Mojo::MT::KeyDB` (`lib/Mojo/MT/KeyDB.pm`, `xs-keydb/`): A thread-safe
  RESP2/RESP3 client for KeyDB and Redis with per-worker `MY_CXT` persistent
  TCP/Unix sockets, an optional XS codec (`Mojo::MT::KeyDB::XS`), and a
  pure-Perl fallback.
- `Mojo::MT::SharedCache` (`lib/Mojo/MT/SharedCache.pm`): A small key-value
  wrapper over `MT::SharedHash` for sharing scalar values across worker threads
  without serialization (falls back to a process-local hash on stock Perl).
- `Mojo::MT::Fiber` (`lib/Mojo/MT/Fiber.pm`): Convenience helpers
  (`run_fibers`, `async_slurp`, `async_slurp_many`, `yield`, `pass`) over
  `MT::Fiber` and the `:async` PerlIO layer.

## Trade-offs and Platform Notes

This is an experimental runtime model, and a few practical trade-offs are worth
knowing upfront:

- **Sealed boot image constraints**: `MT::Image::freeze()` marks the boot-time
  interpreter read-only.  Modules that mutate package-level lexicals or
  `Hash::Util::FieldHash` registries after freeze will trap in worker threads.
  `Mojo::Server::MT` mitigates this by preloading core modules, running
  `Mojo::MT::prepare_for_freeze()`, instantiating the application per worker,
  and storing per-worker caches in `our` globals (which `perl5-mt` isolates per
  worker via GP indirection).  Third-party CPAN modules that keep mutable C
  globals without `PERL_XS_THREADSAFE=1` are not safe to call directly inside
  worker threads unless isolated by `perl5-mt`'s XS compartment layer.
- **Linux vs macOS/BSD socket accepts**: On Linux, each worker binds its own
  `SO_REUSEPORT` listen socket so the kernel distributes incoming connections
  across per-worker accept queues.  On macOS and other non-Linux systems,
  `SO_REUSEPORT` does not load-balance TCP accepts across distinct sockets, so
  `Mojo::Server::MT` binds one shared listen socket per URL and shares the file
  descriptor across workers.
- **`:async` file I/O**: `Mojo::Asset::File` uses `MT::IO` (`:async` and
  `MT::IO::stat`) for all file reads on Linux (`io_uring`).  On macOS and other
  non-Linux platforms, `:async` is only used for files of at least 256 KiB,
  since plain `sysread` and `stat` are faster for small static assets there.
- **Portability of XS extensions**: Both `xs/` and `xs-keydb/` are optional.
  `xs/mt_simd.h` selects `amd64` AVX2/SSE2, `arm64` NEON, POWER VSX/AltiVec
  (LE and BE), `s390x` VX, or the scalar twin at compile time (and always uses
  the scalar twin under `PERL_FORCE_SCALAR` or EBCDIC). `Mojo::Reactor::MT`
  selects `kqueue`, `epoll`, AIX `pollset`, or POSIX `poll`. Stock Perl skips
  the XS extensions altogether.

## Building the Optional XS Extensions

Build `xs/` (`Mojo::MT::Accel`) and `xs-keydb/` (`Mojo::MT::KeyDB::XS`) against
your `perl5-mt` binary (installed or in an uninstalled source tree):

```sh
PERL5_MT=/path/to/perl5-mt/perl
PERL5_MT_LIB=/path/to/perl5-mt/lib

(cd xs && "$PERL5_MT" "-I$PERL5_MT_LIB" Makefile.PL && make)
(cd xs-keydb && "$PERL5_MT" "-I$PERL5_MT_LIB" Makefile.PL && make)
```

If building against an uninstalled `perl5-mt` tree located elsewhere, you can
also point `PERL5_MT_SRC=/path/to/perl5-mt` before running `Makefile.PL`.

## Running Tests

Run the `t/mt/*.t` suite against `perl5-mt` with the built XS extensions:

```sh
/path/to/perl5-mt/perl \
  -I/path/to/perl5-mt/lib \
  -Ixs/blib/lib -Ixs/blib/arch \
  -Ixs-keydb/blib/lib -Ixs-keydb/blib/arch \
  -Ilib \
  -MTest::Harness -e 'runtests(@ARGV)' t/mt/*.t
```

To verify stock Perl compatibility, run the same tests (plus the upstream
`t/mojo/*.t` and `t/mojolicious/*.t` suites) with your system `perl -Ilib`.

## Benchmarks

Reproducible benchmark harnesses and measurement scripts are kept in the
separate
[`mojolicious-mt-bench`](https://github.com/stratum42/mojolicious-mt-bench)
repository so the framework tree stays focused on library code and tests.
