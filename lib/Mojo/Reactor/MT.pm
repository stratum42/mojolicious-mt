package Mojo::Reactor::MT;
use Mojo::Base 'Mojo::Reactor';

use Carp qw(croak);
use Mojo::MT qw(HAS_MT_ACCEL);

BEGIN {
  croak 'Mojo::MT::Accel is required for Mojo::Reactor::MT' unless HAS_MT_ACCEL;
}

sub new {
  my $self = shift->SUPER::new(@_);
  $self->{_r} = Mojo::MT::Accel::reactor_new();
  return $self;
}

sub DESTROY {
  my $self = shift;
  Mojo::MT::Accel::reactor_destroy(delete $self->{_r}) if $self->{_r};
}

sub again {
  my ($self, $id, $after) = @_;
  croak 'Timer not active' unless Mojo::MT::Accel::reactor_again($self->{_r}, $id, $after);
}

sub io {
  my ($self, $handle, $cb) = @_;
  my $fd = fileno($handle) // croak 'Handle is closed';
  Mojo::MT::Accel::reactor_io($self->{_r}, $fd, $cb);
  return $self;
}

sub is_running { !!shift->{running} }

sub next_tick {
  my ($self, $cb) = @_;
  push @{$self->{next_tick}}, $cb;
  $self->{next_timer} //= $self->timer(0 => \&_next);
  return undef;
}

sub one_tick {
  my $self = shift;
  my $r    = $self->{_r};

  # Just one tick
  local $self->{running} = 1 unless $self->{running};

  # Wait for one event
  my $i;
  my @ready;
  until ($i || !$self->{running}) {

    my ($gen, $woke, $due) = Mojo::MT::Accel::reactor_wait($r, \@ready);

    # Stop automatically if there is nothing to watch
    return $self->stop unless $gen;

    ++$i if $woke;

    # Dispatch ready I/O events
    for (my $j = 0; $j < @ready; $j += 2) {
      my $cb = Mojo::MT::Accel::reactor_io_cb($r, $gen, $ready[$j], $ready[$j + 1]) or next;
      ++$i and $self->_try('I/O watcher', $cb, $ready[$j + 1]);
    }

    # Timers (skip entirely when no timers are due)
    if ($due) {
      my $now = Mojo::MT::Accel::monotonic_time();
      my @reinsert;
      while (my $cb = Mojo::MT::Accel::reactor_pop_timer($r, $gen, $now, \@reinsert)) {
        ++$i and $self->_try('Timer', $cb);
      }
      Mojo::MT::Accel::reactor_reinsert_timers($r, $gen, \@reinsert) if @reinsert;
    }
  }
}

sub recurring {
  my ($self, $after, $cb) = @_;
  return Mojo::MT::Accel::reactor_timer($self->{_r}, 1, $after, $cb);
}

sub remove {
  my ($self, $remove) = @_;
  return !!Mojo::MT::Accel::reactor_remove_timer($self->{_r}, $remove) unless ref $remove;
  my $fd = fileno($remove) // croak 'Handle is closed';
  return !!Mojo::MT::Accel::reactor_remove_io($self->{_r}, $fd);
}

sub reset {
  my $self = shift;
  Mojo::MT::Accel::reactor_reset($self->{_r}) if $self->{_r};
  delete @{$self}{qw(events next_tick next_timer running)};
}

sub start {
  my $self = shift;
  local $self->{running} = ($self->{running} || 0) + 1;
  $self->one_tick while $self->{running};
}

sub stop { delete shift->{running} }

sub timer {
  my ($self, $after, $cb) = @_;
  return Mojo::MT::Accel::reactor_timer($self->{_r}, 0, $after, $cb);
}

sub wake {
  my $self = shift;
  Mojo::MT::Accel::reactor_wake($self->{_r}) if $self->{_r};
  return $self;
}

sub watch {
  my ($self, $handle, $read, $write) = @_;
  my $fd = fileno $handle;
  croak 'I/O watcher not active'
    unless defined $fd && Mojo::MT::Accel::reactor_watch($self->{_r}, $fd, $read ? 1 : 0, $write ? 1 : 0);
  return $self;
}

sub _next {
  my $self = shift;
  delete $self->{next_timer};
  while (my $cb = shift @{$self->{next_tick}}) { $self->$cb }
}

sub _try {
  my ($self, $what, $cb) = (shift, shift, shift);
  eval { $self->$cb(@_); 1 } or $self->emit(error => "$what failed: $@");
}

1;

=encoding utf8

=head1 NAME

Mojo::Reactor::MT - Native kqueue/epoll event reactor for perl5-mt

=head1 SYNOPSIS

  use Mojo::Reactor::MT;

  my $reactor = Mojo::Reactor::MT->new;
  $reactor->io($fh => sub ($reactor, $writable) { ... });
  $reactor->timer(0.5 => sub ($reactor) { ... });
  $reactor->wake;
  $reactor->start;

=head1 DESCRIPTION

L<Mojo::Reactor::MT> is a thread-safe C event reactor built on
L<Mojo::MT::Accel>, using C<kqueue> on macOS/BSD and C<epoll> on Linux, an
O(1) file-descriptor watcher array, a C binary min-heap timer queue driven by
C<CLOCK_MONOTONIC>, and C<EVFILT_USER> / C<eventfd> for cross-thread wakeups.
It implements the L<Mojo::Reactor> interface so L<Mojo::IOLoop> can select it
automatically when L<Mojo::MT::Accel> is built, and falls back to the standard
Mojolicious reactors otherwise.

=head1 SEE ALSO

L<Mojo::Reactor>, L<Mojo::Reactor::Poll>, L<Mojo::MT::Accel>, L<Mojo::IOLoop>.

=cut
