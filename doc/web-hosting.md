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

A single process serves one browser; a new connection replaces the old. Hosting
independent users at the same time would need a per-session broker (one guest
process per user), which is the next step tracked as M15.2 in the roadmap.
