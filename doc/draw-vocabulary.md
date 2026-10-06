# The /dev/draw op vocabulary (M6 sketch)

Status: draft, to be refined at the start of M6.1. This sketches the structured
drawing vocabulary the guest emits over `/dev/draw`. It is the concrete form of
the M6 decision recorded in `roadmap.md`: the guest never emits markup, the host
validates every op and field, and the host is the sole author of the SVG sent to
the browser. See `unikernel-design.md` section 9 for the surrounding design.

## Principles

1. The guest emits structured binary ops, not text and not markup. There is no
   SVG, no XML, and no path or color string anywhere in the guest-to-host
   direction.
2. The host validates every field against this specification before acting, and
   constructs all SVG itself from validated values. A malformed op is dropped
   and flagged, never forwarded.
3. The vocabulary is small and closed. Adding an op or an attribute is a
   deliberate change here, not something a guest can do by sending novel bytes.
4. Interaction that the browser can handle locally (hover, press, drag) is
   expressed as a parameterized annotation on a node, not as raw animation. The
   host emits the corresponding SMIL or `data-qu-*` markup. The guest cannot
   inject either.

## Model

Each guest owns a retained scene: a forest of surfaces, each a tree of nodes.
The guest assigns every node a nonzero `u32` id, unique within that guest. The
host namespaces ids per guest, so the browser DOM id is `qu-<guest>-<node>`,
matching the Quaoar client's existing `APP_ID-WIDGET_ID` scheme. Id 0 is
reserved (broadcast and paste, as in Quaoar).

The host keeps the authoritative tree. Ops are mutations on it: create a node
under a parent, set attributes, set text, annotate interaction, listen, remove.
This is Quaoar's protocol made structured and validated.

## Encoding

Ops ride the `/dev/draw` channel (defined in M6.1) as a byte stream, chunked by
the channel rather than one op per slot. The encoding reuses the little-endian,
bounds-checked cursor style already used for 9P and net (`abi/jotun_9p.h`,
`p9o_*`/`p9i_*`); a `draw_out`/`draw_in` pair will mirror it. The guest has no
`printf`, so every field is a fixed-width integer or a length-prefixed byte
string, never formatted text.

Field types:

| Type    | Encoding                                                      |
|---------|---------------------------------------------------------------|
| `u8`    | one byte                                                      |
| `u16`   | two bytes, little-endian                                      |
| `u32`   | four bytes, little-endian                                     |
| `i16`   | two bytes, little-endian, signed (coordinates)                |
| `str`   | `u16` length then that many UTF-8 bytes, host-escaped, capped |
| `color` | `u32`: the sentinel `0x01000000` means "none", otherwise the low 24 bits are `0xRRGGBB` |
| `norm`  | `u16` normalized fraction, `0` to `65535` maps to `0.0` to `1.0` (used for event values; avoids a float ABI) |
| `enum`  | `u16`, range-checked against the fixed set below              |

Every op begins with a `u8` opcode. The host reads exactly the fields the
opcode defines and validates each; a short or over-long op is a protocol error.

## Style block

Shape ops carry a trailing style block: a `u16` presence mask, then the value of
each present field in ascending bit order. All fields are optional; absent
fields inherit the SVG default. This keeps common nodes small and every value
typed and checkable.

| Bit | Field          | Type   | SVG attribute      | Validation                    |
|-----|----------------|--------|--------------------|-------------------------------|
| 0   | fill           | color  | `fill`             | mask to 24 bits, or "none"    |
| 1   | stroke         | color  | `stroke`           | mask to 24 bits, or "none"    |
| 2   | stroke_width   | u16    | `stroke-width`     | capped (for example <= 1024)  |
| 3   | opacity        | u8     | `opacity`          | `0..255`, emitted as `v/255`  |
| 4   | font_family    | enum   | `font-family`      | one of a fixed set (below)    |

`font_family` is an enum, not a string, so no CSS reaches the host: `0` sans,
`1` serif, `2` monospace, each mapped to a fixed, safe family stack.

## Ops: guest to host

Structure:

- `0x01 SURFACE  {id:u32, x:i16, y:i16, w:u16, h:u16, title:str}`
  A top-level window with a titlebar and close button. Maps to Quaoar `window`.
- `0x02 GROUP    {id:u32, parent:u32, tx:i16, ty:i16}`
  A positioned container. Maps to `<g transform="translate(tx,ty)">`.

Shapes (each ends with a style block):

- `0x10 RECT     {id, parent, x:i16, y:i16, w:u16, h:u16, rx:u16, style}`
- `0x11 CIRCLE   {id, parent, cx:i16, cy:i16, r:u16, style}`
- `0x12 ELLIPSE  {id, parent, cx:i16, cy:i16, rx:u16, ry:u16, style}`
- `0x13 LINE     {id, parent, x1:i16, y1:i16, x2:i16, y2:i16, style}`
- `0x14 POLYLINE {id, parent, closed:u8, n:u16, pts:i16[2*n], style}`
  `closed` selects `<polygon>` over `<polyline>`. `n` is capped.
- `0x15 PATH     {id, parent, n:u16, seg:segment[n], style}`
  Each `segment` is `cmd:u8` then its argument count of `i16`. The command set
  is closed: `M` and `L` take 2, `H` and `V` take 1, `C` takes 6, `Q` takes 4,
  `Z` takes 0. The host reads exactly that many arguments and re-emits the path
  string itself, so no path text crosses from the guest. Unknown commands are a
  protocol error.
- `0x16 TEXT     {id, parent, x:i16, y:i16, size:u16, s:str, style}`
  The host XML-escapes `s` and marks the element as the text target for
  `SET_TEXT`.
- `0x17 IMAGE    {id, parent, x:i16, y:i16, w:u16, h:u16, fmt:u8, stride:u32, gpa:u64}`
  A framebuffer node (the pixel path, M7). The pixels live in guest RAM at
  `gpa`, in format `fmt` (`0` RGBA8888), `stride` bytes per row (`0` for `w*4`).
  The host validates the geometry, reads the pixels from guest memory through a
  bounds-checked translation, encodes them as a PNG, and emits an `<image>` data
  URI. No pixel command stream crosses into the host, only a declared buffer.

Mutation:

- `0x20 SET      {id:u32, attr:enum, value...}`
  Set one attribute. `value` is typed by `attr`. Allowed `attr`: `X`, `Y`, `W`,
  `H`, `CX`, `CY`, `R`, `RX`, `RY`, `FILL`, `STROKE`, `STROKE_WIDTH`, `OPACITY`,
  `VISIBLE`. Each maps to a specific SVG attribute the host sets by node id.
- `0x21 SET_TEXT {id:u32, s:str}`
  Replace the text content of a text node. Host-escaped.
- `0x22 REMOVE   {id:u32}`
  Remove a node and its subtree.
- `0x23 INTERACT {id:u32, kind:enum, params...}`
  Attach a declarative behavior. `kind`:
  - `HOVER_FILL {to:color}`  host emits a SMIL `<set fill>` on mouseover/mouseout
  - `PRESS_FILL {to:color}`  host emits a SMIL `<set fill>` on mousedown/mouseup
  - `DRAG {axis:u8, track:u16}` host emits `data-qu-drag`/`data-qu-track`; the
    client reports a `scroll` event on release
  - `XY {}` host emits `data-qu-xy`; the client reports an `xy` event on release
  - `CLICK {}` host registers a click listener (Quaoar `listen id click`)
- `0x24 LISTEN   {id:u32, event:enum}`
  Register for an event type the client can produce, for cases not covered by an
  `INTERACT` kind. `event` from the event enum below.

## Events: host to guest

Delivered on the same channel, read by the guest draw client (which blocks the
calling task through the M5.1 primitive until an event arrives).

- `0x80 EVENT {id:u32, event:enum, value...}`
  - `CLICK`  no value
  - `CLOSE`  no value (window close button)
  - `SCROLL {v:norm}`  scrollbar position, `0.0` to `1.0`
  - `XY {x:norm, y:norm}`  2D handle position
  - `TEXT {s:str}`  textarea content, debounced by the client
  - `PASTE {s:str}`  clipboard text, delivered with id 0 (broadcast)

The event enum (`CLICK=1, CLOSE=2, SCROLL=3, XY=4, TEXT=5, PASTE=6`) is shared by
`LISTEN` and `EVENT`.

## Mapping to SVG (illustrative)

The host authors all of this. A guest `RECT` with fill and a `HOVER_FILL`
annotation becomes, entirely from validated numbers:

    <rect id="qu-3-7" x="10" y="10" width="70" height="28" rx="4"
          fill="#5a5a62">
      <set attributeName="fill" to="#6a6a72"
           begin="mouseover" end="mouseout"/>
    </rect>

Because every attribute value is reconstructed from a typed field, there is no
place for a guest to inject `<script>`, an `on*` handler, a `foreignObject`, or
an external reference. That is the whole point of the restricted vocabulary: the
host's validation is cheap because the input space is small and closed.

## Limits

The host enforces caps and drops ops that exceed them (the browser's SVG DOM
tops out around five to ten thousand nodes, per the Quaoar research):

- nodes per guest: a few thousand (target 4096 initially)
- surfaces per guest: small (for example 16)
- string length: capped (for example 4096 bytes after escaping)
- polyline points and path segments: capped per node
- op size: bounded by the channel chunk size

## Serving and TLS

The host serves the browser client and the WebSocket on one port, bound to
`127.0.0.1` only (`JOTUN_DRAW_PORT`, default 9230, `0` disables the front end).
Nothing listens on a public interface, so the display is reachable only from the
same machine. The WebSocket link is plain `ws://`, which is correct for a
loopback connection where there is no network to protect.

To reach the display from another machine, put a TLS-terminating reverse proxy
in front of it rather than adding TLS to the host. The host stays loopback-only
and the proxy owns the certificate. For example, with nginx:

    location / {
        proxy_pass http://127.0.0.1:9230;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;     # forward the WS upgrade
        proxy_set_header Connection "upgrade";
    }

The client already selects `wss://` when the page is loaded over HTTPS, so no
client change is needed. The proxy is also the right place for authentication
and for any public-facing rate limiting, keeping that policy out of the host.
The host's own limits (frame size, event rate, a single browser at a time)
still apply underneath.

## Scale and latency (M6.6)

The host keeps the scene in an id-indexed tree: an id-to-slot hash makes lookup
O(1), a free-slot stack makes allocation O(1), and sibling links keep children
in creation order so the whole scene serializes in one O(n) walk. These replaced
the first implementation's linear scans, which were quadratic over a build.

Measured on linux/x86-64 (KVM), authoring the full SVG for a scene of rects:

| nodes | serialize (first cut) | serialize (indexed) |
|-------|-----------------------|---------------------|
| 500   | 8.4 ms                | 0.36 ms             |
| 1000  | 18 ms                 | 0.69 ms             |
| 2000  | 43 ms                 | 1.3 ms              |
| 4000  | 111 ms                | 2.6 ms              |

Serialize is now linear in node count, about 0.65 us per node, so a full
re-render of a scene at the node ceiling is about 2.6 ms. Applying an op is
O(1), so building the whole 4096-node scene applies in under 2 ms total.
Building a 4000-node scene from scratch (13 channel chunks, each re-rendered) is
about 21 ms of host work in all.

The node ceiling is 4096 per guest, matching the browser's SVG DOM limit (the
Quaoar research puts it at five to ten thousand). Ops past the cap are dropped,
and the ceiling holds: a run that asks for 5000 rects settles at 4096 nodes.

The host still authors the full SVG every SUBMIT (cheap at these sizes, a 2.6 ms
re-render at the ceiling), but it no longer always transmits it. A browser that
already holds a full-document baseline receives an incremental delta instead,
which ships only the changed nodes. See "Incremental updates" below.

## Incremental updates (M7.2)

The host tracks per-node change state while it applies an op stream: a node is
added, modified, or removed. The first paint to a browser is always the full
document, which establishes a baseline. After that, each SUBMIT produces a delta
that names only what changed, and the browser applies it against the live DOM by
element id.

The delta is a line-oriented text message. The first line is a header,
`!<cw> <ch>`, carrying the canvas width and height. Each later line is one
record:

- `R <id>` removes the node with that guest id.
- `N <parent> <fragment>` adds a node: the SVG fragment is appended under the
  parent node's element (parent id 0 is the scene root).
- `U <fragment>` replaces a node in place, matched by the fragment's element id.

The browser tells a delta from a full document by the first byte: a full
document starts with `<`, a delta with `!`. Fragments never contain newlines, so
the split on newlines is unambiguous.

The host falls back to sending the full document whenever a delta cannot be
applied in order: the browser has no baseline yet, a previous delta is still
queued unsent, or the change set for one SUBMIT exceeds the per-submit cap (128
records). A full document re-establishes the baseline, so the browser never
drifts out of sync. A DOP_IMAGE node re-encodes its PNG only when its pixels are
marked dirty, not on every unrelated SUBMIT.

Measured on the 2D demo, toggling one rect's fill with a single SET goes out as
a 226 byte delta in place of the 1144 byte full document.

On bounding the guest-to-host op rate (the earlier open question): the submit
path is synchronous. Each SUBMIT occupies the vcpu until the host returns, so
the guest is naturally backpressured and cannot outrun the host. An explicit
guest op-rate limit is not needed while submission is synchronous; it would only
matter if asynchronous or batched submission were added later. The inbound
browser event rate is separately capped (M6.5).

## Deferred

Out of scope for the M6 acceptance test, candidates for M6.5 vocabulary breadth:

- gradients and patterns (would be a fixed, parameterized set, never raw defs)
- richer text (spans, alignment, wrapping)
- richer raster: the framebuffer node ships with real PNG compression (M7.1),
  and changed-node deltas avoid re-sending it when unrelated nodes change
  (M7.2); still deferred are dirty-rectangle partial updates that re-encode and
  re-transmit only changed regions of a framebuffer
- `font_family` beyond the three-way enum
- grouping transforms beyond translate (scale, rotate), if a use case needs them

## Open questions

- Whether coordinates should be `i16` device units or a normalized fixed-point
  space that the host maps into the surface viewBox for resolution independence.
- Whether `SET` should coalesce multiple attributes in one op to cut op count
  for animation-like updates.
- Per-guest op rate bounding. The inbound browser event rate is now capped
  (M6.5). Bounding the guest-to-host op submission rate is still open and
  belongs with the M6.6 scale and latency work.
