use Mojo::Base -strict;

use Test::More;
use File::Temp qw(tempdir);
use Mojo::Asset::File;
use Mojo::File qw(path);
use Mojo::MT   qw(HAS_MT HAS_MT_ASYNC_IO HAS_MT_FIBER);
use Mojo::MT::Fiber qw(async_slurp async_slurp_many is_in_fiber run_fibers);
use Mojolicious;
use Mojolicious::Static;
use Test::Mojo;

my $dir = tempdir(CLEANUP => 1);
my @files;
for my $i (1 .. 4) {
  my $p = path($dir, "sample_$i.txt");
  $p->spew("file-$i:" . ('x' x (4096 * $i)));
  push @files, $p->to_string;
}

# 1. Mojo::MT::Fiber run_fibers + async_slurp + async_slurp_many
subtest 'Mojo::MT::Fiber run_fibers and async_slurp' => sub {
  my @out = run_fibers(
    sub { Mojo::MT::Fiber::pass(); 'a:' . length(async_slurp($files[0])) },
    sub { Mojo::MT::Fiber::pass(); 'b:' . length(async_slurp($files[1])) },
    sub { Mojo::MT::Fiber::pass(); 'c:' . length(async_slurp($files[2])) },
    sub { Mojo::MT::Fiber::pass(); 'd:' . length(async_slurp($files[3])) },
  );
  is_deeply \@out, [
    'a:' . (7 + 4096),
    'b:' . (7 + 8192),
    'c:' . (7 + 12288),
    'd:' . (7 + 16384),
  ], 'run_fibers + async_slurp returned exact byte lengths';

  my @contents = async_slurp_many(@files);
  is scalar(@contents), 4, 'async_slurp_many returned 4 files';
  like $contents[0], qr/^file-1:x+$/, 'first file content matches';
  like $contents[3], qr/^file-4:x+$/, 'fourth file content matches';

  my ($inside) = run_fibers(sub { is_in_fiber() });
  is $inside, (HAS_MT_FIBER ? 1 : 0), 'is_in_fiber matches HAS_MT_FIBER';
};

# 2. Mojo::Asset::File :async fast path (mtime, size, slurp, get_chunk, range)
subtest 'Mojo::Asset::File :async read and stat fast path' => sub {
  my $asset = Mojo::Asset::File->new(path => $files[0]);
  is $asset->size, 7 + 4096, 'size matches via MT::IO::stat / stat';
  ok $asset->mtime > 0, 'mtime positive';
  is length($asset->slurp), 7 + 4096, 'slurp reads full file';

  # Range get_chunk
  my $ranged = Mojo::Asset::File->new(path => $files[0], start_range => 0, end_range => 6);
  is $ranged->get_chunk(0, 100), 'file-1:', 'get_chunk respects start_range/end_range';
  is $ranged->get_chunk(7, 100), '', 'get_chunk past end_range returns empty string';
};

# 3. Mojolicious::Static serving static files via :async + MT::IO::stat
subtest 'Mojolicious::Static integration' => sub {
  my $app = Mojolicious->new;
  $app->static->paths([$dir]);
  my $t = Test::Mojo->new($app);

  $t->get_ok('/sample_2.txt')
    ->status_is(200)
    ->header_is('Accept-Ranges' => 'bytes')
    ->content_is('file-2:' . ('x' x 8192));

  $t->get_ok('/sample_2.txt' => {Range => 'bytes=0-6'})
    ->status_is(206)
    ->header_is('Content-Range' => 'bytes 0-6/8199')
    ->content_is('file-2:');

  $t->get_ok('/does_not_exist.txt')->status_is(404);
};

done_testing();
