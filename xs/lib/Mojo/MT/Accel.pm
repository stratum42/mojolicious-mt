package Mojo::MT::Accel;
use strict;
use warnings;

our $VERSION = '0.01';

require XSLoader;
XSLoader::load('Mojo::MT::Accel', $VERSION);

1;

=encoding utf8

=head1 NAME

Mojo::MT::Accel - Optional thread-safe C/SIMD accelerator for mojolicious-mt

=head1 DESCRIPTION

L<Mojo::MT::Accel> provides optional thread-safe (C<PERL_XS_THREADSAFE=1>) C
and SIMD (amd64 AVX2/SSE2, arm64 NEON, POWER VSX/AltiVec, IBM Z VX, and a
portable scalar twin) implementations for the L<Mojo::Reactor::MT> event loop
(C<kqueue>, C<epoll>, AIX C<pollset>, and POSIX C<poll>), HTTP/1.1
request/header parsing and response serialization, L<Mojo::JSON> encoding and
decoding, and L<Mojo::Util> string helpers. Edge cases delegate back to
Mojolicious's pure-Perl implementations.

=cut
