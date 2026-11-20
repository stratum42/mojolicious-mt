package Mojo::MT::SharedCache;
use Mojo::Base -strict;

use Mojo::MT qw(HAS_MT_SHARED_HASH);

BEGIN {
  require MT::SharedHash if HAS_MT_SHARED_HASH;
}

# Fallback process-local store for stock Perl (and worker-isolated if ever used without MT::SharedHash)
our $NEXT_ID = 1;
our %LOCAL_STORE;

sub new {
  my ($class, %args) = @_;
  return $class->attach($args{id}) if defined $args{id};

  if (HAS_MT_SHARED_HASH) {
    my $sh = MT::SharedHash->new;
    return bless {_id => $sh->id, _sh => $sh}, $class;
  }

  my $id = $NEXT_ID++;
  $LOCAL_STORE{$id} = {};
  return bless {_id => $id, _data => $LOCAL_STORE{$id}}, $class;
}

sub attach {
  my ($class, $id) = @_;
  if (HAS_MT_SHARED_HASH) {
    my $sh = MT::SharedHash->attach($id);
    return bless {_id => $id, _sh => $sh}, $class;
  }

  $LOCAL_STORE{$id} //= {};
  return bless {_id => $id, _data => $LOCAL_STORE{$id}}, $class;
}

sub id { $_[0]{_id} }

sub get {
  my ($self, $key) = @_;
  return HAS_MT_SHARED_HASH ? $self->{_sh}->get($key) : $self->{_data}{$key};
}

sub set {
  my ($self, $key, $val) = @_;
  return HAS_MT_SHARED_HASH ? $self->{_sh}->set($key, $val) : ($self->{_data}{$key} = $val);
}

sub delete {
  my ($self, $key) = @_;
  return HAS_MT_SHARED_HASH ? $self->{_sh}->delete($key) : (defined CORE::delete($self->{_data}{$key}) ? 1 : 0);
}

sub exists {
  my ($self, $key) = @_;
  return HAS_MT_SHARED_HASH ? $self->{_sh}->exists($key) : (CORE::exists($self->{_data}{$key}) ? 1 : 0);
}

sub keys {
  my $self = shift;
  return HAS_MT_SHARED_HASH ? $self->{_sh}->keys : CORE::keys(%{$self->{_data}});
}

sub get_or_set {
  my ($self, $key, $cb) = @_;
  if (HAS_MT_SHARED_HASH) {
    my $sh = $self->{_sh};
    return $sh->get($key) if $sh->exists($key);
    my $val = $cb->($key);
    return $sh->set($key, $val);
  }
  my $data = $self->{_data};
  return $data->{$key} if CORE::exists $data->{$key};
  return $data->{$key} = $cb->($key);
}

1;

=encoding utf8

=head1 NAME

Mojo::MT::SharedCache - Cross-worker shared key-value cache backed by MT::SharedHash

=head1 SYNOPSIS

  use Mojo::MT::SharedCache;

  # Create before MT::Image::freeze()
  my $cache = Mojo::MT::SharedCache->new;
  my $id    = $cache->id;

  # Attach inside an MT::Worker thread
  my $wcache = Mojo::MT::SharedCache->attach($id);
  $wcache->set(greeting => 'hello');
  my $val = $wcache->get_or_set(answer => sub { 42 });

=head1 DESCRIPTION

L<Mojo::MT::SharedCache> wraps C<perl5-mt>'s sharded concurrent L<MT::SharedHash>
for sharing scalar values across L<Mojo::Server::MT> worker threads without
serialization. Create the cache before C<MT::Image::freeze()> and attach by
integer C<id> inside each worker. On stock Perl, it falls back to a
process-local Perl hash.

=head1 METHODS

=head2 new

  my $cache = Mojo::MT::SharedCache->new;
  my $cache = Mojo::MT::SharedCache->new(id => $id);

Construct a new shared cache or attach to an existing cache by integer C<id>.

=head2 attach

  my $cache = Mojo::MT::SharedCache->attach($id);

Attach to an existing shared cache by its integer C<id>.

=head2 id

  my $id = $cache->id;

Return the integer identifier for this shared cache.

=head2 get

  my $val = $cache->get($key);

Retrieve the scalar value stored under C<$key>.

=head2 set

  my $val = $cache->set($key, $val);

Store a scalar value under C<$key> and return it.

=head2 delete

  my $bool = $cache->delete($key);

Delete C<$key> from the cache.

=head2 exists

  my $bool = $cache->exists($key);

Return true if C<$key> exists in the cache.

=head2 keys

  my @keys = $cache->keys;

Return a snapshot list of keys currently in the cache.

=head2 get_or_set

  my $val = $cache->get_or_set($key, sub { ... });

Return the cached value for C<$key> if present, otherwise invoke the callback,
store the returned scalar value, and return it.

=head1 SEE ALSO

L<Mojo::MT>, L<Mojo::Server::MT>.

=cut
