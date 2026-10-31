# Neovim images

Use a Neovim build providing the experimental [`vim.ui.img` API][api] from the 0.13 API line.
Builds without that API retain ordinary text editing without the image backend.

dot installs its backend in the embedded Neovim instance before user configuration runs. It does not install a plugin
or write to your Neovim configuration. Both `vim.ui.img` and `require("vim.ui.img")` resolve to the backend; user
configuration can deliberately replace it afterward.

## Usage

Pass PNG bytes, not a filename:

```lua
local id = vim.ui.img.set(vim.fn.readblob([[C:\images\example.png]]), {
  col = 5,
  height = 8,
  row = 5,
  width = 16,
  zindex = 0,
})
```

Use the upstream [set/get/del interface][api] to update, inspect, or remove that image. Image IDs belong to the current
embedded session and are invalidated by restart.

## Placement

Coordinates are one-based cells on Neovim's global screen grid, not buffer positions. Explicit cell dimensions follow
the editor's font, linespace, and DPI. A requested width/height box preserves the image's aspect ratio and centers it;
one supplied dimension derives the other. With neither dimension, natural image dimensions are Qt logical pixels.

Signed `zindex` values follow [Kitty's placement ordering][layout], including images below text and non-default cell
backgrounds. The cursor remains above the whole image/grid subtree.

A grid scroll containing an image's projected anchor moves and clips that placement as a unit. After the anchor leaves
the region, the remembered region, or an enclosing region, continues to move its retained portion. A different partial
overlap without the anchor does not take ownership or split the image into fragments.

Scrolled-out source stays discarded when scrolling reverses. Every successful `set(id, opts)`, including a z-only or
empty update, merges the requested options and re-places the complete image. `get` reports those requested options,
not the automatically scrolled position.

Resize clips to the available grid without deleting the resource. Text-only clear/redraw does not delete images.
Automatic movement requires an emitted `grid_scroll`; a viewport change drawn by rewriting cells carries no image motion.
Plugins must explicitly update/delete placements when buffer identity, wrapping, folds, or window layout changes
cannot be represented by the applicable grid scroll. This backend does not provide buffer/extmark anchoring.

## Errors and resources

Uploads are validated and decoded before an ID is returned. Invalid PNGs/options, unknown update IDs, and exhausted
resources raise Lua errors. Missing IDs retain the upstream nil/false query/deletion results.

The limits are defined by [`ImageModel`][limits]. They bound encoded input, decoded images/cache reservations, and
retained image count. A concurrent upload is rejected while another is pending. Existing images are not silently
evicted to admit a new one.

The cache limit is not a total-process-memory cap: RPC buffers, temporary decode storage, Qt objects, and GPU copies
are separate. A successful upload acknowledges decoded state, not GPU presentation. A later texture failure is reported
in Neovim and invalidates that resource.

## Implementation boundaries

See the [application architecture][architecture] for component responsibilities. Image requests use the existing
msgpack-RPC connection, not a TTY flag, terminal escape parser, or additional transport.

The connection uses Neovim's standard `msgpack-rpc` client mode for coalesced replies. The backend flushes pending redraws
before image operations. Scroll projection publishes at `flush`; replies complete nested requests before their callers.
Decoding owns its input bytes, and stale work/replies cannot cross session generations.

Rendering invariants and validation gates are defined in the [agent guide](../../AGENTS.md).

[api]: https://github.com/neovim/neovim/blob/e0516a46e8cead23e119cd298ef5eea8a570a1de/runtime/lua/vim/ui/img.lua
[architecture]: ../../README.md#architecture
[layout]: https://sw.kovidgoyal.net/kitty/graphics-protocol/#controlling-displayed-image-layout
[limits]: ../../desktop/include/ImageModel.h
