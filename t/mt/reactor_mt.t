use Mojo::Base -strict;

use Test::More;
use Mojo::MT qw(HAS_MT HAS_MT_ACCEL);

plan skip_all => 'Mojo::MT::Accel not available' unless HAS_MT_ACCEL;

use IO::Socket::INET;
use Mojo::Reactor;
use Mojo::Util qw(steady_time);
require Mojo::Reactor::MT;

# Shared between subtests
my ($reactor, $reactor2, $listen, $port, $client, $server, $readable, $writable, $recurring);

subtest 'Auto-detection and Instantiation' => sub {
  local $ENV{MOJO_REACTOR};
  is(Mojo::Reactor->detect, 'Mojo::Reactor::MT', 'detect prefers Mojo::Reactor::MT when HAS_MT_ACCEL');
  $reactor = Mojo::Reactor::MT->new;
  is ref $reactor,                 'Mojo::Reactor::MT', 'right object';
  is ref Mojo::Reactor::MT->new,   'Mojo::Reactor::MT', 'right object';
  undef $reactor;
  is ref Mojo::Reactor::MT->new,   'Mojo::Reactor::MT', 'right object';
  use_ok 'Mojo::IOLoop';
  $reactor = Mojo::IOLoop->singleton->reactor;
  is ref $reactor, 'Mojo::Reactor::MT', 'IOLoop singleton uses Mojo::Reactor::MT';
};

subtest 'Make sure it stops automatically when not watching for events' => sub {
  my $triggered;
  Mojo::IOLoop->next_tick(sub { $triggered++ });
  Mojo::IOLoop->start;
  ok $triggered, 'reactor waited for one event';
  my $time = time;
  Mojo::IOLoop->start;
  Mojo::IOLoop->one_tick;
  ok time < ($time + 10), 'stopped automatically';
};

subtest 'Listen' => sub {
  $listen = IO::Socket::INET->new(Listen => 5, LocalAddr => '127.0.0.1');
  $port   = $listen->sockport;
  $reactor->io($listen => sub { pop() ? $writable++ : $readable++ })->watch($listen, 0, 0)->watch($listen, 1, 1);
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok !$readable, 'handle is not readable';
  ok !$writable, 'handle is not writable';
};

subtest 'Connect' => sub {
  $client = IO::Socket::INET->new(PeerAddr => '127.0.0.1', PeerPort => $port);
  $reactor->timer(1 => sub { shift->stop });
  $reactor->start;
  ok $readable,  'handle is readable';
  ok !$writable, 'handle is not writable';
};

subtest 'Accept' => sub {
  $server = $listen->accept;
  ok $reactor->remove($listen),  'removed';
  ok !$reactor->remove($listen), 'not removed again';
  ($readable, $writable) = ();
  $reactor->io($client => sub { pop() ? $writable++ : $readable++ });
  $reactor->again($reactor->timer(0.025 => sub { shift->stop }));
  $reactor->start;
  ok !$readable, 'handle is not readable';
  ok $writable,  'handle is writable';
  print $client "hello!\n";
  select(undef, undef, undef, 0.05);
  ok $reactor->remove($client), 'removed';
  ($readable, $writable) = ();
  $reactor->io($server => sub { pop() ? $writable++ : $readable++ });
  $reactor->watch($server, 1, 0);
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,  'handle is readable';
  ok !$writable, 'handle is not writable';
  ($readable, $writable) = ();
  $reactor->watch($server, 1, 1);
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable, 'handle is readable';
  ok $writable, 'handle is writable';
  ($readable, $writable) = ();
  $reactor->watch($server, 0, 0);
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok !$readable, 'handle is not readable';
  ok !$writable, 'handle is not writable';
  ($readable, $writable) = ();
  $reactor->watch($server, 1, 0);
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,  'handle is readable';
  ok !$writable, 'handle is not writable';
  ($readable, $writable) = ();
  $reactor->io($server => sub { pop() ? $writable++ : $readable++ });
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable, 'handle is readable';
  ok $writable, 'handle is writable';
};

subtest 'Timers' => sub {
  my $timer;
  $reactor->timer(0 => sub { $timer++ });
  ok $reactor->remove($reactor->timer(0 => sub { $timer++ })), 'removed';
  my $id = $reactor->recurring(0 => sub { $recurring++ });
  ($readable, $writable) = ();
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,  'handle is readable again';
  ok $writable,  'handle is writable again';
  ok $timer,     'timer was triggered';
  ok $recurring, 'recurring was triggered';
  my $done;
  ($readable, $writable, $timer, $recurring) = ();
  $reactor->timer(0.025 => sub { $done = shift->is_running });
  $reactor->one_tick while !$done;
  ok $readable,  'handle is readable again';
  ok $writable,  'handle is writable again';
  ok !$timer,    'timer was not triggered';
  ok $recurring, 'recurring was triggered again';
  ($readable, $writable, $timer, $recurring) = ();
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,              'handle is readable again';
  ok $writable,              'handle is writable again';
  ok !$timer,                'timer was not triggered';
  ok $recurring,             'recurring was triggered again';
  ok $reactor->remove($id),  'removed';
  ok !$reactor->remove($id), 'not removed again';
  ($readable, $writable, $timer, $recurring) = ();
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,   'handle is readable again';
  ok $writable,   'handle is writable again';
  ok !$timer,     'timer was not triggered';
  ok !$recurring, 'recurring was not triggered again';
  ($readable, $writable, $timer, $recurring) = ();
  my $next_tick;
  is $reactor->next_tick(sub { $next_tick++ }), undef, 'returned undef';
  $id = $reactor->recurring(0 => sub { $recurring++ });
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $readable,  'handle is readable again';
  ok $writable,  'handle is writable again';
  ok !$timer,    'timer was not triggered';
  ok $recurring, 'recurring was triggered again';
  ok $next_tick, 'next tick was triggered';
};

subtest 'Reset' => sub {
  $reactor->next_tick(sub { die 'Reset failed' });
  $reactor->reset;
  ($readable, $writable, $recurring) = ();
  $reactor->next_tick(sub { shift->stop });
  $reactor->start;
  ok !$readable,  'io event was not triggered again';
  ok !$writable,  'io event was not triggered again';
  ok !$recurring, 'recurring was not triggered again';
  $reactor2 = Mojo::Reactor::MT->new;
  is ref $reactor2, 'Mojo::Reactor::MT', 'right object';
};

subtest 'Ordered next_tick' => sub {
  my $result = [];
  for my $i (1 .. 10) {
    $reactor->next_tick(sub { push @$result, $i });
  }
  $reactor->start;
  is_deeply $result, [1 .. 10], 'right result';
};

subtest 'Reset while watchers are active' => sub {
  $writable = undef;
  $reactor->io($_ => sub { ++$writable and shift->reset })->watch($_, 0, 1) for $client, $server;
  $reactor->start;
  is $writable, 1, 'only one handle was writable';
};

subtest 'Concurrent reactors' => sub {
  my $timer = 0;
  $reactor->recurring(0 => sub { $timer++ });
  my $timer2;
  $reactor2->recurring(0 => sub { $timer2++ });
  $reactor->timer(0.025 => sub { shift->stop });
  $reactor->start;
  ok $timer,   'timer was triggered';
  ok !$timer2, 'timer was not triggered';
  $timer = $timer2 = 0;
  $reactor2->timer(0.025 => sub { shift->stop });
  $reactor2->start;
  ok !$timer, 'timer was not triggered';
  ok $timer2, 'timer was triggered';
  $reactor->reset;
  $reactor2->reset;
};

subtest 'Restart timer' => sub {
  my ($single, $pair, $one, $two, $last);
  $reactor->timer(0.025 => sub { $single++ });
  $one = $reactor->timer(
    0.025 => sub {
      my $reactor = shift;
      $last++ if $single && $pair;
      $pair++ ? $reactor->stop : $reactor->again($two);
    }
  );
  $two = $reactor->timer(
    0.025 => sub {
      my $reactor = shift;
      $last++ if $single && $pair;
      $pair++ ? $reactor->stop : $reactor->again($one);
    }
  );
  $reactor->start;
  is $pair, 2, 'timer pair was triggered';
  ok $single, 'single timer was triggered';
  ok $last,   'timers were triggered in the right order';
};

subtest 'Reset timer' => sub {
  my $before = steady_time;
  my ($after, $again, $two);
  my $one = $reactor->timer(300 => sub { $after = steady_time });
  $two = $reactor->recurring(
    300 => sub {
      my $reactor = shift;
      $reactor->remove($two) if ++$again > 3;
    }
  );
  $reactor->timer(
    0.025 => sub {
      my $reactor = shift;
      $reactor->again($one, 0.025);
      $reactor->again($two, 0.025);
    }
  );
  $reactor->start;
  ok $after, 'timer was triggered';
  ok(($after - $before) < 200, 'less than 200 seconds');
  is $again, 4, 'recurring timer triggered four times';
};

subtest 'Restart inactive timer & inactive watcher' => sub {
  my $id = $reactor->timer(0 => sub { });
  ok $reactor->remove($id), 'removed';
  eval { $reactor->again($id) };
  like $@, qr/Timer not active/, 'right error';

  ok !$reactor->remove($listen), 'not removed again';
  eval { $reactor->watch($listen, 1, 1) };
  like $@, qr!I/O watcher not active!, 'right error';
};

subtest 'Error and reset events' => sub {
  my $err;
  $reactor->unsubscribe('error')->on(
    error => sub {
      shift->stop;
      $err = pop;
    }
  );
  $reactor->timer(0 => sub { die "works!\n" });
  $reactor->start;
  like $err, qr/works!/, 'right error';

  $reactor->on(error => sub { });
  ok $reactor->has_subscribers('error'), 'has subscribers';
  $reactor->reset;
  ok !$reactor->has_subscribers('error'), 'no subscribers';
};

subtest 'Recursion' => sub {
  my $timer;
  my $r = Mojo::Reactor::MT->new;
  $r->timer(0 => sub { ++$timer and shift->one_tick });
  $r->one_tick;
  is $timer, 1, 'timer was triggered once';
};

subtest 'Wakeup (EVFILT_USER / eventfd)' => sub {
  my $r = Mojo::Reactor::MT->new;
  my $long_id = $r->timer(10 => sub { });
  my $t0 = steady_time;
  $r->wake;
  $r->one_tick;
  my $elapsed = steady_time - $t0;
  ok $elapsed < 1.0, "wake() returned from one_tick immediately (${elapsed}s)";
  $r->remove($long_id);
};

subtest 'Multi-worker MT::Worker with Mojo::Reactor::MT' => sub {
  plan skip_all => 'MT::Worker not available' unless HAS_MT && eval { require MT::Worker; 1 };
  require IO::Socket;

  # Reset singleton before freeze
  Mojo::MT::prepare_for_freeze();
  MT::Image::freeze();

  my @workers;
  for my $wid (1 .. 4) {
    push @workers, MT::Worker->spawn(
      sub {
        my $r = Mojo::Reactor::MT->new;
        my ($s1, $s2) = IO::Socket->socketpair(IO::Socket::AF_UNIX(), IO::Socket::SOCK_STREAM(), IO::Socket::PF_UNSPEC());
        my $got = '';
        $r->io(
          $s1 => sub {
            my ($reactor, $writable) = @_;
            return if $writable;
            sysread($s1, my $buf, 64);
            $got .= $buf;
            $reactor->stop if $got eq "ping-$wid";
          }
        )->watch($s1, 1, 0);
        my $ticks = 0;
        $r->timer(0.005 => sub { $ticks++; syswrite($s2, "ping-$wid") });
        $r->timer(2.0   => sub { shift->stop });
        $r->start;
        $r->reset;
        return ($got eq "ping-$wid" && $ticks == 1) ? 1 : 0;
      }
    );
  }
  my $ok_count = 0;
  for my $w (@workers) {
    $ok_count += ($w->join ? 1 : 0);
  }
  is $ok_count, 4, 'all 4 MT::Worker threads ran independent Mojo::Reactor::MT loops cleanly';
};

done_testing();
