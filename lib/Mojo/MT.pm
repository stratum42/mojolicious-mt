package Mojo::MT;
use Mojo::Base -strict;

use Exporter qw(import);

# Detect perl5-mt runtime (MT::Image::freeze is a core builtin in perl5-mt)
use constant HAS_MT => $ENV{MOJO_NO_MT}
  ? 0
  : ((defined &MT::Image::freeze || eval { require MT::Image; 1 }) ? 1 : 0);

# Detect Tier-1 thread-safe XS accelerator (Mojo::MT::Accel)
use constant HAS_MT_ACCEL => ($ENV{MOJO_NO_MT} || $ENV{MOJO_NO_MT_ACCEL})
  ? 0
  : (eval { require Mojo::MT::Accel; 1 } ? 1 : 0);

# Detect perl5-mt native concurrency & I/O primitives
use constant HAS_MT_FIBER       => HAS_MT && eval { require MT::Fiber;      1 } ? 1 : 0;
use constant HAS_MT_ASYNC_IO    => HAS_MT && eval { require MT::IO;         1 } ? 1 : 0;
use constant HAS_MT_SHARED_HASH => HAS_MT && eval { require MT::SharedHash; 1 } ? 1 : 0;

our @EXPORT_OK = qw(HAS_MT HAS_MT_ACCEL HAS_MT_FIBER HAS_MT_ASYNC_IO HAS_MT_SHARED_HASH prepare_for_freeze);
our %EXPORT_TAGS = (all => \@EXPORT_OK);

# When Mojo::MT is loaded under perl5-mt, avoid Hash::Util::FieldHash in
# Mojo::DynamicMethods so Mojolicious apps never attach FieldHash weakref
# backrefs that conflict with MT::Image::freeze() / worker teardown.
if (HAS_MT) {
  no warnings 'once';
  $Mojo::DynamicMethods::FROZEN_MODE = 1;
}

# Prepare Mojolicious global state for MT::Image::freeze()
sub prepare_for_freeze {
  return unless HAS_MT;

  # Clear boot-time Mojo::IOLoop singleton so each worker creates its own loop/reactor
  Mojo::IOLoop->reset_singleton if defined &Mojo::IOLoop::reset_singleton;

  # Pre-warm UTF-8 encoder before freeze so workers avoid mutating Encode::Alias lexicals
  Mojo::Util::_encoding('UTF-8') if defined &Mojo::Util::_encoding;

  # Migrate Mojo::DynamicMethods off Hash::Util::FieldHash before freeze so
  # Hash::Util::FieldHash::CLONE does not mutate a frozen SVf_IMMORTAL registry
  # and worker teardown has no dangling fieldhash weakref backrefs.
  Mojo::DynamicMethods::prepare_for_freeze()
    if defined &Mojo::DynamicMethods::prepare_for_freeze;

  return 1;
}

1;

=encoding utf8

=head1 NAME

Mojo::MT - Feature detection for perl5-mt native acceleration

=head1 SYNOPSIS

  use Mojo::MT qw(HAS_MT HAS_MT_ACCEL HAS_MT_FIBER HAS_MT_ASYNC_IO HAS_MT_SHARED_HASH);

  if (HAS_MT) {
    ...
  }

=head1 DESCRIPTION

L<Mojo::MT> provides compile-time feature detection constants for C<perl5-mt>
and the optional L<Mojo::MT::Accel> thread-safe XS extension, along with
L</"prepare_for_freeze"> to ready Mojolicious globals before sealing the boot
image. On stock Perl, all constants fold to C<0> at compile time so existing
applications continue to run on pure-Perl paths unchanged.

=head1 CONSTANTS

=head2 HAS_MT

True when running under C<perl5-mt> (C<MT::Image::freeze> is available) and
C<$ENV{MOJO_NO_MT}> is not set.

=head2 HAS_MT_ACCEL

True when L<Mojo::MT::Accel> is loadable and neither C<$ENV{MOJO_NO_MT}> nor
C<$ENV{MOJO_NO_MT_ACCEL}> is set.

=head2 HAS_MT_FIBER

True when L</"HAS_MT"> is true and L<MT::Fiber> loads.

=head2 HAS_MT_ASYNC_IO

True when L</"HAS_MT"> is true and L<MT::IO> loads.

=head2 HAS_MT_SHARED_HASH

True when L</"HAS_MT"> is true and L<MT::SharedHash> loads.

=head1 FUNCTIONS

=head2 prepare_for_freeze

  Mojo::MT::prepare_for_freeze();

Clears the boot-time L<Mojo::IOLoop> singleton, pre-warms the UTF-8 encoder,
and migrates L<Mojo::DynamicMethods> off L<Hash::Util::FieldHash> so worker
threads cloned from a sealed C<MT::Image::freeze()> image do not mutate frozen
structures. No-op on stock Perl.

=head1 SEE ALSO

L<Mojolicious>, L<Mojo::Server::MT>, L<Mojo::IOLoop>.

=cut
