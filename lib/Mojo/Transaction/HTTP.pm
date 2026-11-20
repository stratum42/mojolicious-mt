package Mojo::Transaction::HTTP;
use Mojo::Base 'Mojo::Transaction';

use Mojo::Date;
use Mojo::MT qw(HAS_MT_ACCEL);

has 'previous';

# Per-second cached HTTP Date header (our-scoped for perl5-mt worker isolation)
our $DATE_EPOCH = -1;
our $DATE_STR   = '';

sub client_read {
  my ($self, $chunk) = @_;

  # Skip body for HEAD request
  my $res = $self->res;
  $res->content->skip_body(1) if uc $self->req->method eq 'HEAD';
  return undef unless $res->parse($chunk)->is_finished;

  # Unexpected 1xx response
  return $self->completed if !$res->is_info || $res->headers->upgrade;
  $self->res($res->new)->emit(unexpected => $res);
  return undef unless length(my $leftovers = $res->content->leftovers);
  $self->client_read($leftovers);
}

sub client_write { shift->_write(0) }

sub is_empty { !!(uc $_[0]->req->method eq 'HEAD' || $_[0]->res->is_empty) }

sub keep_alive {
  my $self = shift;

  # Close
  my $req      = $self->req;
  my $res      = $self->res;
  my $req_conn = $req->headers->connection;
  my $res_conn = $res->headers->connection;
  if (defined $req_conn) {
    $req_conn = lc $req_conn;
    return undef if $req_conn eq 'close';
  }
  if (defined $res_conn) {
    $res_conn = lc $res_conn;
    return undef if $res_conn eq 'close';
  }

  # Keep-alive is optional for 1.0
  return ($res_conn // '') eq 'keep-alive' if $res->version eq '1.0';
  return ($req_conn // '') eq 'keep-alive' if $req->version eq '1.0';

  # Keep-alive is the default for 1.1
  return 1;
}

sub redirects {
  my $previous = shift;
  my @redirects;
  unshift @redirects, $previous while $previous = $previous->previous;
  return \@redirects;
}

sub resume { ++$_[0]{writing} and return $_[0]->emit('resume') }

sub server_read {
  my ($self, $chunk) = @_;

  # Parse request
  my $req = $self->req;
  $req->parse($chunk) unless $req->error;

  # Generate response
  $self->emit('request') if $req->is_finished && !$self->{handled}++;
}

sub server_write {
  my $self = shift;
  return '' unless $self->{writing};

  # Single-shot C/XS HTTP/1.1 response serializer fast path
  if (HAS_MT_ACCEL && !$self->{http_state}) {
    my $res     = $self->res;
    my $content = $res->{content};
    if ($content && ref($content) eq 'Mojo::Content::Single'
     && !$content->{dynamic} && !$res->{fix} && !defined($res->{start_buffer})
     && !($res->{events} && $res->{events}{progress}))
    {
      my $code = $res->{code} || 404;
      my $req  = $self->{req};
      my $meth = $req && $req->{method};
      if ($code >= 200 && $code != 204 && $code != 304 && (!defined $meth || uc($meth) ne 'HEAD')) {
        my $headers = $content->{headers};
        my $hdrs_hv = $headers && $headers->{headers};
        if (!$hdrs_hv || !$hdrs_hv->{'transfer-encoding'}) {
          my $asset = $content->{asset};
          if (!$asset || (ref($asset) eq 'Mojo::Asset::Memory'
           && !defined($asset->{start_range}) && !defined($asset->{end_range})
           && length($asset->{content} // '') <= 131072))
          {
            my $body = $asset ? ($asset->{content} // '') : '';
            $headers ||= $content->headers;
            $hdrs_hv = $headers->{headers} ||= {};
            my $date_str;
            if (!$hdrs_hv->{date}) {
              my $now = int time;
              if ($now != $DATE_EPOCH) {
                $DATE_EPOCH = $now;
                $DATE_STR   = Mojo::Date->new($now)->to_string;
              }
              $date_str = $DATE_STR;
              $hdrs_hv->{date} = [$date_str];
            }
            $hdrs_hv->{'content-length'} = [length $body] unless $hdrs_hv->{'content-length'};

            my $wire = Mojo::MT::Accel::build_http_response(
              $code, $res->{message}, $hdrs_hv, $body, undef, 1, $headers->{names}, $res->{version}
            );
            if (defined $wire) {
              $res->{fix}   = 1;
              $res->{state} = 'finished';
              $res->emit('finish') if !$res->{finished}++ && $res->{events} && $res->{events}{finish};
              @$self{qw(http_state offset write)} = ('body', length($body), 0);
              $self->completed;
              return $wire;
            }
          }
        }
      }
    }
  }

  return $self->_write(1);
}

sub _body {
  my ($self, $msg, $finish) = @_;

  # Prepare body chunk
  my $buffer = $msg->get_body_chunk($self->{offset});
  $self->{offset} += defined $buffer ? length $buffer : 0;

  # Delayed
  $self->{writing} = 0 unless defined $buffer;

  # Finished (static content does not need an extra round trip for the empty chunk)
  if (defined $buffer && (!length $buffer || (!$msg->content->is_dynamic && $self->{offset} >= $msg->body_size))) {
    $msg->finish;
    $finish ? $self->completed : ($self->{writing} = 0);
  }

  return $buffer // '';
}

sub _headers {
  my ($self, $msg, $head) = @_;

  # Prepare header chunk
  my $buffer  = $msg->get_header_chunk($self->{offset});
  my $written = defined $buffer ? length $buffer : 0;
  $self->{write}  -= $written;
  $self->{offset} += $written;

  # Switch to body
  if ($self->{write} <= 0) {
    @$self{qw(http_state offset)} = ('body', 0);

    # Response without body
    $self->completed->{http_state} = 'empty' if $head && $self->is_empty;
  }

  return $buffer;
}

sub _start_line {
  my ($self, $msg) = @_;

  # Prepare start-line chunk
  my $buffer  = $msg->get_start_line_chunk($self->{offset});
  my $written = defined $buffer ? length $buffer : 0;
  $self->{write}  -= $written;
  $self->{offset} += $written;

  # Switch to headers
  @$self{qw(http_state write offset)} = ('headers', $msg->header_size, 0) if $self->{write} <= 0;

  return $buffer;
}

sub _write {
  my ($self, $server) = @_;

  # Client starts writing right away
  return '' unless $server ? $self->{writing} : ($self->{writing} //= 1);

  # Nothing written yet
  $self->{offset} ||= 0;
  $self->{write}  ||= 0;
  my $msg = $server ? $self->res : $self->req;
  if (!$self->{http_state}) {
    if ($server) {
      my $headers = $msg->headers;
      if (!$headers->date) {
        my $now = int time;
        if ($now != $DATE_EPOCH) {
          $DATE_EPOCH = $now;
          $DATE_STR   = Mojo::Date->new($now)->to_string;
        }
        $headers->date($DATE_STR);
      }
    }
    @$self{qw(http_state write)} = ('start_line', $msg->start_line_size);
  }

  # Start-line
  my $chunk = '';
  $chunk .= $self->_start_line($msg) if $self->{http_state} eq 'start_line';

  # Headers
  $chunk .= $self->_headers($msg, $server) if $self->{http_state} eq 'headers';

  # Body
  $chunk .= $self->_body($msg, $server) if $self->{http_state} eq 'body';

  return $chunk;
}

1;

=encoding utf8

=head1 NAME

Mojo::Transaction::HTTP - HTTP transaction

=head1 SYNOPSIS

  use Mojo::Transaction::HTTP;

  # Client
  my $tx = Mojo::Transaction::HTTP->new;
  $tx->req->method('GET');
  $tx->req->url->parse('http://example.com');
  $tx->req->headers->accept('application/json');
  say $tx->res->code;
  say $tx->res->headers->content_type;
  say $tx->res->body;
  say $tx->remote_address;

  # Server
  my $tx = Mojo::Transaction::HTTP->new;
  say $tx->req->method;
  say $tx->req->url->to_abs;
  say $tx->req->headers->accept;
  say $tx->remote_address;
  $tx->res->code(200);
  $tx->res->headers->content_type('text/plain');
  $tx->res->body('Hello World!');

=head1 DESCRIPTION

L<Mojo::Transaction::HTTP> is a container for HTTP transactions, based on L<RFC
7230|https://tools.ietf.org/html/rfc7230> and L<RFC 7231|https://tools.ietf.org/html/rfc7231>.

=head1 EVENTS

L<Mojo::Transaction::HTTP> inherits all events from L<Mojo::Transaction> and can emit the following new ones.

=head2 request

  $tx->on(request => sub ($tx) {...});

Emitted when a request is ready and needs to be handled.

  $tx->on(request => sub ($tx) { $tx->res->headers->header('X-Bender' => 'Bite my shiny metal ass!') });

=head2 resume

  $tx->on(resume => sub ($tx) {...});

Emitted when transaction is resumed.

=head2 unexpected

  $tx->on(unexpected => sub ($tx, $res) {...});

Emitted for unexpected C<1xx> responses that will be ignored.

  $tx->on(unexpected => sub ($tx) { $tx->res->on(finish => sub { say 'Follow-up response is finished.' }) });

=head1 ATTRIBUTES

L<Mojo::Transaction::HTTP> inherits all attributes from L<Mojo::Transaction> and implements the following new ones.

=head2 previous

  my $previous = $tx->previous;
  $tx          = $tx->previous(Mojo::Transaction::HTTP->new);

Previous transaction that triggered this follow-up transaction, usually a L<Mojo::Transaction::HTTP> object.

  # Paths of previous requests
  say $tx->previous->previous->req->url->path;
  say $tx->previous->req->url->path;

=head1 METHODS

L<Mojo::Transaction::HTTP> inherits all methods from L<Mojo::Transaction> and implements the following new ones.

=head2 client_read

  $tx->client_read($bytes);

Read data client-side, used to implement user agents such as L<Mojo::UserAgent>.

=head2 client_write

  my $bytes = $tx->client_write;

Write data client-side, used to implement user agents such as L<Mojo::UserAgent>.

=head2 is_empty

  my $bool = $tx->is_empty;

Check transaction for C<HEAD> request and C<1xx>, C<204> or C<304> response.

=head2 keep_alive

  my $bool = $tx->keep_alive;

Check if connection can be kept alive.

=head2 redirects

  my $redirects = $tx->redirects;

Return an array reference with all previous transactions that preceded this follow-up transaction.

  # Paths of all previous requests
  say $_->req->url->path for @{$tx->redirects};

=head2 resume

  $tx = $tx->resume;

Resume transaction.

=head2 server_read

  $tx->server_read($bytes);

Read data server-side, used to implement web servers such as L<Mojo::Server::Daemon>.

=head2 server_write

  my $bytes = $tx->server_write;

Write data server-side, used to implement web servers such as L<Mojo::Server::Daemon>.

=head1 SEE ALSO

L<Mojolicious>, L<Mojolicious::Guides>, L<https://mojolicious.org>.

=cut
