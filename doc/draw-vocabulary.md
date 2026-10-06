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

## Deferred

Out of scope for the M6 acceptance test, candidates for M6.5 vocabulary breadth:

- gradients and patterns (would be a fixed, parameterized set, never raw defs)
- richer text (spans, alignment, wrapping)
- images and the pixel surface (that is M7, a separate shared framebuffer node)
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
