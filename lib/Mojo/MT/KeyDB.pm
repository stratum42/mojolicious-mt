package Mojo::MT::KeyDB;
use Mojo::Base -base;

use Carp qw(croak);
use Errno qw(EINTR);
use IO::Socket::IP;
use IO::Socket::UNIX;
use Mojo::MT qw(HAS_MT);
use Socket qw(IPPROTO_TCP TCP_NODELAY);

use constant HAS_KEYDB_XS => (HAS_MT && !$ENV{MOJO_NO_MT_ACCEL} && !$ENV{MOJO_NO_MT_KEYDB_XS})
  ? (eval { require Mojo::MT::KeyDB::XS; 1 } ? 1 : 0)
  : 0;

has host     => '127.0.0.1';
has port     => 6379;
has path     => undef;
has timeout  => 5;
has db       => 0;
has password => undef;
has use_xs   => sub { HAS_KEYDB_XS };

# Pure-Perl per-endpoint connection cache (used on stock Perl or when XS is disabled)
my %PP_CONNS;

sub new {
  my $self = shift->SUPER::new(@_);
  # Eagerly materialize defaults so frozen objects never mutate $self in workers
  $self->{host}     = '127.0.0.1'   unless defined $self->{host};
  $self->{port}     = 6379          unless defined $self->{port};
  $self->{path}     = undef         unless exists  $self->{path};
  $self->{timeout}  = 5             unless defined $self->{timeout};
  $self->{db}       = 0             unless defined $self->{db};
  $self->{password} = undef         unless exists  $self->{password};
  $self->{use_xs}   = HAS_KEYDB_XS  unless defined $self->{use_xs};
  return $self;
}

sub _xs_enabled {
  my $self = shift;
  return ($self->{use_xs} && HAS_KEYDB_XS) ? 1 : 0;
}

sub _endpoint_key {
  my $self = shift;
  my $target = (defined $self->{path} && length $self->{path})
    ? 'unix:' . $self->{path}
    : 'tcp:' . ($self->{host} // '127.0.0.1') . ':' . ($self->{port} // 6379);
  return join("\0", $target, $self->{db} // 0, $self->{password} // '');
}

# ----------------------------------------------------------------------------
# Pure-Perl RESP2/RESP3 encoder & streaming reply parser
# ----------------------------------------------------------------------------

sub _pp_encode_cmd {
  croak 'encode_cmd requires at least 1 argument' unless @_;
  my $out = '*' . scalar(@_) . "\r\n";
  for my $arg (@_) {
    my $s = defined $arg ? "$arg" : '';
    utf8::encode($s) if utf8::is_utf8($s);
    $out .= '$' . length($s) . "\r\n" . $s . "\r\n";
  }
  return $out;
}

sub _pp_parse_reply {
  my ($buf_ref, $pos_ref, $wrap_errors, $depth) = @_;
  $depth //= 0;
  croak 'RESP protocol error: nesting depth exceeded (64)' if $depth > 64;

  my $len = length($$buf_ref);
  my $pos = $$pos_ref;
  return (0, undef, 0) if $pos >= $len;

  my $type = substr($$buf_ref, $pos, 1);
  my $cur  = $pos + 1;

  if ($type eq '+') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    $$pos_ref = $crlf + 2;
    return (1, substr($$buf_ref, $cur, $crlf - $cur), 0);
  }

  if ($type eq '-') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $msg = substr($$buf_ref, $cur, $crlf - $cur);
    $$pos_ref = $crlf + 2;
    my $val = $wrap_errors ? Mojo::MT::KeyDB::Error->new($msg) : $msg;
    return (1, $val, 1);
  }

  if ($type eq ':') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $raw = substr($$buf_ref, $cur, $crlf - $cur);
    croak 'RESP protocol error: Invalid RESP integer' unless $raw =~ /\A[+-]?\d+\z/;
    $$pos_ref = $crlf + 2;
    return (1, int($raw), 0);
  }

  if ($type eq '$' || $type eq '!' || $type eq '=') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $raw_len = substr($$buf_ref, $cur, $crlf - $cur);
    croak 'RESP protocol error: Invalid RESP bulk length' unless $raw_len =~ /\A-?\d+\z/;
    my $blen = int($raw_len);
    my $body_start = $crlf + 2;
    if ($blen < 0) {
      croak 'RESP protocol error: Invalid negative RESP bulk length' unless $blen == -1;
      $$pos_ref = $body_start;
      return (1, undef, 0);
    }
    return (0, undef, 0) if $body_start + $blen + 2 > $len;
    croak 'RESP protocol error: Missing CRLF after RESP bulk string'
      unless substr($$buf_ref, $body_start + $blen, 2) eq "\r\n";
    my $data = substr($$buf_ref, $body_start, $blen);
    $$pos_ref = $body_start + $blen + 2;
    if ($type eq '!') {
      my $val = $wrap_errors ? Mojo::MT::KeyDB::Error->new($data) : $data;
      return (1, $val, 1);
    }
    if ($type eq '=' && $blen >= 4 && substr($data, 3, 1) eq ':') {
      $data = substr($data, 4);
    }
    return (1, $data, 0);
  }

  if ($type eq '*' || $type eq '~') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $raw_cnt = substr($$buf_ref, $cur, $crlf - $cur);
    croak 'RESP protocol error: Invalid RESP array length' unless $raw_cnt =~ /\A-?\d+\z/;
    my $count = int($raw_cnt);
    my $p = $crlf + 2;
    if ($count < 0) {
      croak 'RESP protocol error: Invalid negative RESP array length' unless $count == -1;
      $$pos_ref = $p;
      return (1, undef, 0);
    }
    my @arr;
    for (1 .. $count) {
      my ($ok, $elem) = _pp_parse_reply($buf_ref, \$p, 1, $depth + 1);
      return (0, undef, 0) unless $ok;
      push @arr, $elem;
    }
    $$pos_ref = $p;
    return (1, \@arr, 0);
  }

  if ($type eq '_') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    $$pos_ref = $crlf + 2;
    return (1, undef, 0);
  }

  if ($type eq '#') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $b = substr($$buf_ref, $cur, $crlf - $cur);
    croak 'RESP protocol error: Invalid RESP3 boolean' unless $b eq 't' || $b eq 'f';
    $$pos_ref = $crlf + 2;
    return (1, $b eq 't' ? 1 : 0, 0);
  }

  if ($type eq ',') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $raw = substr($$buf_ref, $cur, $crlf - $cur);
    $$pos_ref = $crlf + 2;
    if ($raw eq 'inf')  { return (1, 0 + 'inf',  0) }
    if ($raw eq '-inf') { return (1, 0 + '-inf', 0) }
    if ($raw eq 'nan')  { return (1, 0 + 'nan',  0) }
    croak 'RESP protocol error: Invalid RESP3 double value'
      unless $raw =~ /\A[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?\z/;
    return (1, 0.0 + $raw, 0);
  }

  if ($type eq '%') {
    my $crlf = index($$buf_ref, "\r\n", $cur);
    return (0, undef, 0) if $crlf < 0;
    my $raw_cnt = substr($$buf_ref, $cur, $crlf - $cur);
    croak 'RESP protocol error: Invalid RESP3 map length' unless $raw_cnt =~ /\A\d+\z/;
    my $pairs = int($raw_cnt);
    my $p = $crlf + 2;
    my %map;
    for (1 .. $pairs) {
      my ($ok1, $k) = _pp_parse_reply($buf_ref, \$p, 1, $depth + 1);
      return (0, undef, 0) unless $ok1;
      my ($ok2, $v) = _pp_parse_reply($buf_ref, \$p, 1, $depth + 1);
      return (0, undef, 0) unless $ok2;
      $map{defined $k ? $k : ''} = $v;
    }
    $$pos_ref = $p;
    return (1, \%map, 0);
  }

  croak sprintf('RESP protocol error: Unexpected RESP type byte: 0x%02x', ord($type));
}

sub _pp_decode_reply {
  my ($buf) = @_;
  $buf //= '';
  my $pos = 0;
  my ($ok, $val, $is_err) = _pp_parse_reply(\$buf, \$pos, 0, 0);
  return wantarray ? ($ok ? ($val, $pos, $is_err) : (undef, 0, 0)) : ($ok ? $val : undef);
}

# ----------------------------------------------------------------------------
# Pure-Perl socket I/O
# ----------------------------------------------------------------------------

sub _pp_send_all {
  my ($sock, $data) = @_;
  my $off = 0;
  my $len = length($data);
  while ($off < $len) {
    my $n = syswrite($sock, $data, $len - $off, $off);
    if (!defined $n) {
      next if $! == EINTR;
      croak "Mojo::MT::KeyDB send failed: $!";
    }
    croak 'Mojo::MT::KeyDB send returned 0 (connection closed)' if $n == 0;
    $off += $n;
  }
}

sub _pp_recv_reply {
  my ($conn, $wrap_errors) = @_;
  my $sock = $conn->{sock};
  while (1) {
    if ($conn->{rpos} < length($conn->{rbuf})) {
      my ($ok, $val, $is_err) = _pp_parse_reply(\$conn->{rbuf}, \$conn->{rpos}, $wrap_errors, 0);
      if ($ok) {
        if ($conn->{rpos} >= length($conn->{rbuf})) {
          $conn->{rbuf} = '';
          $conn->{rpos} = 0;
        } elsif ($conn->{rpos} > 4096) {
          substr($conn->{rbuf}, 0, $conn->{rpos}, '');
          $conn->{rpos} = 0;
        }
        return ($val, $is_err);
      }
    }
    if ($conn->{rpos} > 0) {
      substr($conn->{rbuf}, 0, $conn->{rpos}, '');
      $conn->{rpos} = 0;
    }
    my $chunk = '';
    my $n = sysread($sock, $chunk, 8192);
    if (!defined $n) {
      next if $! == EINTR;
      close($sock);
      $conn->{sock} = undef;
      croak "Mojo::MT::KeyDB recv failed: $!";
    }
    if ($n == 0) {
      close($sock);
      $conn->{sock} = undef;
      croak 'Mojo::MT::KeyDB connection closed by peer';
    }
    $conn->{rbuf} .= $chunk;
  }
}

sub _pp_open_conn {
  my $self = shift;
  my $timeout = $self->{timeout} // 5;
  my $sock;
  if (defined $self->{path} && length $self->{path}) {
    $sock = IO::Socket::UNIX->new(
      Type    => SOCK_STREAM,
      Peer    => $self->{path},
      Timeout => $timeout,
    ) or croak "Mojo::MT::KeyDB connect failed ($self->{path}): $!";
  } else {
    my $host = $self->{host} // '127.0.0.1';
    my $port = $self->{port} // 6379;
    $sock = IO::Socket::IP->new(
      PeerHost => $host,
      PeerPort => $port,
      Proto    => 'tcp',
      Timeout  => $timeout,
    ) or croak "Mojo::MT::KeyDB connect failed ($host:$port): $@";
    setsockopt($sock, IPPROTO_TCP, TCP_NODELAY, pack('l', 1));
  }
  binmode($sock);

  my $conn = {sock => $sock, rbuf => '', rpos => 0};

  if (defined $self->{password} && length $self->{password}) {
    _pp_send_all($sock, _pp_encode_cmd('AUTH', $self->{password}));
    my ($reply, $is_err) = _pp_recv_reply($conn, 0);
    if ($is_err) {
      close($sock);
      croak "KeyDB error: $reply";
    }
  }
  if ($self->{db}) {
    _pp_send_all($sock, _pp_encode_cmd('SELECT', $self->{db}));
    my ($reply, $is_err) = _pp_recv_reply($conn, 0);
    if ($is_err) {
      close($sock);
      croak "KeyDB error: $reply";
    }
  }
  return $conn;
}

sub _pp_get_conn {
  my $self = shift;
  my $key  = $self->_endpoint_key;
  my $conn = $PP_CONNS{$key};
  return $conn if $conn && $conn->{sock};
  $conn = $self->_pp_open_conn;
  $PP_CONNS{$key} = $conn unless Internals::SvREADONLY(%PP_CONNS);
  return $conn;
}

# ----------------------------------------------------------------------------
# Public connection lifecycle API
# ----------------------------------------------------------------------------

sub connect {
  my $self = shift;
  if ($self->_xs_enabled) {
    Mojo::MT::KeyDB::XS::connect(
      $self->{host}, $self->{port}, $self->{path},
      $self->{timeout}, $self->{db}, $self->{password},
    );
  } else {
    $self->_pp_get_conn;
  }
  return $self;
}

sub disconnect {
  my $self = shift;
  if ($self->_xs_enabled) {
    Mojo::MT::KeyDB::XS::disconnect(
      $self->{host}, $self->{port}, $self->{path},
      $self->{db}, $self->{password},
    );
  } else {
    my $key = $self->_endpoint_key;
    if (my $conn = delete $PP_CONNS{$key}) {
      close($conn->{sock}) if $conn->{sock};
    }
  }
  return $self;
}

sub is_connected {
  my $self = shift;
  if ($self->_xs_enabled) {
    return Mojo::MT::KeyDB::XS::is_connected(
      $self->{host}, $self->{port}, $self->{path},
      $self->{db}, $self->{password},
    ) ? 1 : 0;
  }
  my $conn = $PP_CONNS{$self->_endpoint_key};
  return ($conn && $conn->{sock}) ? 1 : 0;
}

# ----------------------------------------------------------------------------
# Core command & pipeline API
# ----------------------------------------------------------------------------

sub cmd {
  my ($self, @args) = @_;
  croak 'cmd requires at least 1 argument' unless @args;

  if ($self->_xs_enabled) {
    return Mojo::MT::KeyDB::XS::keydb_cmd(
      $self->{host}, $self->{port}, $self->{path},
      $self->{timeout}, $self->{db}, $self->{password},
      @args,
    );
  }

  my $key  = $self->_endpoint_key;
  my $was  = ($PP_CONNS{$key} && $PP_CONNS{$key}{sock}) ? 1 : 0;
  my $wire = _pp_encode_cmd(@args);

  for my $attempt (0, 1) {
    my $conn = $self->_pp_get_conn;
    my ($reply, $is_err);
    my $ok = eval {
      _pp_send_all($conn->{sock}, $wire);
      ($reply, $is_err) = _pp_recv_reply($conn, 0);
      1;
    };
    if (!$ok) {
      my $err = $@;
      delete $PP_CONNS{$key} unless Internals::SvREADONLY(%PP_CONNS);
      if ($was && $attempt == 0) {
        $was = 0;
        next;
      }
      die $err;
    }
    croak "KeyDB error: $reply" if $is_err;
    return $reply;
  }
}

sub pipeline {
  my ($self, $cmds) = @_;
  croak 'pipeline expects an arrayref of commands' unless ref $cmds eq 'ARRAY';
  return [] unless @$cmds;

  if ($self->_xs_enabled) {
    return Mojo::MT::KeyDB::XS::keydb_pipeline(
      $self->{host}, $self->{port}, $self->{path},
      $self->{timeout}, $self->{db}, $self->{password},
      $cmds,
    );
  }

  my $wire = '';
  for my $i (0 .. $#$cmds) {
    my $c = $cmds->[$i];
    croak "pipeline element $i is not an arrayref" unless ref $c eq 'ARRAY';
    croak "pipeline element $i is empty" unless @$c;
    $wire .= _pp_encode_cmd(@$c);
  }

  my $conn = $self->_pp_get_conn;
  _pp_send_all($conn->{sock}, $wire);
  my @res;
  for (1 .. scalar(@$cmds)) {
    my ($reply) = _pp_recv_reply($conn, 1);
    push @res, $reply;
  }
  return \@res;
}

# ----------------------------------------------------------------------------
# Convenience KeyDB/Redis commands
# ----------------------------------------------------------------------------

sub ping {
  my ($self, $msg) = @_;
  return defined $msg ? $self->cmd('PING', $msg) : $self->cmd('PING');
}

sub get    { shift->cmd('GET', shift) }
sub incr   { shift->cmd('INCR', shift) }
sub decr   { shift->cmd('DECR', shift) }
sub del    { shift->cmd('DEL', @_) }
sub exists { shift->cmd('EXISTS', @_) }
sub expire { shift->cmd('EXPIRE', shift, shift) }
sub mget   { shift->cmd('MGET', @_) }
sub mset   { shift->cmd('MSET', @_) }
sub hget   { shift->cmd('HGET', shift, shift) }
sub hset   { shift->cmd('HSET', @_) }

sub info {
  my ($self, $section) = @_;
  return defined $section ? $self->cmd('INFO', $section) : $self->cmd('INFO');
}

sub set {
  my ($self, $key, $val, %opts) = @_;
  my @args = ('SET', $key, $val);
  for my $k (qw(ex px exat pxat)) {
    my $v = $opts{$k} // $opts{uc $k};
    push @args, uc($k), $v if defined $v;
  }
  push @args, 'NX'      if $opts{nx}      || $opts{NX};
  push @args, 'XX'      if $opts{xx}      || $opts{XX};
  push @args, 'KEEPTTL' if $opts{keepttl} || $opts{KEEPTTL};
  push @args, 'GET'     if $opts{get}     || $opts{GET};
  return $self->cmd(@args);
}

package Mojo::MT::KeyDB::Error;
use strict;
use warnings;
use overload '""' => sub { ${$_[0]} }, fallback => 1;

sub new     { my ($class, $msg) = @_; bless \$msg, $class }
sub message { ${$_[0]} }

1;

=encoding utf8

=head1 NAME

Mojo::MT::KeyDB - Thread-safe KeyDB and Redis client for mojolicious-mt

=head1 SYNOPSIS

  use Mojo::MT::KeyDB;

  my $kdb = Mojo::MT::KeyDB->new(host => '127.0.0.1', port => 6379);
  $kdb->set('greeting', 'hello', ex => 60);
  my $val = $kdb->get('greeting');
  my $n   = $kdb->incr('counter');

  my $replies = $kdb->pipeline([
    ['SET', 'a', '1'],
    ['SET', 'b', '2'],
    ['MGET', 'a', 'b'],
  ]);

=head1 DESCRIPTION

L<Mojo::MT::KeyDB> is a KeyDB and Redis client that uses the optional
thread-safe L<Mojo::MT::KeyDB::XS> C/NEON extension when available, and falls
back to a pure-Perl RESP2/RESP3 implementation on stock Perl or when
C<MOJO_NO_MT_ACCEL=1> is set.

Under C<perl5-mt>, each C<MT::Worker> thread lazily maintains its own
per-worker socket pool in C<MY_CXT>, so a L<Mojo::MT::KeyDB> object constructed
before C<MT::Image::freeze()> can be used from worker threads without locks or
mutation of frozen state.

=head1 SEE ALSO

L<Mojo::MT>, L<Mojo::MT::KeyDB::XS>, L<Mojo::Server::MT>.

=cut
