use strict;
use warnings;
use Config;
use File::Basename qw(dirname);
use File::Spec;
use ExtUtils::MakeMaker;

my @extra;
if (!-d "$Config{archlibexp}/CORE") {
  my ($perl_dir) = grep { defined $_ && -f File::Spec->catfile($_, 'perl.h') }
    ($ENV{PERL5_MT_SRC}, dirname($^X), File::Spec->rel2abs('../../perl5-mt'));
  if ($perl_dir) {
    my $lib_dir = File::Spec->catdir($perl_dir, 'lib');
    push @extra,
      PERL_SRC => $perl_dir,
      PERL     => qq{"$^X" "-I$lib_dir"},
      FULLPERL => qq{"$^X" "-I$lib_dir"};
  }
}

WriteMakefile(
  NAME          => 'Mojo::MT::Accel',
  VERSION_FROM  => 'lib/Mojo/MT/Accel.pm',
  ABSTRACT_FROM => 'lib/Mojo/MT/Accel.pm',
  DEFINE        => '-DPERL_XS_THREADSAFE=1',
  OPTIMIZE      => '-O3',
  INC           => '-I.',
  OBJECT        => 'Accel$(OBJ_EXT)',
  depend        => {
    'Accel.c' => 'mt_reactor.h mt_http.h mt_json.h mt_util.h',
  },
  @extra,
);
