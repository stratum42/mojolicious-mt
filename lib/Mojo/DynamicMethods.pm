package Mojo::DynamicMethods;
use Mojo::Base -strict;

use Hash::Util::FieldHash qw(fieldhash id_2obj);
use Mojo::Util            qw(monkey_patch);

our (%DYN_METHODS, %PLAIN_METHODS, %INSTALLED, $FROZEN_MODE);
my $SETUP;

sub import {
  my ($flag, $caller) = ($_[1] // '', caller);
  return unless $flag eq '-dispatch';

  my $dyn_pkg    = "${caller}::_Dynamic";
  my $caller_can = $caller->can('SUPER::can');
  monkey_patch $dyn_pkg, 'can', sub {
    my ($self, $method, @rest) = @_;

    # Delegate to our parent's "can" if there is one, without breaking if not
    my $can = $self->$caller_can($method, @rest);
    return undef unless $can;
    no warnings 'once';
    my $h = do { no strict 'refs'; *{"${dyn_pkg}::${method}"}{CODE} };
    return $h && $h eq $can ? undef : $can;
  };

  {
    no strict 'refs';
    unshift @{"${caller}::ISA"}, $dyn_pkg;
  }
}

sub prepare_for_freeze {
  return if $FROZEN_MODE;
  $FROZEN_MODE = 1;

  # Drain fieldhash cleanly (removing all weakref backrefs) into a plain worker-safe hash
  for my $id (keys %DYN_METHODS) {
    my $methods = delete $DYN_METHODS{$id};
    my $obj     = id_2obj($id);
    $PLAIN_METHODS{$obj} = $methods if defined $obj;
  }
  if (defined &Hash::Util::FieldHash::_ob_reg) {
    %{Hash::Util::FieldHash::_ob_reg()} = ();
  }

  # Rebind installed dynamic dispatchers to the plain hash
  for my $target (keys %INSTALLED) {
    my $dyn_pkg = "${target}::_Dynamic";
    for my $name (keys %{$INSTALLED{$target}}) {
      monkey_patch($dyn_pkg, $name, $target->BUILD_DYNAMIC($name, \%PLAIN_METHODS));
    }
  }
}

sub register {
  my ($target, $object, $name, $code) = @_;

  if ($FROZEN_MODE) {
    my $dyn_pkg = "${target}::_Dynamic";
    monkey_patch($dyn_pkg, $name, $target->BUILD_DYNAMIC($name, \%PLAIN_METHODS))
      unless do { no strict 'refs'; *{"${dyn_pkg}::${name}"}{CODE} };
    $PLAIN_METHODS{$object}{$name} = $code;
    $INSTALLED{$target}{$name}     = 1;
    return;
  }

  $SETUP ||= do { fieldhash %DYN_METHODS; 1 };

  my $dyn_pkg = "${target}::_Dynamic";
  monkey_patch($dyn_pkg, $name, $target->BUILD_DYNAMIC($name, \%DYN_METHODS))
    unless do { no strict 'refs'; *{"${dyn_pkg}::${name}"}{CODE} };
  $DYN_METHODS{$object}{$name} = $code;
  $INSTALLED{$target}{$name}   = 1;
}

"Ph'nglui mglw'nafh Cthulhu R'lyeh wgah'nagl fhtagn";

=encoding utf8

=head1 NAME

Mojo::DynamicMethods - Fast dynamic method dispatch

=head1 SYNOPSIS

  package MyClass;
  use Mojo::Base -base, -signatures;

  use Mojo::DynamicMethods -dispatch;

  sub BUILD_DYNAMIC ($class, $method, $dyn_methods) {
    return sub {...};
  }

  sub add_helper ($self, $name, $cb) {
    Mojo::DynamicMethods::register 'MyClass', $self, $name, $cb;
  }

  package main;

  # Generate methods dynamically (and hide them from "$obj->can(...)")
  my $obj = MyClass->new;
  $obj->add_helper(foo => sub { warn 'Hello Helper!' });
  $obj->foo;

=head1 DESCRIPTION

L<Mojo::DynamicMethods> provides dynamic method dispatch for per-object helper methods without requiring use of
C<AUTOLOAD>.

To opt your class into dynamic dispatch simply pass the C<-dispatch> flag.

  use Mojo::DynamicMethods -dispatch;

And then implement a C<BUILD_DYNAMIC> method in your class, making sure that the key you use to lookup methods in
C<$dyn_methods> is the same thing you pass as C<$ref> to L</"register">.

  sub BUILD_DYNAMIC ($class, $method, $dyn_methods) {
    return sub ($self, @args) {
      my $dynamic = $dyn_methods->{$self}{$method};
      return $self->$dynamic(@args) if $dynamic;
      my $package = ref $self;
      croak qq{Can't locate object method "$method" via package "$package"};
    };
  }

Note that this module will summon B<Cthulhu>, use it at your own risk!

=head1 FUNCTIONS

L<Mojo::DynamicMethods> implements the following functions.

=head2 register

  Mojo::DynamicMethods::register $class, $ref, $name, $cb;

Registers the method C<$name> as eligible for dynamic dispatch for C<$class>, and sets C<$cb> to be looked up for
C<$name> by reference C<$ref> in a dynamic method constructed by C<BUILD_DYNAMIC>.

=head1 SEE ALSO

L<Mojolicious>, L<Mojolicious::Guides>, L<https://mojolicious.org>.

=cut
