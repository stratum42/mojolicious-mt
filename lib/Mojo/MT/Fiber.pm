package Mojo::MT::Fiber;
use Mojo::Base -strict;

use Carp     qw(croak);
use Exporter qw(import);
use Mojo::MT qw(HAS_MT_ASYNC_IO HAS_MT_FIBER);

BEGIN {
  require MT::Fiber if HAS_MT_FIBER;
  require MT::IO    if HAS_MT_ASYNC_IO;
}

our @EXPORT_OK = qw(run_fibers async_slurp async_slurp_many yield pass is_in_fiber);
our %EXPORT_TAGS = (all => \@EXPORT_OK);

sub run_fibers {
  shift if @_ && !ref $_[0] && $_[0] eq __PACKAGE__;
  my @subs = @_;
  return wantarray ? () : [] unless @subs;

  if (HAS_MT_FIBER) {
    my @ids = map { MT::Fiber::schedule($_) } @subs;
    MT::Fiber::run();
    my @results = map { MT::Fiber::join($_) } @ids;
    return wantarray ? @results : \@results;
  }

  my @results = map { $_->() } @subs;
  return wantarray ? @results : \@results;
}

sub async_slurp {
  shift if @_ > 1 && !ref $_[0] && $_[0] eq __PACKAGE__;
  my ($path) = @_;
  croak 'File path is required' unless defined $path;

  my $layer = HAS_MT_ASYNC_IO ? '<:async' : '<:raw';
  open my $fh, $layer, $path or croak qq{Can't open file "$path": $!};

  my $content = '';
  my $ret;
  while ($ret = read($fh, my $buf, 65536)) {
    $content .= $buf;
  }
  my $err = defined $ret ? undef : $!;
  close $fh;
  croak qq{Can't read from file "$path": $err} if defined $err;

  return $content;
}

sub async_slurp_many {
  shift if @_ && !ref $_[0] && $_[0] eq __PACKAGE__;
  my @paths = @_;
  return wantarray ? () : [] unless @paths;

  my @subs = map {
    my $p = $_;
    sub { async_slurp($p) }
  } @paths;
  return run_fibers(@subs);
}

sub yield {
  shift if @_ && !ref $_[0] && $_[0] eq __PACKAGE__;
  return HAS_MT_FIBER ? MT::Fiber::yield(@_) : undef;
}

sub pass {
  shift if @_ && !ref $_[0] && $_[0] eq __PACKAGE__;
  return HAS_MT_FIBER ? MT::Fiber::pass() : undef;
}

sub is_in_fiber {
  return HAS_MT_FIBER ? (defined MT::Fiber::current() ? 1 : 0) : 0;
}

1;

=encoding utf8

=head1 NAME

Mojo::MT::Fiber - Cooperative stackful fibers and :async I/O helpers for Mojolicious

=head1 SYNOPSIS

  use Mojo::MT::Fiber qw(run_fibers async_slurp async_slurp_many);

  my @results = run_fibers(
    sub { async_slurp('/path/to/a.txt') },
    sub { async_slurp('/path/to/b.txt') },
  );

  my @files = async_slurp_many('/path/to/a.txt', '/path/to/b.txt');

=head1 DESCRIPTION

L<Mojo::MT::Fiber> provides small convenience helpers over C<perl5-mt>'s
cooperative stackful L<MT::Fiber> scheduler and C<:async> PerlIO layer. Inside
scheduled fibers, C<:async> reads park only the calling fiber while other
fibers run. Behavior varies by platform (for example, Linux uses C<io_uring>
while macOS/BSD fall back to non-Linux async I/O paths). On stock Perl, all
helpers fall back to plain synchronous execution.

=head1 FUNCTIONS

=head2 run_fibers

  my @results = run_fibers(\&sub1, \&sub2, ...);
  my $results = run_fibers(\&sub1, \&sub2, ...);

Schedule all coderefs onto the L<MT::Fiber> cooperative scheduler, run them to
completion, and return their joined results. Falls back to sequential execution
on stock Perl.

=head2 async_slurp

  my $bytes = async_slurp($path);

Read the entire contents of C<$path> using the C<:async> PerlIO layer under
C<perl5-mt> (or C<:raw> on stock Perl).

=head2 async_slurp_many

  my @contents = async_slurp_many(@paths);

Concurrently read multiple files via L</"run_fibers"> and L</"async_slurp">.

=head2 yield

  Mojo::MT::Fiber::yield(@args);

Yield from the current fiber under C<perl5-mt> (no-op on stock Perl).

=head2 pass

  Mojo::MT::Fiber::pass();

Yield to the next ready scheduled fiber under C<perl5-mt> (no-op on stock Perl).

=head2 is_in_fiber

  my $bool = Mojo::MT::Fiber::is_in_fiber();

Return true if currently executing inside a L<MT::Fiber>.

=head1 SEE ALSO

L<Mojo::MT>, L<Mojo::Server::MT>.

=cut
