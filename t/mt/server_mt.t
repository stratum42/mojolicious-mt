use Mojo::Base -strict;

use Test::More;
use File::Temp   qw(tempfile);
use IO::Socket::IP;
use Mojo::IOLoop::Server;
use Mojo::MT     qw(HAS_MT HAS_MT_SHARED_HASH);
use Mojo::MT::SharedCache;
use Mojo::Server::MT;
use Mojolicious;
use Mojolicious::Command::mt;
use Time::HiRes  qw(usleep);

# 1. SharedCache unit tests (works on both perl5-mt and stock Perl)
subtest 'Mojo::MT::SharedCache basic operations' => sub {
  my $c1 = Mojo::MT::SharedCache->new;
  ok defined $c1->id, 'cache has integer id';

  is $c1->set(alpha => 'one'), 'one', 'set returns value';
  ok $c1->exists('alpha'), 'exists true for set key';
  is $c1->get('alpha'), 'one', 'get returns stored value';

  my $c2 = Mojo::MT::SharedCache->attach($c1->id);
  is $c2->get('alpha'), 'one', 'attached handle sees same value';

  my $calls = 0;
  is $c2->get_or_set(beta => sub { $calls++; 'computed' }), 'computed', 'get_or_set computes on miss';
  is $c2->get_or_set(beta => sub { $calls++; 'other' }),    'computed', 'get_or_set returns cached on hit';
  is $calls, 1, 'callback invoked exactly once';

  my @keys = sort $c1->keys;
  is_deeply \@keys, [qw(alpha beta)], 'keys returns all keys';

  ok $c1->delete('alpha'), 'delete returns true';
  ok !$c1->exists('alpha'), 'deleted key no longer exists';
};

# 2. CLI command option parsing (works on both perl5-mt and stock Perl)
subtest 'Mojolicious::Command::mt build_server' => sub {
  my $cmd = Mojolicious::Command::mt->new(app => Mojolicious->new);
  my $srv = $cmd->build_server(
    '-w', '6',
    '-l', 'http://127.0.0.1:4321',
    '-c', '500',
    '-b', '256',
    '-k', '10',
    '-r', '5000',
    '-s',
  );
  isa_ok $srv, 'Mojo::Server::MT';
  is $srv->workers,            6, 'workers parsed';
  is_deeply $srv->listen,      ['http://127.0.0.1:4321'], 'listen parsed';
  is $srv->clients,            500, 'clients parsed';
  is $srv->backlog,            256, 'backlog parsed';
  is $srv->keep_alive_timeout, 10,  'keep_alive_timeout parsed';
  is $srv->max_requests,       5000, 'max_requests parsed';
  ok $srv->silent,             'silent parsed';
};

# Helper: send HTTP/1.1 requests over a single keep-alive socket
sub _http_keepalive {
  my ($port, @paths) = @_;
  my $sock = IO::Socket::IP->new(
    PeerAddr => '127.0.0.1',
    PeerPort => $port,
    Timeout  => 5,
  ) or die "connect 127.0.0.1:$port failed: $@";

  my @responses;
  for my $i (0 .. $#paths) {
    my $conn = ($i == $#paths) ? 'close' : 'keep-alive';
    print $sock "GET $paths[$i] HTTP/1.1\r\nHost: 127.0.0.1:$port\r\nConnection: $conn\r\n\r\n";

    my $head = '';
    while ($head !~ /\r\n\r\n/s) {
      my $n = sysread($sock, my $buf, 1);
      last unless $n;
      $head .= $buf;
    }
    my ($status) = $head =~ m{^HTTP/1\.1 (\d+)};
    my ($clen)   = $head =~ m{^Content-Length:\s*(\d+)}mi;
    my ($date)   = $head =~ m{^Date:\s*([^\r\n]+)}mi;
    my $body     = '';
    while (defined $clen && length($body) < $clen) {
      my $n = sysread($sock, my $buf, $clen - length($body));
      last unless $n;
      $body .= $buf;
    }
    push @responses, {status => $status, body => $body, date => $date};
  }
  close $sock;
  return @responses;
}

SKIP: {
  skip 'perl5-mt required for native multi-worker Mojo::Server::MT tests', 2 unless HAS_MT;

  require MT::Fault;
  MT::Fault::reset();

  # 3. Subprocess test: Mojolicious::Lite script invoked via `./lite.pl mt -w 2` + SIGTERM
  # (Run subprocess before freezing current process so fork/exec is completely clean)
  subtest 'Mojolicious::Lite script via CLI mt command + SIGTERM' => sub {
    my ($fh, $script) = tempfile(SUFFIX => '.pl', UNLINK => 1);
    print $fh <<'EOF';
use Mojolicious::Lite -signatures;
app->log->level('error');
get '/lite' => sub ($c) {
  no warnings 'once';
  $c->render(text => 'lite-ok:' . ($Mojo::Server::MT::WORKER_ID // 'x'));
};
app->start;
EOF
    close $fh;

    my $port = Mojo::IOLoop::Server->generate_port;
    my $pid  = fork();
    die "fork failed: $!" unless defined $pid;
    if ($pid == 0) {
      my @inc = map { ("-I", $_) } @INC;
      exec($^X, @inc, $script, 'mt', '-w', '2', '-l', "http://127.0.0.1:$port", '-s')
        or die "exec failed: $!";
    }

    # Wait for server to accept connections
    my $up = 0;
    for (1 .. 150) {
      my $s = IO::Socket::IP->new(PeerAddr => '127.0.0.1', PeerPort => $port, Timeout => 0.2);
      if ($s) { close $s; $up = 1; last }
      usleep(50_000);
    }
    ok $up, "Mojolicious::Lite mt server listening on port $port";

    if ($up) {
      my @res = _http_keepalive($port, '/lite', '/lite', '/lite');
      is scalar(@res), 3, 'received 3 keep-alive responses';
      ok((grep { ($_->{status} // 0) == 200 && ($_->{body} // '') =~ /^lite-ok:\d+$/ } @res) == 3,
        'all 3 Lite keep-alive responses returned 200 OK with worker id');
      ok defined $res[0]{date} && length $res[0]{date}, 'Date header present on Lite response';
    }

    kill 'TERM', $pid;
    my $reaped = 0;
    for (1 .. 100) {
      if (waitpid($pid, 1) > 0) { $reaped = 1; last }
      usleep(50_000);
    }
    if (!$reaped) {
      kill 'KILL', $pid;
      waitpid($pid, 0);
    }
    ok $reaped, 'Mojolicious::Lite mt server exited cleanly on SIGTERM';
    is $? >> 8, 0, 'exit code 0 on SIGTERM';
  };

  # 4. In-process 4-worker Mojolicious class app + SharedCache cross-worker visibility
  subtest '4-worker Mojo::Server::MT class app, keep-alive, SharedCache, 0 faults' => sub {
    {
      package TestMTApp;
      use Mojo::Base 'Mojolicious';

      sub startup {
        my $self = shift;
        $self->log->level('error');
        my $r = $self->routes;

        $r->get('/ping' => sub {
          my $c = shift;
          $c->render(text => 'pong:' . $Mojo::Server::MT::WORKER_ID);
        });

        $r->get('/cache/set/:k/:v' => sub {
          my $c     = shift;
          my $cache = Mojo::Server::MT->shared_cache_for_worker;
          $cache->set($c->param('k'), $c->param('v'));
          $c->render(text => 'stored:' . $Mojo::Server::MT::WORKER_ID);
        });

        $r->get('/cache/get/:k' => sub {
          my $c     = shift;
          my $cache = Mojo::Server::MT->shared_cache_for_worker;
          my $val   = $cache->get($c->param('k')) // 'MISSING';
          $c->render(text => $val);
        });

        $r->get('/tmpl' => sub {
          my $c = shift;
          $c->render(inline => 'Hello <%= $who %> & <%= "<tag>" %>', who => 'MT');
        });
      }
    }

    my $cache  = Mojo::MT::SharedCache->new;
    my $server = Mojo::Server::MT->new(
      app_class               => 'TestMTApp',
      listen                  => ['http://127.0.0.1'],
      workers                 => 4,
      max_requests            => 1000,
      shared_cache            => $cache,
      shutdown_check_interval => 0.02,
      silent                  => 1,
    );

    $server->start;
    my $port = $server->ports->[0];
    ok $port && $port > 0, "4-worker MT server bound ephemeral port $port";

    # Keep-alive burst across 8 separate connections x 5 requests = 40 requests
    my $ok_count = 0;
    my %seen_dates;
    for my $conn (1 .. 8) {
      my @res = _http_keepalive($port, ('/ping') x 5);
      for my $r (@res) {
        $ok_count++ if ($r->{status} // 0) == 200 && ($r->{body} // '') =~ /^pong:[0-3]$/;
        $seen_dates{$r->{date}}++ if defined $r->{date};
      }
    }
    is $ok_count, 40, 'all 40 keep-alive requests succeeded across 4 workers';
    ok keys(%seen_dates) >= 1, 'cached Date header populated on all responses';

    # Cross-worker SharedCache visibility
    my ($set_res) = _http_keepalive($port, '/cache/set/shared_key/hello_mt');
    is $set_res->{status}, 200, 'cache set returned 200';
    is $cache->get('shared_key'), 'hello_mt', 'main thread sees value written by worker';

    my $cache_hits = 0;
    for (1 .. 12) {
      my ($get_res) = _http_keepalive($port, '/cache/get/shared_key');
      $cache_hits++ if ($get_res->{status} // 0) == 200 && ($get_res->{body} // '') eq 'hello_mt';
    }
    is $cache_hits, 12, 'all 12 subsequent requests across workers saw shared cache value';

    # EP template rendering + xml_escape inside post-freeze MT workers
    my ($tmpl_res) = _http_keepalive($port, '/tmpl');
    is $tmpl_res->{status}, 200, 'EP template route returned 200';
    is $tmpl_res->{body}, "Hello MT & &lt;tag&gt;\n", 'EP template rendered with xml_escape cleanly';

    # Graceful shutdown & join
    $server->stop;
    my $results = $server->join_workers;
    is scalar(@$results), 4, 'joined all 4 workers';
    is_deeply [sort @$results], [qw(ok:0 ok:1 ok:2 ok:3)], 'all 4 workers exited cleanly';
    is MT::Fault::count(), 0, 'zero MT_IMMORTAL_MUTATION or runtime faults';
  };
}

done_testing();
