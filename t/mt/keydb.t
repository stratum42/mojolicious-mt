use Mojo::Base -strict;

use Test::More;
use IO::Select;
use IO::Socket::INET;
use Mojo::MT qw(HAS_MT);
use Mojo::MT::KeyDB;
use POSIX qw(WNOHANG);
use Time::HiRes qw(sleep);

my $HAS_XS = Mojo::MT::KeyDB::HAS_KEYDB_XS;

# ============================================================================
# 1. RESP2 / RESP3 Encoding & Decoding Unit Tests (XS <-> Pure-Perl parity)
# ============================================================================

subtest 'RESP2/RESP3 encoding & decoding unit tests' => sub {
  my @encode_cases = (
    ['PING'],
    ['SET', 'mykey', 'myvalue'],
    ['SET', 'bin', "a\r\nb\x00c", 'EX', 60],
    ['MGET', 'k1', 'k2', '', 'k4'],
  );

  for my $args (@encode_cases) {
    my $pp = Mojo::MT::KeyDB::_pp_encode_cmd(@$args);
    like $pp, qr/\A\*\d+\r\n/, "pp encode starts with array header (@$args[0])";
    if ($HAS_XS) {
      my $xs = Mojo::MT::KeyDB::XS::encode_cmd(@$args);
      is $xs, $pp, "XS and PP encode_cmd match byte-for-byte ($args->[0])";
    }
  }

  my $long_str = 'x' x 64 . 'hello-neon-crlf-scan';
  my @decode_cases = (
    ['simple string',       "+OK\r\n",                            'OK',          0],
    ['long simple string',  "+$long_str\r\n",                     $long_str,     0],
    ['error',               "-ERR unknown command\r\n",           'ERR unknown command', 1],
    ['integer zero',        ":0\r\n",                             0,             0],
    ['integer positive',    ":42\r\n",                            42,            0],
    ['integer negative',    ":-1234567890\r\n",                   -1234567890,   0],
    ['bulk string',         "\$5\r\nhello\r\n",                   'hello',       0],
    ['bulk empty',          "\$0\r\n\r\n",                        '',            0],
    ['bulk binary',         "\$7\r\na\r\nb\x00cd\r\n",            "a\r\nb\x00cd", 0],
    ['bulk nil',            "\$-1\r\n",                           undef,         0],
    ['array empty',         "*0\r\n",                             [],            0],
    ['array nil',           "*-1\r\n",                            undef,         0],
    ['array mixed',         "*3\r\n\$3\r\nfoo\r\n:99\r\n\$-1\r\n", ['foo', 99, undef], 0],
    ['nested array',        "*2\r\n*2\r\n:1\r\n:2\r\n+hi\r\n",    [[1, 2], 'hi'], 0],
    ['RESP3 null',          "_\r\n",                              undef,         0],
    ['RESP3 bool true',     "#t\r\n",                             1,             0],
    ['RESP3 bool false',    "#f\r\n",                             0,             0],
    ['RESP3 double',        ",3.14159\r\n",                       3.14159,       0],
    ['RESP3 neg double',    ",-0.5\r\n",                          -0.5,          0],
    ['RESP3 map',           "%2\r\n+a\r\n:1\r\n\$1\r\nb\r\n\$3\r\ntwo\r\n", {a => 1, b => 'two'}, 0],
    ['RESP3 set',           "~2\r\n+x\r\n+y\r\n",                 ['x', 'y'],    0],
    ['RESP3 verbatim',      "=15\r\ntxt:Some string\r\n",         'Some string', 0],
    ['RESP3 blob error',    "!11\r\nSYNTAX fail\r\n",             'SYNTAX fail', 1],
  );

  for my $c (@decode_cases) {
    my ($name, $wire, $exp, $exp_err) = @$c;
    my ($pp_val, $pp_pos, $pp_err) = Mojo::MT::KeyDB::_pp_decode_reply($wire);
    is_deeply $pp_val, $exp,         "PP decode $name value";
    is $pp_pos,        length($wire), "PP decode $name consumed";
    is $pp_err,        $exp_err,      "PP decode $name is_err";

    if ($HAS_XS) {
      my ($xs_val, $xs_pos, $xs_err) = Mojo::MT::KeyDB::XS::decode_reply($wire);
      is_deeply $xs_val, $pp_val, "XS/PP parity value ($name)";
      is $xs_pos,        $pp_pos, "XS/PP parity consumed ($name)";
      is $xs_err,        $pp_err, "XS/PP parity is_err ($name)";
    }
  }

  # Incomplete frame handling
  for my $partial ('', '+O', '$5\r\nhel', "*2\r\n:1\r\n", "%1\r\n+k\r\n") {
    my ($pv, $pc) = Mojo::MT::KeyDB::_pp_decode_reply($partial);
    is $pc, 0, 'PP incomplete returns consumed=0';
    is $pv, undef, 'PP incomplete returns undef';
    if ($HAS_XS) {
      my ($xv, $xc) = Mojo::MT::KeyDB::XS::decode_reply($partial);
      is $xc, 0, 'XS incomplete returns consumed=0';
      is $xv, undef, 'XS incomplete returns undef';
    }
  }
};

# ============================================================================
# Lightweight loopback mock KeyDB RESP2 server (multi-client via IO::Select)
# ============================================================================

sub start_mock_keydb {
  my $listen = IO::Socket::INET->new(
    LocalAddr => '127.0.0.1',
    LocalPort => 0,
    Proto     => 'tcp',
    Listen    => 128,
    ReuseAddr => 1,
  ) or die "mock listen failed: $!";
  my $port = $listen->sockport;

  my $pid = fork();
  die "fork failed: $!" unless defined $pid;
  if ($pid == 0) {
    my $sel = IO::Select->new($listen);
    my %buf;
    my %kv;
    my %hashes;
    my $running = 1;
    local $SIG{TERM} = sub { $running = 0 };
    local $SIG{INT}  = sub { $running = 0 };

    my $enc_bulk = sub {
      my $v = shift;
      return "\$-1\r\n" unless defined $v;
      return '$' . length($v) . "\r\n" . $v . "\r\n";
    };

    my $handle_cmd = sub {
      my $args = shift;
      my $op = uc($args->[0] // '');
      if ($op eq 'PING') {
        return defined $args->[1] ? $enc_bulk->($args->[1]) : "+PONG\r\n";
      }
      if ($op eq 'AUTH' || $op eq 'SELECT') {
        return "+OK\r\n";
      }
      if ($op eq 'SET') {
        my ($k, $v) = ($args->[1], $args->[2]);
        my %opts = map { uc($_) => 1 } @{$args}[3 .. $#$args];
        return "\$-1\r\n" if $opts{NX} && exists $kv{$k};
        return "\$-1\r\n" if $opts{XX} && !exists $kv{$k};
        $kv{$k} = $v;
        return "+OK\r\n";
      }
      if ($op eq 'GET') {
        return $enc_bulk->($kv{$args->[1]});
      }
      if ($op eq 'INCR') {
        my $k = $args->[1];
        $kv{$k} = ($kv{$k} // 0) + 1;
        return ':' . $kv{$k} . "\r\n";
      }
      if ($op eq 'DECR') {
        my $k = $args->[1];
        $kv{$k} = ($kv{$k} // 0) - 1;
        return ':' . $kv{$k} . "\r\n";
      }
      if ($op eq 'DEL') {
        my $n = 0;
        for my $k (@{$args}[1 .. $#$args]) {
          $n++ if defined delete $kv{$k};
        }
        return ":$n\r\n";
      }
      if ($op eq 'EXISTS') {
        my $n = 0;
        for my $k (@{$args}[1 .. $#$args]) {
          $n++ if exists $kv{$k};
        }
        return ":$n\r\n";
      }
      if ($op eq 'EXPIRE') {
        return exists $kv{$args->[1]} ? ":1\r\n" : ":0\r\n";
      }
      if ($op eq 'MSET') {
        for (my $i = 1; $i < @$args; $i += 2) {
          $kv{$args->[$i]} = $args->[$i + 1];
        }
        return "+OK\r\n";
      }
      if ($op eq 'MGET') {
        my @keys = @{$args}[1 .. $#$args];
        my $out = '*' . scalar(@keys) . "\r\n";
        $out .= $enc_bulk->($kv{$_}) for @keys;
        return $out;
      }
      if ($op eq 'HSET') {
        my $k = $args->[1];
        my $added = 0;
        for (my $i = 2; $i < @$args; $i += 2) {
          $added++ unless exists $hashes{$k}{$args->[$i]};
          $hashes{$k}{$args->[$i]} = $args->[$i + 1];
        }
        return ":$added\r\n";
      }
      if ($op eq 'HGET') {
        my ($k, $f) = ($args->[1], $args->[2]);
        return $enc_bulk->($hashes{$k}{$f});
      }
      if ($op eq 'INFO') {
        return $enc_bulk->("# Server\r\nkeydb_mock:1\r\n");
      }
      return "-ERR unknown command '$op'\r\n";
    };

    while ($running) {
      my @ready = $sel->can_read(0.1);
      for my $s (@ready) {
        if ($s == $listen) {
          my $c = $listen->accept or next;
          binmode($c);
          $sel->add($c);
          $buf{$c} = '';
        } else {
          my $chunk = '';
          my $n = sysread($s, $chunk, 16384);
          if (!$n) {
            $sel->remove($s);
            delete $buf{$s};
            close($s);
            next;
          }
          $buf{$s} .= $chunk;
          my $resp = '';
          while (length($buf{$s})) {
            my $pos = 0;
            my ($ok, $req) = Mojo::MT::KeyDB::_pp_parse_reply(\$buf{$s}, \$pos, 0, 0);
            last unless $ok;
            substr($buf{$s}, 0, $pos, '');
            $resp .= $handle_cmd->($req) if ref $req eq 'ARRAY';
          }
          if (length $resp) {
            my $off = 0;
            while ($off < length($resp)) {
              my $w = syswrite($s, $resp, length($resp) - $off, $off);
              last unless $w;
              $off += $w;
            }
          }
        }
      }
    }
    close($listen);
    POSIX::_exit(0);
  }

  close($listen);
  return ($port, $pid);
}

my @CHILD_PIDS;
END {
  local $?;
  for my $p (@CHILD_PIDS) {
    next unless $p;
    kill 'TERM', $p;
    for (1 .. 20) {
      last if waitpid($p, WNOHANG) > 0;
      sleep 0.02;
    }
    kill 'KILL', $p;
    waitpid($p, WNOHANG);
  }
}

sub run_client_suite {
  my ($label, $kdb) = @_;
  subtest $label => sub {
    is $kdb->connect, $kdb, 'connect returns self';
    ok $kdb->is_connected, 'is_connected true';
    is $kdb->ping, 'PONG', 'PING -> PONG';
    is $kdb->ping('hello'), 'hello', 'PING with message';

    is $kdb->set('k1', 'v1', ex => 60), 'OK', 'SET with EX';
    is $kdb->get('k1'), 'v1', 'GET returns value';
    is $kdb->get('missing_key_xyz'), undef, 'GET missing returns undef';
    is $kdb->set('k1', 'v2', nx => 1), undef, 'SET NX on existing returns undef';
    is $kdb->exists('k1'), 1, 'EXISTS existing == 1';
    is $kdb->expire('k1', 120), 1, 'EXPIRE existing == 1';

    is $kdb->del('cnt'), ($kdb->exists('cnt') ? 1 : 0), 'DEL cleanup';
    is $kdb->incr('cnt'), 1, 'INCR -> 1';
    is $kdb->incr('cnt'), 2, 'INCR -> 2';
    is $kdb->decr('cnt'), 1, 'DECR -> 1';

    is $kdb->mset('m1' => 'a', 'm2' => 'b'), 'OK', 'MSET';
    is_deeply $kdb->mget('m1', 'm2', 'm_missing'), ['a', 'b', undef], 'MGET';

    ok $kdb->hset('h1', 'f1', 'val1') >= 0, 'HSET';
    is $kdb->hget('h1', 'f1'), 'val1', 'HGET';
    is $kdb->hget('h1', 'no_field'), undef, 'HGET missing';

    like $kdb->info, qr/# Server/i, 'INFO returns server section';

    # Pipeline (including an error element)
    my $pipe = $kdb->pipeline([
      ['SET', 'p1', 'one'],
      ['SET', 'p2', 'two'],
      ['GET', 'p1'],
      ['MGET', 'p1', 'p2'],
      ['NOSUCHCMD'],
      ['INCR', 'p_cnt'],
    ]);
    is ref $pipe, 'ARRAY', 'pipeline returns arrayref';
    is scalar(@$pipe), 6, 'pipeline returned 6 replies';
    is $pipe->[0], 'OK', 'pipeline[0] SET OK';
    is $pipe->[1], 'OK', 'pipeline[1] SET OK';
    is $pipe->[2], 'one', 'pipeline[2] GET';
    is_deeply $pipe->[3], ['one', 'two'], 'pipeline[3] MGET';
    isa_ok $pipe->[4], 'Mojo::MT::KeyDB::Error', 'pipeline[4] error wrapped';
    like "$pipe->[4]", qr/ERR/i, 'pipeline error stringifies';
    is $pipe->[5], 1, 'pipeline[5] INCR after error';

    # Top-level error throws
    eval { $kdb->cmd('NOSUCHCMD') };
    like $@, qr/KeyDB error:.*ERR/i, 'top-level error croaks';

    is $kdb->del('k1', 'cnt', 'm1', 'm2', 'p1', 'p2', 'p_cnt'), 7, 'DEL multiple keys';

    $kdb->disconnect;
    ok !$kdb->is_connected, 'is_connected false after disconnect';
    # Lazy reconnect on next command
    is $kdb->ping, 'PONG', 'lazy reconnect works after disconnect';
    $kdb->disconnect;
  };
}

# ============================================================================
# 2. Integration Tests against Loopback Mock KeyDB Server
# ============================================================================

my ($mock_port, $mock_pid) = start_mock_keydb();
push @CHILD_PIDS, $mock_pid;

run_client_suite(
  'Mock KeyDB (pure-Perl fallback)',
  Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $mock_port, use_xs => 0),
);

if ($HAS_XS) {
  run_client_suite(
    'Mock KeyDB (Tier-1 XS fast path)',
    Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $mock_port, use_xs => 1),
  );
}

# ============================================================================
# 3. Live Server Test (keydb-server / valkey-server / redis-server if present)
# ============================================================================

my $live_bin;
for my $cand (qw(keydb-server valkey-server redis-server /opt/homebrew/bin/keydb-server /opt/homebrew/bin/valkey-server /opt/homebrew/bin/redis-server)) {
  if ($cand =~ m{^/}) {
    if (-x $cand) { $live_bin = $cand; last }
  } else {
    for my $dir (split /:/, ($ENV{PATH} // '')) {
      if (-x "$dir/$cand") { $live_bin = "$dir/$cand"; last }
    }
    last if $live_bin;
  }
}

SKIP: {
  skip 'no local keydb-server/valkey-server/redis-server installed', 1 unless $live_bin;

  # Pick a free loopback port
  my $probe = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => 0, Proto => 'tcp', Listen => 1)
    or die "probe port failed: $!";
  my $live_port = $probe->sockport;
  close($probe);

  my $lpid = fork();
  die "fork live server failed: $!" unless defined $lpid;
  if ($lpid == 0) {
    open STDOUT, '>', '/dev/null';
    open STDERR, '>', '/dev/null';
    {
      exec($live_bin, '--bind', '127.0.0.1', '--port', $live_port,
           '--save', '', '--appendonly', 'no', '--loglevel', 'warning');
    }
    POSIX::_exit(127);
  }
  push @CHILD_PIDS, $lpid;

  # Wait up to 3s for server to accept PING
  my $ready = 0;
  my $probe_kdb = Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $live_port, timeout => 0.5);
  for (1 .. 60) {
    my $pong = eval { $probe_kdb->ping };
    if (defined $pong && $pong eq 'PONG') {
      $probe_kdb->disconnect;
      $ready = 1;
      last;
    }
    sleep 0.05;
  }
  skip "live server $live_bin did not start on port $live_port", 1 unless $ready;

  run_client_suite(
    "Live server ($live_bin on 127.0.0.1:$live_port, " . ($HAS_XS ? 'XS' : 'pure-Perl') . ')',
    Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $live_port),
  );
}

# ============================================================================
# 4. Multi-Worker perl5-mt Test (4 concurrent MT::Worker threads after freeze)
# ============================================================================

our $FROZEN_KDB;

sub _mt_keydb_worker {
  my ($wid, $port) = @_;
  # 1. Exercise the frozen Mojo::MT::KeyDB object created before MT::Image::freeze()
  #    Each worker lazily opens its own MY_CXT socket with zero locks & zero mutation of $FROZEN_KDB
  return 0 unless $FROZEN_KDB->ping eq 'PONG';
  my $ok = 1;
  for my $i (1 .. 50) {
    my $k = "w${wid}:k${i}";
    $FROZEN_KDB->set($k, "val-${wid}-${i}");
    my $got = $FROZEN_KDB->get($k);
    $ok = 0 unless defined $got && $got eq "val-${wid}-${i}";
    $FROZEN_KDB->incr('mt_shared_counter');
  }
  my $pipe = $FROZEN_KDB->pipeline([
    ['SET', "w${wid}:p1", "pipe-${wid}"],
    ['GET', "w${wid}:p1"],
    ['MGET', "w${wid}:k1", "w${wid}:k50"],
  ]);
  $ok = 0 unless $pipe->[0] eq 'OK'
    && $pipe->[1] eq "pipe-${wid}"
    && $pipe->[2][0] eq "val-${wid}-1"
    && $pipe->[2][1] eq "val-${wid}-50";

  # 2. Also verify a worker-local Mojo::MT::KeyDB->new() reuses the worker's MY_CXT slot
  my $local_kdb = Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $port);
  $ok = 0 unless $local_kdb->is_connected && $local_kdb->ping eq 'PONG';
  $local_kdb->disconnect;
  return $ok ? 1 : 0;
}

SKIP: {
  skip 'perl5-mt + MT::Worker + Mojo::MT::KeyDB::XS required for multi-worker test', 1
    unless HAS_MT && $HAS_XS && defined &MT::Worker::spawn;

  subtest '4 concurrent MT::Worker threads sharing frozen Mojo::MT::KeyDB' => sub {
    # Construct package-level client BEFORE MT::Image::freeze() so it is frozen in the image
    $FROZEN_KDB = Mojo::MT::KeyDB->new(host => '127.0.0.1', port => $mock_port);
    $FROZEN_KDB->del('mt_shared_counter');
    $FROZEN_KDB->disconnect;

    Mojo::MT::prepare_for_freeze();
    MT::Image::freeze();

    my @worker_ids;
    for my $wid (1 .. 4) {
      push @worker_ids, MT::Worker::spawn('main::_mt_keydb_worker', $wid, $mock_port);
    }

    my $passed = 0;
    for my $id (@worker_ids) {
      my $res = MT::Worker::join($id);
      $passed += ($res ? 1 : 0);
    }
    is $passed, 4, 'all 4 MT::Worker threads completed KeyDB ops & pipelines cleanly';
    is $FROZEN_KDB->get('mt_shared_counter'), '200',
      'shared INCR counter across 4 concurrent workers == 200';
    $FROZEN_KDB->disconnect;
  };
}

done_testing();
