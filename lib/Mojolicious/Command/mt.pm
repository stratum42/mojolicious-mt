package Mojolicious::Command::mt;
use Mojo::Base 'Mojolicious::Command';

use Mojo::Server::MT;
use Mojo::Util qw(getopt);

has description => 'Start application with native perl5-mt multi-worker HTTP and WebSocket server';
has usage       => sub { shift->extract_usage };

sub build_server {
  my ($self, @args) = @_;

  my $mt = Mojo::Server::MT->new(app => $self->app);
  die $self->usage
    unless getopt \@args,
    'a|accepts=i'            => sub { $mt->accepts($_[1]) },
    'b|backlog=i'            => sub { $mt->backlog($_[1]) },
    'c|clients=i'            => sub { $mt->clients($_[1]) },
    'i|inactivity-timeout=i' => sub { $mt->inactivity_timeout($_[1]) },
    'k|keep-alive-timeout=i' => sub { $mt->keep_alive_timeout($_[1]) },
    'l|listen=s'             => \my @listen,
    'p|proxy:s'              => \my @proxy,
    'r|requests=i'           => sub { $mt->max_requests($_[1]) },
    's|silent'               => sub { $mt->silent(1) },
    'w|workers=i'            => sub { $mt->workers($_[1]) };

  $mt->listen(\@listen) if @listen;
  $mt->reverse_proxy(1) if @proxy;
  my @trusted = grep {length} @proxy;
  $mt->trusted_proxies(\@trusted) if @trusted;
  return $mt;
}

sub run { shift->build_server(@_)->run }

1;

=encoding utf8

=head1 NAME

Mojolicious::Command::mt - Native perl5-mt multi-worker server command

=head1 SYNOPSIS

  Usage: APPLICATION mt [OPTIONS]

    ./myapp.pl mt
    ./myapp.pl mt -w 8 -l http://127.0.0.1:8080
    ./myapp.pl mt -m production -w 12 -l http://*:3000 -r 10000

  Options:
    -a, --accepts <number>               Number of connections for workers to
                                         accept, defaults to 0 (unlimited)
    -b, --backlog <size>                 Listen backlog size, defaults to
                                         SOMAXCONN
    -c, --clients <number>               Maximum number of concurrent
                                         connections per worker
    -h, --help                           Show this summary of available options
        --home <path>                    Path to home directory of your
                                         application, defaults to the value of
                                         MOJO_HOME or auto-detection
    -i, --inactivity-timeout <seconds>   Inactivity timeout, defaults to the
                                         value of MOJO_INACTIVITY_TIMEOUT or 30
    -k, --keep-alive-timeout <seconds>   Keep-alive timeout, defaults to the
                                         value of MOJO_KEEP_ALIVE_TIMEOUT or 5
    -l, --listen <location>              One or more locations you want to
                                         listen on, defaults to the value of
                                         MOJO_LISTEN or "http://*:3000"
    -m, --mode <name>                    Operating mode for your application,
                                         defaults to the value of
                                         MOJO_MODE/PLACK_ENV or "development"
    -p, --proxy [<network>]              Activate reverse proxy support,
                                         defaults to the value of
                                         MOJO_REVERSE_PROXY, optionally takes
                                         one or more trusted proxy addresses or
                                         networks
    -r, --requests <number>              Maximum number of requests per
                                         keep-alive connection, defaults to 100
    -s, --silent                         Disable console messages
    -w, --workers <number>               Number of MT worker threads, defaults
                                         to physical CPU cores

=head1 DESCRIPTION

L<Mojolicious::Command::mt> starts applications with the L<Mojo::Server::MT>
multi-worker backend on C<perl5-mt> (falling back to L<Mojo::Server::Daemon>
on stock Perl).

=head1 METHODS

=head2 build_server

  my $server = $mt->build_server(@ARGV);

Build L<Mojo::Server::MT> instance from command line arguments.

=head2 run

  $mt->run(@ARGV);

Run this command.

=head1 SEE ALSO

L<Mojo::Server::MT>, L<Mojolicious>, L<Mojolicious::Guides>.

=cut
