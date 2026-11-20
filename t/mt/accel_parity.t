use Mojo::Base -strict;

use Test::More;
use Mojo::MT qw(HAS_MT HAS_MT_ACCEL);

plan skip_all => 'Mojo::MT::Accel not available' unless HAS_MT_ACCEL;

use MIME::Base64 qw(decode_base64 encode_base64);
use Mojo::ByteStream qw(b);
use Mojo::Headers;
use Mojo::JSON qw(decode_json encode_json false from_json j to_json true);
use Mojo::Message::Request;
use Mojo::Util qw(b64_decode b64_encode decode encode url_escape url_unescape xml_escape);
use Scalar::Util qw(dualvar);

# Pure-Perl reference helpers
my %REF_XML = ('&' => '&amp;', '<' => '&lt;', '>' => '&gt;', '"' => '&quot;', '\'' => '&#39;');
sub ref_xml_escape {
  return $_[0] if ref $_[0] && ref $_[0] eq 'Mojo::ByteStream';
  my $str = shift // '';
  $str =~ s/([&<>"'])/$REF_XML{$1}/ge;
  return $str;
}
sub ref_url_escape {
  my $str = shift;
  $str =~ s/([^A-Za-z0-9\-._~])/sprintf '%%%02X', ord $1/ge;
  return $str;
}
sub ref_url_unescape {
  my $str = shift;
  $str =~ s/%([0-9a-fA-F]{2})/chr hex $1/ge;
  return $str;
}

package ParityTOJSON;
use Mojo::Base -base;
has 'data';
sub TO_JSON { shift->data }

package ParityOverload;
use Mojo::Base -base;
use overload '""' => sub { 'overloaded-str' }, fallback => 1;

package main;

subtest 'xml_escape parity' => sub {
  my @cases = (
    '',
    'plain ascii string without any special characters at all 0123456789',
    'a' x 128,
    '&<>"\'',
    ('x' x 33) . '<script>alert("xss" & \'1\')</script>' . ('y' x 33),
    "привет мир <tag attr=\"val\" & 'single'>",
    "привет мир без спецсимволов " x 5,
    undef,
  );
  for my $i (0 .. $#cases) {
    my $c = $cases[$i];
    is xml_escape($c), ref_xml_escape($c), "xml_escape parity case $i";
  }
  my $bs = b('<p>safe</p>');
  is xml_escape($bs), '<p>safe</p>', 'xml_escape preserves Mojo::ByteStream';
};

subtest 'url_escape and url_unescape parity' => sub {
  my @cases = (
    '',
    'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~',
    ('a' x 64) . ' hello/world?a=1&b=2#frag',
    join('', map { chr $_ } 0 .. 255),
    encode('UTF-8', "foo\x{df}\x{0100}bar\x{263a}"),
    "wide\x{263a}char",
  );
  for my $c (@cases) {
    my $got = url_escape($c);
    my $exp = ref_url_escape($c);
    is $got, $exp, 'url_escape parity (len=' . length($c) . ')';
    is url_unescape($got), ref_url_unescape($got), 'url_unescape roundtrip parity';
  }
  is url_escape('&business;23', 's&'), '%26bu%73ine%73%73;23', 'url_escape custom pattern';
  is url_unescape('100%ZZ%2%20%2f%2F'), ref_url_unescape('100%ZZ%2%20%2f%2F'), 'url_unescape invalid/mixed hex';
  is url_unescape("utf8-\x{263a}-%C3%9F-%E9"), ref_url_unescape("utf8-\x{263a}-%C3%9F-%E9"), 'url_unescape UTF-8 string';
};

subtest 'b64_encode and b64_decode parity' => sub {
  for my $len (0, 1, 2, 3, 4, 56, 57, 58, 100, 256, 1000) {
    my $bytes = join('', map { chr($_ % 256) } 0 .. $len - 1);
    is b64_encode($bytes),          encode_base64($bytes),          "b64_encode default eol (len=$len)";
    is b64_encode($bytes, ''),      encode_base64($bytes, ''),      "b64_encode empty eol (len=$len)";
    is b64_encode($bytes, "\r\n"),  encode_base64($bytes, "\r\n"),  "b64_encode CRLF eol (len=$len)";
    my $enc = b64_encode($bytes);
    is b64_decode($enc),            decode_base64($enc),            "b64_decode parity (len=$len)";
    is b64_decode($enc),            $bytes,                         "b64 roundtrip (len=$len)";
  }
  is b64_decode("Zm9v\nYmFy\r\n"), 'foobar', 'b64_decode ignores whitespace';
};

subtest 'Mojo::JSON parity (encode_json / decode_json / to_json / from_json)' => sub {
  my $complex = {
    z_last   => [1, -2, 0, 3.14159, 1e12, -0.005],
    a_first  => 'hello world ' . ('x' x 64),
    escapes  => "\x00\x01\x08\x09\x0a\x0c\x0d\x1f\"\\/</script>",
    unicode  => "♥ mojolicious \x{10346} \x{0152} \xe9",
    bools    => [true, false, \1, \0, undef],
    nested   => {k1 => [1, {k2 => 'v2'}], k0 => []},
    to_json  => ParityTOJSON->new(data => {ok => true, n => 42}),
    overload => ParityOverload->new,
    dual     => dualvar(99, 'ninety-nine'),
    str_num  => '007',
  };

  my $accel_bytes = encode_json($complex);
  my $ref_bytes   = encode('UTF-8', Mojo::JSON::_encode_value($complex));
  is $accel_bytes, $ref_bytes, 'encode_json matches pure-Perl byte-for-byte';

  my $accel_chars = to_json($complex);
  my $ref_chars   = Mojo::JSON::_encode_value($complex);
  is $accel_chars, $ref_chars, 'to_json matches pure-Perl char-for-char';

  my $dec_accel = decode_json($accel_bytes);
  my $dec_ref;
  is Mojo::JSON::_decode(\$dec_ref, $accel_bytes), undef, 'pure-Perl decodes accelerated output';
  is_deeply $dec_accel, $dec_ref, 'decode_json matches pure-Perl structure';

  my $from_accel = from_json($accel_chars);
  is_deeply $from_accel, $dec_ref, 'from_json matches pure-Perl structure';

  # Core booleans
  if (defined &builtin::is_bool) {
    is encode_json([!!1, !!0]), '[true,false]', 'core booleans encoded';
  }

  # Inf / NaN stringified
  like encode_json({inf => 9**9**9}),       qr/^\{"inf":".+"\}$/, 'Inf encoded as string';
  like encode_json({nan => -sin(9**9**9)}), qr/^\{"nan":".+"\}$/, 'NaN encoded as string';

  # Error parity (falls back to pure Perl for exact messages)
  my @bad_cases = ('', '{', '["\ud800"]', '{"a":1}extra', '["♥"]', '[' x 513 . ']' x 513);
  for my $i (0 .. $#bad_cases) {
    my $bad = $bad_cases[$i];
    eval { decode_json($bad) };
    my $err_accel = $@;
    my $err_ref   = Mojo::JSON::_decode(\my $tmp, $bad);
    ok $err_accel, "malformed JSON case $i throws";
    like $err_accel, qr/\Q$err_ref\E/, "error message case $i matches pure-Perl";
  }
};

subtest 'HTTP/1.1 parser parity (Mojo::Message::Request + Mojo::Headers)' => sub {
  my $raw = join("\r\n",
    'POST /api/v1/items?foo=bar&baz=1 HTTP/1.1',
    'Host: example.com:8080',
    'User-Agent: Mozilla/5.0 (Macintosh; ARM64)',
    'Accept: application/json',
    'Accept: text/plain',
    'Content-Type: application/json; charset=utf-8',
    'X-Custom-Header: custom-value',
    'Content-Length: 13',
    '',
    '{"hello":123}'
  );

  my $req = Mojo::Message::Request->new;
  $req->parse($raw);
  ok $req->is_finished, 'request finished';
  is $req->method, 'POST', 'method';
  is $req->version, '1.1', 'version';
  is $req->url->path->to_string, '/api/v1/items', 'path';
  is $req->url->query->param('foo'), 'bar', 'query param';
  is $req->headers->host, 'example.com:8080', 'Host header';
  is_deeply $req->headers->every_header('Accept'), ['application/json', 'text/plain'], 'multi-value header';
  is $req->headers->header('X-Custom-Header'), 'custom-value', 'custom header';
  is_deeply $req->json, {hello => 123}, 'JSON body decoded';

  # Obs-fold fallback parity
  my $h = Mojo::Headers->new;
  $h->parse("X-Fold: one\r\n two\r\n\tthree\r\n\r\nleftover");
  ok $h->is_finished, 'obs-fold finished';
  is $h->header('X-Fold'), 'one two three', 'obs-fold folded identically';
  is $h->leftovers, 'leftover', 'leftovers preserved';

  # Incremental chunked header parsing
  my $h2 = Mojo::Headers->new;
  $h2->parse("Host: example.org\r\nX-A: ");
  ok !$h2->is_finished, 'incomplete header block not finished';
  $h2->parse("1\r\n\r\nbody");
  ok $h2->is_finished, 'completed after second chunk';
  is $h2->host, 'example.org', 'Host parsed';
  is $h2->header('X-A'), '1', 'X-A parsed';
  is $h2->leftovers, 'body', 'body leftover';

  # Line limit exceeded
  my $h3 = Mojo::Headers->new(max_line_size => 10);
  $h3->parse("X-Too-Long-Header: 1234567890\r\n\r\n");
  ok $h3->is_limit_exceeded, 'max_line_size limit detected';

  # Bad start-line
  my $bad_req = Mojo::Message::Request->new;
  $bad_req->parse("NOT_A_VALID_REQUEST_LINE\r\n\r\n");
  is $bad_req->error->{message}, 'Bad request start-line', 'bad start-line error';
};

subtest 'HTTP/1.1 response serializer (build_http_response) parity' => sub {
  require Mojo::Transaction::HTTP;

  # Direct XS call with 6-arg signature
  my $date = 'Fri, 20 Nov 2026 15:00:00 GMT';
  my $wire = Mojo::MT::Accel::build_http_response(
    200, undef,
    {'content-type' => ['text/plain;charset=UTF-8'], 'server' => ['Mojolicious (Perl)']},
    'Hello, World!', $date, 1
  );
  is $wire,
    "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nContent-Type: text/plain;charset=UTF-8\r\nDate: $date\r\nServer: Mojolicious (Perl)\r\n\r\nHello, World!",
    'build_http_response 6-arg keep-alive output matches exact HTTP/1.1 format';

  # Connection: close when keep_alive == 0
  my $wire_close = Mojo::MT::Accel::build_http_response(
    404, undef, {'content-type' => 'text/plain'}, 'nope', $date, 0
  );
  is $wire_close,
    "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 4\r\nContent-Type: text/plain\r\nDate: $date\r\n\r\nnope",
    'build_http_response keep_alive=0 adds Connection: close';

  # Full Mojo::Transaction::HTTP::server_write parity vs pure-Perl _write(1)
  my $tx_xs = Mojo::Transaction::HTTP->new;
  $tx_xs->req->method('GET');
  $tx_xs->res->code(200);
  $tx_xs->res->headers->date($date)->server('Mojolicious (Perl)')->content_type('text/plain');
  $tx_xs->res->headers->add('X-Custom-Hdr' => 'v1', 'v2');
  $tx_xs->res->body('Hello XS!');
  $tx_xs->resume;
  my $out_xs = $tx_xs->server_write;

  my $tx_pp = Mojo::Transaction::HTTP->new;
  $tx_pp->req->method('GET');
  $tx_pp->res->code(200);
  $tx_pp->res->headers->date($date)->server('Mojolicious (Perl)')->content_type('text/plain');
  $tx_pp->res->headers->add('X-Custom-Hdr' => 'v1', 'v2');
  $tx_pp->res->body('Hello XS!');
  $tx_pp->resume;
  my $out_pp = $tx_pp->_write(1);

  is $out_xs, $out_pp, 'Mojo::Transaction::HTTP::server_write XS fast-path matches pure-Perl byte-for-byte';
  ok $tx_xs->is_finished, 'XS fast-path marks transaction finished';
  ok $tx_xs->res->is_finished, 'XS fast-path marks response finished';
  is $tx_xs->res->headers->content_length, 9, 'XS fast-path populates Content-Length header on response';
};

subtest 'Multi-worker MT::Worker thread safety for Mojo::MT::Accel' => sub {
  plan skip_all => 'MT::Worker not available' unless HAS_MT && eval { require MT::Worker; 1 };

  Mojo::MT::prepare_for_freeze();
  MT::Image::freeze();

  my @workers;
  for my $wid (1 .. 4) {
    push @workers, MT::Worker->spawn(
      sub {
        for my $i (1 .. 50) {
          my $j = encode_json({worker => $wid, iter => $i, msg => "hi <$wid/$i> ♥", flag => true});
          my $d = decode_json($j);
          return 0 unless $d->{worker} == $wid && $d->{iter} == $i && $d->{flag};
          return 0 unless xml_escape("<w$wid&$i>") eq "&lt;w$wid&amp;$i&gt;";
          return 0 unless url_unescape(url_escape("a/b?c=$wid&$i")) eq "a/b?c=$wid&$i";
          return 0 unless b64_decode(b64_encode("data-$wid-$i", '')) eq "data-$wid-$i";
          my $req = Mojo::Message::Request->new;
          $req->parse("GET /w/$wid/$i HTTP/1.1\r\nHost: local\r\nX-W: $wid\r\n\r\n");
          return 0 unless $req->is_finished && $req->headers->header('X-W') eq "$wid";
          my $resp = Mojo::MT::Accel::build_http_response(
            200, undef, {'content-type' => ['text/plain']}, "w$wid-$i", 'Fri, 20 Nov 2026 15:00:00 GMT', 1
          );
          return 0 unless defined $resp && index($resp, "\r\n\r\nw$wid-$i") > 0;
        }
        return 1;
      }
    );
  }
  my $ok = 0;
  $ok += ($_->join ? 1 : 0) for @workers;
  is $ok, 4, 'all 4 MT::Worker threads executed Mojo::MT::Accel concurrently with 0 faults';
};

done_testing();
