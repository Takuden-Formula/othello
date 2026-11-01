#!/usr/bin/perl
# Othello - tiny local web server for macOS (uses only the Perl that ships with macOS)
# Usage: perl mac/serve.pl <root dir> <port file>   -> prints the chosen port, then serves until killed
use strict; use warnings;
use IO::Socket::INET;
use File::Spec;
use Cwd qw(abs_path);

my $root = abs_path($ARGV[0] || '.');
my $portFile = $ARGV[1];
my %mime = (
  html => 'text/html; charset=utf-8', js => 'text/javascript; charset=utf-8', mjs => 'text/javascript; charset=utf-8',
  wasm => 'application/wasm', task => 'application/octet-stream', css => 'text/css', json => 'application/json',
  png => 'image/png', svg => 'image/svg+xml', csv => 'text/csv; charset=utf-8', ico => 'image/x-icon', md => 'text/plain; charset=utf-8', glb => 'model/gltf-binary',
);
$SIG{PIPE} = 'IGNORE';
$SIG{CHLD} = 'IGNORE';   # finished children are reaped automatically
$| = 1;

my ($srv, $port);
for my $p (8801 .. 8809) {
  $srv = IO::Socket::INET->new(LocalAddr => '127.0.0.1', LocalPort => $p, Proto => 'tcp', Listen => 32, ReuseAddr => 1);
  if ($srv) { $port = $p; last; }
}
die "ERROR: no free port in 8801-8809\n" unless $srv;
if ($portFile) { open(my $f, '>', $portFile) or die; print $f $port; close $f; }
print "PORT $port\n";

sub reply {
  my ($c, $code, $type, $body) = @_;
  my $msg = $code == 200 ? 'OK' : $code == 404 ? 'Not Found' : 'Bad Request';
  print $c "HTTP/1.1 $code $msg\r\nContent-Type: $type\r\nContent-Length: " . length($body) .
    "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
  print $c $body;
}

# one child process per connection: Chrome opens spare connections that send nothing,
# and a single-threaded loop would wait on them forever
while (1) {
  my $c = $srv->accept or next;
  my $pid = fork;
  if (!defined $pid) { close $c; next; }
  if ($pid) { close $c; next; }
  close $srv;
  alarm 30;   # drop idle connections
  handle($c);
  exit 0;
}

sub handle {
  my ($c) = @_;
  binmode $c;
  my $line = <$c>;
  my $hdr; while (defined($hdr = <$c>)) { last if $hdr =~ /^\r?\n$/; }   # read the rest of the request headers
  unless (defined $line && $line =~ m{^(GET|HEAD) (\S+)}) { reply($c, 400, 'text/plain', '400') if defined $line; close $c; return; }
  my ($method, $path) = ($1, $2);
  $path =~ s/[?#].*//;
  $path =~ s/%([0-9A-Fa-f]{2})/chr(hex($1))/ge;   # raw UTF-8 bytes, same as the file names on disk
  $path =~ s{^/+}{};
  $path = 'index.html' if $path eq '';
  my $full = File::Spec->catfile($root, $path);
  my $real = -f $full ? abs_path($full) : undef;
  if (!$real || index($real, $root . '/') != 0) {
    reply($c, 404, 'text/plain', '404');
    print "404 $path\n" unless $path eq 'favicon.ico';
    close $c; return;
  }
  my ($ext) = $real =~ /\.([^.\/]+)$/;
  my $type = $mime{lc($ext // '')} // 'application/octet-stream';
  if (open(my $fh, '<:raw', $real)) {
    local $/; my $body = <$fh>; close $fh;
    reply($c, 200, $type, $method eq 'HEAD' ? '' : $body);
  } else { reply($c, 404, 'text/plain', '404'); }
  close $c;
}
