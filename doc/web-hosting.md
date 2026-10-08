# Web hosting

Jotun's display is already a web stack. The host serves a small browser client
and streams the guest's screen over a WebSocket (`host/drawws.c`), and input
events flow back the same way. To reach it from a browser over the network you
add one thing: a TLS-terminating reverse proxy in front of the loopback port.
This is a single shared session: one guest, one browser at a time.

## Run it

1. Build the host, the guest, and a bundle for this machine's architecture:

   ```
   make
   make bundle BUNDLE_ARCHS=x86_64      # or aarch64
   ```

2. Start the guest with the draw server open (default port 9230). The `web`
   target runs the jsh web terminal:

   ```
   make web
   ```

   Any guest works. To host a different one, run `jotun` directly with
   `JOTUN_DRAW_PORT=9230` and the guest you want (for example `triton`).

3. Put Caddy in front for TLS and open the browser:

   ```
   caddy run --config example/web/Caddyfile
   ```

   Open `https://localhost:8443/` and accept the local certificate once. For a
   real deployment, edit `example/web/Caddyfile` to name a domain; Caddy then
   gets a certificate automatically.

## The web terminal

`make web` runs the `webterm` guest: the jsh shell rendered as a monospace text
screen over the draw channel. Type a command and press Enter. Builtins (`ls`,
`cat`, `echo`, `bind`, `mount`) and bundle programs (`hello`) all work, and
output appears both in the browser and on the host console. Paste is supported.
Keyboard input travels as `DEV_TEXT` and `DEV_PASTE` draw events; a single event
value is capped near 63 bytes, so a long paste is split into chunks.

## Smoke test

`make web-smoke` checks the whole path without a browser: it spawns the webterm
guest with the draw server, connects a WebSocket, types a command as input
events, and confirms the output comes back rendered. It needs a bundle built
first (`make bundle BUNDLE_ARCHS=<arch>`).

## Security model

The host binds the draw server to loopback only and has no TLS and no
authentication of its own, by design. The reverse proxy is the trust boundary:
it owns TLS and, if you want to restrict access, authentication (for example
Caddy `basic_auth`), off the box. This matches the split in the design doc
(section 9.2): the host stays trusted, the guest stays sandboxed, and the proxy
is an explicit front, not a hole in that model. See the M10 and M15 decisions in
`doc/roadmap.md`.

A single `make web` process serves one browser; a new connection replaces the
old. To host independent users at the same time, use the broker below.

## Multiple users

`make web-multi` runs `brokerd`, a supervisor that gives each browser its own
`jotun` child process. One session is one child: the ordinary single-guest stack
with its own loopback draw port, fully isolated from the others at the
address-space level. Teardown is kill-and-reap, so a session leaks nothing into
the rest. `jotun` itself is unchanged; the broker is a separate program that
proxies each browser to its child.

```
make web-multi
```

Front it with the same proxy as the single-session case. The default
`example/web/Caddyfile` already points at port 9230, which is also brokerd's
default, so no change is needed:

```
caddy run --config example/web/Caddyfile
```

Open `https://localhost:8443/` in two different browsers (or two private
windows) and each gets its own terminal. A `jsess` cookie ties a browser's page
load and its WebSocket to one child, so the two sessions never see each other.

The policy is trivial and inline: every session runs the same bundle and guest
(the webterm by default). Environment variables tune the mechanism:
`JOTUN_BROKER_PORT` (listen port, default 9230), `JOTUN_BROKER_PORTBASE` (first
child draw port, default 9300), `JOTUN_BROKER_MAX` (concurrent sessions, default
16; a request past the cap gets a 503), `JOTUN_BROKER_IDLE` (seconds before an
idle session is reclaimed, default 300), and `JOTUN_BROKER_GUEST` (the guest to
run, default `webterm`). Each child also runs under `setrlimit` caps on address
space, CPU time, and open files. A broker shutdown (Ctrl-C) kills every child.
Sending brokerd `SIGUSR1` (`kill -USR1 <pid>`) dumps the live session table
(user, pid, port, and idle age) to its stderr, for debugging admission and
quotas without exposing a network surface.

`make broker-smoke` checks this headless: it starts brokerd, opens two
independent sessions, types a different token into each webterm, and asserts each
session sees only its own token before tearing everything down. Like
`make web-smoke` it needs a bundle built first.

`make caddy-smoke` checks the TLS front end: it runs brokerd behind Caddy (the
`make web-multi` recipe) and reaches the site over TLS with `openssl s_client`,
confirming two cookieless requests each come back `302` from brokerd through the
proxy with their own `jsess` cookie. It needs `caddy` on the PATH and a bundle.

Several sessions serve the same read-only depot at once, which the depot's
shared read lock allows (`cas_new_ro`); a bundle rebuild, which needs the
exclusive write lock, waits for the serving processes to exit first.

This is the M15.2 host mechanism. The next step, tracked in `doc/roadmap.md`,
moves the admission and bundle-selection policy out of brokerd and into a
sandboxed broker guest over a host control channel.

## Authentication and per-user admission

Authentication stays off the box in the reverse proxy, as M10 decided. For a
multi-user deployment the M15.3 pieces add a bearer-token gate in front and a
per-user session cap in brokerd, without the host learning any tokens.

- `authd` is a small loopback validator Caddy delegates to with `forward_auth`.
  It maps a bearer token to a user and a per-user cap, read from a token store
  file (`token user [cap]` lines, path in `JOTUN_AUTH_TOKENS`). On a valid token
  it returns `X-Jotun-Auth: <secret> <user> <cap>`, which Caddy copies onto the
  request to brokerd.
- brokerd, when `JOTUN_BROKER_AUTH_SECRET` is set, trusts only the `X-Jotun-Auth`
  line whose first token is that shared secret, so a client cannot forge the
  identity. It enforces the per-user cap (the header's value, or
  `JOTUN_BROKER_USER_MAX`, default 3), refuses a request with no valid identity
  (`403`), and honours a `jsess` cookie only for its owner, so one user's cookie
  cannot attach to another's session. With the secret unset, brokerd behaves as
  before (open), so `make web-multi` is unchanged.
- A client presents its token as `Authorization: Bearer <token>` on every
  request (programmatic clients), or as the `jauth` cookie a browser obtains once
  by visiting `/login?token=<token>`. The cookie is how a browser carries the
  token to the WebSocket, which cannot take a custom header.

Mint a token with `./jmint <user> [cap]` (it appends to `JOTUN_AUTH_TOKENS` and
prints the token once). List the stored tokens, masked, with `./jmint -l`, and
revoke a user's tokens with `./jmint -r <user>`; authd reloads the store on
change, so a revoke takes effect without a restart. Run the pair with
`make web-auth JOTUN_BROKER_AUTH_SECRET=<secret>` and front it with
`example/web/Caddyfile-auth`. `make auth-smoke` checks it headless: authd's token
validation (header, cookie, and `/login`), and brokerd's per-user caps and
identity gate.

The box itself stays trusted: brokerd binds loopback and trusts the identity
header because only Caddy, through authd, can produce the shared secret. Tokens
are bearer secrets, carried over TLS only; keep the store file readable only by
the operator.
