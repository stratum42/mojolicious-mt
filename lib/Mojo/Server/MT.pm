package Mojo::Server::MT;
use Mojo::Base 'Mojo::Server';

use Carp         qw(croak);
use IO::Socket::IP;
use Mojo::IOLoop;
use Mojo::IOLoop::Server;
use Mojo::JSON   qw(decode_json encode_json);
use Mojo::Loader qw(load_class);
use Mojo::MT     qw(HAS_MT HAS_MT_SHARED_HASH);
use Mojo::MT::SharedCache;
use Mojo::Server::Daemon;
use Mojo::URL;
use Socket       qw(SOCK_STREAM SOMAXCONN);
use Sub::Util    ();
use Symbol       ();
use Time::HiRes  qw(usleep);

has accepts                 => 0;
has app_class               => undef;
has app_file                => undef;
has app_mode                => undef;
has backlog                 => undef;
has clients                 => undef;
has inactivity_timeout      => sub { $ENV{MOJO_INACTIVITY_TIMEOUT} // 30 };
has keep_alive_timeout      => sub { $ENV{MOJO_KEEP_ALIVE_TIMEOUT} // 5 };
has listen                  => sub { [split /,/, $ENV{MOJO_LISTEN} || 'http://*:3000'] };
has max_requests            => 100;
has shared_cache            => undef;
has shutdown_check_interval => 0.05;
has silent                  => undef;
has workers                 => sub { _detect_cpus() };

# Alias max_clients -> clients for parity with Mojo::Server::Daemon / Prefork
sub max_clients { shift->clients(@_) }

# Worker-isolated globals (per-worker GP indirection under perl5-mt)
our $IMAGE_FROZEN      = 0;
our $FREEZE_PATCHED    = 0;
our $SYMBOL_GENSEQ     = 0;
our $WORKER_ID         = undef;
our $SHARED_CACHE_ID   = undef;
our $SHARED_CACHE      = undef;

sub shared_cache_for_worker {
  return $SHARED_CACHE if $SHARED_CACHE;
  return $SHARED_CACHE = Mojo::MT::SharedCache->attach($SHARED_CACHE_ID)
    if defined $SHARED_CACHE_ID;
  return undef;
}

sub ports { $_[0]{_ports} || [] }

sub run {
  my $self = shift;

  # Graceful fallback to single-process Daemon on stock Perl
  unless (HAS_MT) {
    warn "Mojo::Server::MT requires perl5-mt; falling back to Mojo::Server::Daemon\n" unless $self->silent;
    my $daemon = $self->_build_fallback_daemon;
    return $daemon->run;
  }

  $self->start;

  my $shutdown = $self->{_shutdown_cell};
  my $alive    = $self->{_alive_cell};

  local $SIG{INT} = local $SIG{TERM} = sub { $shutdown->set(1) };

  # Sleep in short intervals so SIGINT/SIGTERM handlers fire promptly
  while (!$shutdown->get) {
    last if $alive->get == 0 && $self->{_started_workers};
    select(undef, undef, undef, 0.05);
  }

  return $self->join_workers;
}

sub start {
  my $self = shift;
  croak 'Mojo::Server::MT::start requires perl5-mt' unless HAS_MT;

  require MT::Cell;
  require MT::Fault;

  # 1. Preload core XS modules & Mojolicious runtime before MT::Image::freeze()
  _preload_core_modules();

  # 2. Patch Symbol::gensym & Sub::Util::set_subname so workers don't mutate frozen SVs/CVs
  _patch_for_freeze();

  # 3. Determine app loading strategy (class vs file) & preload app code
  my ($app_class, $app_file, $app_mode) = $self->_resolve_app_spec;
  $ENV{MOJO_MODE} = $app_mode if defined $app_mode && !$ENV{MOJO_MODE};

  if ($app_class && !$app_file) {
    my $e = load_class($app_class);
    die ref $e ? $e : qq{Can't load application class "$app_class": $e\n} if $e;
    # Pre-warm DATA sections before freeze so workers never race on shared *DATA handles
    Mojo::Loader::data_section($app_class);
    Mojo::Loader::data_section('main');
  }

  # 4. Resolve listen URLs (and on non-Linux OSes like macOS where SO_REUSEPORT does not
  #    load-balance TCP accepts across sockets, bind one shared listen socket per URL)
  my ($resolved_listen, $ports, $shared_socks) = $self->_resolve_listen_urls;
  $self->{_resolved_listen} = $resolved_listen;
  $self->{_ports}           = $ports;
  $self->{_shared_socks}    = $shared_socks;

  # 5. Create shared MT::Cell coordination flags & optional SharedCache
  my $shutdown_cell = $self->{_shutdown_cell} = MT::Cell->new(0);
  my $ready_cell    = $self->{_ready_cell}    = MT::Cell->new(0);
  my $alive_cell    = $self->{_alive_cell}    = MT::Cell->new(0);

  my $cache_id;
  if (defined(my $sc = $self->shared_cache)) {
    $cache_id = ref $sc ? $sc->id : $sc;
  }
  elsif (HAS_MT_SHARED_HASH) {
    my $sc = Mojo::MT::SharedCache->new;
    $self->shared_cache($sc);
    $cache_id = $sc->id;
  }
  $self->{_shared_cache_id} = $cache_id;

  # 6. Serialize worker config as plain JSON string (no mutable refs across spawn)
  my $cfg = {
    accepts                 => $self->accepts,
    app_class               => $app_class,
    app_file                => $app_file,
    app_mode                => $app_mode,
    backlog                 => $self->backlog,
    clients                 => $self->clients,
    inactivity_timeout      => $self->inactivity_timeout,
    keep_alive_timeout      => $self->keep_alive_timeout,
    listen                  => $resolved_listen,
    max_requests            => $self->max_requests,
    reverse_proxy           => $self->reverse_proxy ? 1 : 0,
    shutdown_check_interval => $self->shutdown_check_interval,
    trusted_proxies         => $self->trusted_proxies,
  };
  my $cfg_json = encode_json($cfg);

  # 7. Prepare Mojolicious globals and seal the shared boot image
  if (!$IMAGE_FROZEN) {
    Mojo::MT::prepare_for_freeze();
    MT::Image::freeze();
    $IMAGE_FROZEN = 1;
  }

  # 8. Spawn N native MT::Worker threads
  my $n = $self->workers || 1;
  my @worker_ids;
  for my $idx (0 .. $n - 1) {
    push @worker_ids,
      MT::Worker::spawn(
      'Mojo::Server::MT::_worker_main',
      $idx, $cfg_json, $shutdown_cell->id, $ready_cell->id, $alive_cell->id, $cache_id,
      );
  }
  $self->{_worker_ids}      = \@worker_ids;
  $self->{_started_workers} = $n;

  # 9. Wait for all workers to bind and start listening
  $self->wait_ready(10);

  unless ($self->silent) {
    for my $i (0 .. $#$resolved_listen) {
      my $url = Mojo::URL->new($resolved_listen->[$i]);
      $url->query->pairs([]);
      $url->host('127.0.0.1') if ($url->host // '') eq '*';
      say "Web application available at $url ($n MT workers)";
    }
  }

  return $self;
}

sub wait_ready {
  my ($self, $timeout) = @_;
  $timeout //= 10;
  my $target = $self->{_started_workers} || 0;
  my $ready  = $self->{_ready_cell} or return 1;
  my $steps  = int($timeout / 0.01);
  while ($ready->get < $target && $steps-- > 0) {
    usleep(10_000);
  }
  return $ready->get >= $target ? 1 : 0;
}

sub stop {
  my $self = shift;
  $self->{_shutdown_cell}->set(1) if $self->{_shutdown_cell};
  return $self;
}

sub join_workers {
  my $self = shift;
  my $wids = delete $self->{_worker_ids} or return [];
  my @results = map { MT::Worker::join($_) } @$wids;
  if (my $socks = delete $self->{_shared_socks}) {
    close $_ for @$socks;
  }
  return \@results;
}

sub _worker_main {
  my ($idx, $cfg_json, $shutdown_id, $ready_id, $alive_id, $cache_id) = @_;

  $WORKER_ID       = $idx;
  $SHARED_CACHE_ID = $cache_id;
  $SHARED_CACHE    = defined $cache_id ? Mojo::MT::SharedCache->attach($cache_id) : undef;

  my $shutdown = MT::Cell->attach($shutdown_id);
  my $ready    = MT::Cell->attach($ready_id);
  my $alive    = MT::Cell->attach($alive_id);
  $alive->update(sub { $_[0] + 1 });

  my @open_socks;
  my $ok = eval {
    my $cfg = decode_json($cfg_json);

    # Instantiate worker-local application (avoids pre-freeze weakref traps)
    my $app;
    if (defined $cfg->{app_file}) {
      $app = Mojo::Server->new->load_app($cfg->{app_file});
    }
    elsif (defined $cfg->{app_class}) {
      my $class = $cfg->{app_class};
      load_class($class);
      $app = $class->new;
    }
    else {
      require Mojo::HelloWorld;
      $app = Mojo::HelloWorld->new;
    }
    $app->mode($cfg->{app_mode}) if defined $cfg->{app_mode} && $app->can('mode');

    # Bind worker-private SO_REUSEPORT sockets (Linux) or reuse shared listen fd
    # (macOS/BSD, where SO_REUSEPORT does not load-balance across sockets).
    my @worker_listen;
    for my $loc (@{$cfg->{listen}}) {
      my $url   = Mojo::URL->new($loc);
      my $proto = $url->protocol;
      if (($proto eq 'http' || $proto eq 'https') && !defined $url->query->param('fd')) {
        my $host = $url->host;
        my $addr = (!defined $host || $host eq '*') ? '0.0.0.0' : $host;
        $addr =~ y/[]//d;
        my $port = $url->port || ($proto eq 'https' ? 443 : 80);

        my $sock = IO::Socket::IP->new(
          LocalAddr => $addr,
          LocalPort => $port,
          Listen    => $cfg->{backlog} // SOMAXCONN,
          Type      => SOCK_STREAM,
          ReuseAddr => 1,
          ReusePort => 1,
        ) or die "Worker $idx can't bind $addr:$port: $@\n";
        push @open_socks, $sock;

        $url->query->param(fd => fileno($sock));
        push @worker_listen, $url->to_string;
      }
      else {
        push @worker_listen, $loc;
      }
    }

    my $loop = Mojo::IOLoop->singleton;
    $loop->max_accepts($cfg->{accepts}) if $cfg->{accepts};

    my $daemon = Mojo::Server::Daemon->new(
      app                => $app,
      backlog            => $cfg->{backlog},
      inactivity_timeout => $cfg->{inactivity_timeout},
      ioloop             => $loop,
      keep_alive_timeout => $cfg->{keep_alive_timeout},
      listen             => \@worker_listen,
      max_clients        => $cfg->{clients},
      max_requests       => $cfg->{max_requests},
      reverse_proxy      => $cfg->{reverse_proxy},
      silent             => 1,
      trusted_proxies    => $cfg->{trusted_proxies},
    );

    $daemon->start;

    # Daemon's IO::Socket::IP->new_from_fd dup'd each fd; close our copies
    close $_ for @open_socks;
    @open_socks = ();

    $ready->update(sub { $_[0] + 1 });

    my $interval = $cfg->{shutdown_check_interval} || 0.05;
    my $tid = $loop->recurring(
      $interval => sub {
        if ($shutdown->get) {
          $daemon->stop;
          $loop->stop;
        }
      }
    );

    $loop->start;
    $loop->remove($tid);
    1;
  };
  my $err = $@;

  close $_ for @open_socks;
  $alive->update(sub { $_[0] - 1 });

  if (!$ok) {
    warn "Mojo::Server::MT worker $idx error: $err\n";
    return "error:$err";
  }
  return "ok:$idx";
}

sub _resolve_app_spec {
  my $self = shift;
  my $app_file  = $self->app_file;
  my $app_class = $self->app_class;
  my $app_mode  = $self->app_mode;

  if (!$app_file && !$app_class && $self->{app}) {
    my $ref = ref $self->{app};
    $app_mode //= $self->{app}->mode if $self->{app}->can('mode');
    if ($ref && $ref ne 'Mojolicious::Lite' && $ref ne 'Mojolicious') {
      $app_class = $ref;
    }
    elsif ($ref eq 'Mojolicious::Lite' && defined $0 && $0 ne '-e' && -f $0) {
      $app_file = $0;
    }
    else {
      $app_class = $ref || 'Mojo::HelloWorld';
    }
  }
  $app_class //= 'Mojo::HelloWorld' unless defined $app_file;
  return ($app_class, $app_file, $app_mode);
}

sub _resolve_listen_urls {
  my $self = shift;
  my $use_shared_fd = ($^O ne 'linux');
  my $multi_worker  = ($self->workers || 1) > 1;

  my (@resolved, @ports, @shared_socks);
  for my $loc (@{$self->listen}) {
    my $url   = Mojo::URL->new($loc);
    my $proto = $url->protocol;
    if ($proto eq 'http' || $proto eq 'https') {
      my $port = $url->port;
      if ($use_shared_fd && !defined $url->query->param('fd')) {
        my $host = $url->host;
        my $addr = (!defined $host || $host eq '*') ? '0.0.0.0' : $host;
        $addr =~ y/[]//d;

        my $sock = IO::Socket::IP->new(
          LocalAddr => $addr,
          ($port ? (LocalPort => $port) : ()),
          Listen    => $self->backlog // SOMAXCONN,
          Type      => SOCK_STREAM,
          ReuseAddr => 1,
        ) or die "Can't create listen socket on $addr:" . ($port || 0) . ": $@\n";
        $sock->blocking(0);
        push @shared_socks, $sock;

        $port = $sock->sockport;
        $url->port($port);
        $url->query->param(fd => fileno($sock));
        $url->query->param(single_accept => 1)
          if $multi_worker && !defined $url->query->param('single_accept');
      }
      elsif (!$port) {
        $port = Mojo::IOLoop::Server->generate_port;
        $url->port($port);
      }
      push @ports,    $port;
      push @resolved, $url->to_string;
    }
    else {
      push @resolved, $loc;
    }
  }
  return (\@resolved, \@ports, \@shared_socks);
}

sub _build_fallback_daemon {
  my $self = shift;
  my $app;
  if (defined $self->app_file) {
    $app = $self->load_app($self->app_file);
  }
  elsif (defined $self->app_class) {
    $app = $self->build_app($self->app_class);
  }
  else {
    $app = $self->app;
  }
  return Mojo::Server::Daemon->new(
    app                => $app,
    backlog            => $self->backlog,
    inactivity_timeout => $self->inactivity_timeout,
    keep_alive_timeout => $self->keep_alive_timeout,
    listen             => $self->listen,
    max_clients        => $self->clients,
    max_requests       => $self->max_requests,
    reverse_proxy      => $self->reverse_proxy,
    silent             => $self->silent,
    trusted_proxies    => $self->trusted_proxies,
  );
}

sub _preload_core_modules {
  # Preload core XS & Mojolicious modules before MT::Image::freeze() so all
  # Tier-1 XS runs in-process without compartment IPC and op trees are shared.
  for my $mod (
    qw(
    Socket IO::Socket::IP IO::Socket::UNIX Time::HiRes Digest::SHA
    MIME::Base64 Encode Scalar::Util List::Util Fcntl POSIX
    Mojo::Date Mojo::Headers Mojo::JSON Mojo::Util Mojo::URL
    Mojo::Path Mojo::Parameters Mojo::Asset::Memory Mojo::Asset::File
    Mojo::Content::Single Mojo::Content::MultiPart
    Mojo::Message::Request Mojo::Message::Response
    Mojo::Transaction::HTTP Mojo::Transaction::WebSocket
    Mojo::IOLoop Mojo::IOLoop::Server Mojo::IOLoop::Stream
    Mojo::Server::Daemon Mojolicious Mojolicious::Lite
    Mojolicious::Static Mojolicious::Renderer Mojolicious::Controller
    Mojo::Template
    )
  ) {
    eval "require $mod";
  }

  # Optional Tier-1 accelerator & native kqueue/epoll reactor
  eval { require Mojo::MT::Accel };
  eval { require Mojo::Reactor::MT };

  # Ensure the detected reactor class is compiled into the boot image
  my $reactor_class = Mojo::Reactor->detect;
  eval "require $reactor_class" if $reactor_class;
}

sub _patch_for_freeze {
  return if $FREEZE_PATCHED;
  $FREEZE_PATCHED = 1;

  my $old_gensym = \&Symbol::gensym;
  my $new_gensym = sub () {
    my $name = 'GEN' . $SYMBOL_GENSEQ++;
    no strict 'refs';
    my $ref = \*{"Symbol::$name"};
    delete $Symbol::{$name};
    $ref;
  };

  # Sub::Util::set_subname mutates CvGV in-place, which traps with SIGBUS on
  # frozen boot-image CVs when Mojo::Template / EPRenderer monkey-patches helpers.
  my $old_set_subname = \&Sub::Util::set_subname;
  my $new_set_subname = sub { return $_[1] };

  no warnings 'redefine';
  no strict 'refs';
  my @queue = ('main::');
  my %seen;
  while (my $stash = shift @queue) {
    next if $seen{$stash}++;
    my $symtab = \%{$stash};
    if (exists $symtab->{gensym}) {
      my $g = $symtab->{gensym};
      if (ref(\$g) eq 'GLOB' && defined *{$g}{CODE} && *{$g}{CODE} == $old_gensym) {
        *{"${stash}gensym"} = $new_gensym;
      }
    }
    if (exists $symtab->{set_subname}) {
      my $g = $symtab->{set_subname};
      if (ref(\$g) eq 'GLOB' && defined *{$g}{CODE} && *{$g}{CODE} == $old_set_subname) {
        *{"${stash}set_subname"} = $new_set_subname;
      }
    }
    for my $k (keys %$symtab) {
      push @queue, "$stash$k" if $k =~ /::$/ && $k ne 'main::';
    }
  }
}

sub _detect_cpus {
  if ($^O eq 'darwin') {
    my $n = `sysctl -n hw.physicalcpu 2>/dev/null`;
    return int($n) if defined $n && $n =~ /^(\d+)/ && $1 > 0;
  }
  elsif (-r '/proc/cpuinfo') {
    if (open my $fh, '<', '/proc/cpuinfo') {
      my $n = grep { /^processor\s*:/ } <$fh>;
      close $fh;
      return $n if $n > 0;
    }
  }
  return 4;
}

1;

=encoding utf8

=head1 NAME

Mojo::Server::MT - Multi-worker HTTP and WebSocket server for perl5-mt

=head1 SYNOPSIS

  use Mojo::Server::MT;

  my $server = Mojo::Server::MT->new(
    app_class => 'MyApp',
    listen    => ['http://127.0.0.1:8080'],
    workers   => 8,
  );
  $server->run;

=head1 DESCRIPTION

L<Mojo::Server::MT> is an experimental single-process, multi-worker HTTP and
WebSocket server for C<perl5-mt>. It preloads core modules, seals the compiled
interpreter read-only with C<MT::Image::freeze()>, and runs one L<Mojo::IOLoop>
per C<MT::Worker> thread.

On Linux, each worker binds its own C<SO_REUSEPORT> listen socket so the kernel
distributes incoming connections across workers. On macOS and other non-Linux
systems, where C<SO_REUSEPORT> does not load-balance TCP accepts across
separate sockets, L<Mojo::Server::MT> binds a single shared listen socket per
URL and shares the file descriptor across workers.

On stock Perl without C<perl5-mt>, L<Mojo::Server::MT> falls back to
L<Mojo::Server::Daemon>.

=head1 ATTRIBUTES

L<Mojo::Server::MT> inherits all attributes from L<Mojo::Server> and implements
the following new ones: C<accepts>, C<app_class>, C<app_file>, C<app_mode>,
C<backlog>, C<clients> (alias C<max_clients>), C<inactivity_timeout>,
C<keep_alive_timeout>, C<listen>, C<max_requests>, C<shared_cache>,
C<shutdown_check_interval>, C<silent>, and C<workers>.

=head1 METHODS

=head2 start

Preloads modules, seals the boot image with C<MT::Image::freeze()>, spawns
worker threads, and waits until all workers are listening.

=head2 run

Starts the server, installs C<SIGINT>/C<SIGTERM> handlers, waits for shutdown,
and joins all workers cleanly.

=head2 stop

Signals all workers to shut down gracefully via the shared C<MT::Cell>.

=head2 join_workers

Joins all spawned C<MT::Worker> threads and returns an arrayref of worker
exit statuses.

=head2 ports

Returns an arrayref of resolved TCP ports the server is listening on.

=head1 SEE ALSO

L<Mojolicious::Command::mt>, L<Mojo::MT::SharedCache>, L<Mojo::Server::Daemon>.

=cut
