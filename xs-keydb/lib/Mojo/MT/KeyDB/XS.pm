package Mojo::MT::KeyDB::XS;
use strict;
use warnings;

our $VERSION = '0.01';

require XSLoader;
XSLoader::load('Mojo::MT::KeyDB::XS', $VERSION);

END {
  disconnect_all() if defined &disconnect_all;
}

1;

=encoding utf8

=head1 NAME

Mojo::MT::KeyDB::XS - Optional thread-safe C/SIMD KeyDB RESP2/RESP3 client

=head1 DESCRIPTION

L<Mojo::MT::KeyDB::XS> provides an optional thread-safe
(C<PERL_XS_THREADSAFE=1>) C/SIMD RESP2/RESP3 encoder, streaming reply parser,
and per-worker C<MY_CXT> TCP/Unix socket pool for L<Mojo::MT::KeyDB>.

=cut
