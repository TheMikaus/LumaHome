# LumaHome

LumaHome is an experimental Luma3DS fork that adds a HOME Menu extension
runtime. Its first working feature is persistent and live alphabetical title
sorting through a top-screen overlay.

## Current baseline

- Upstream base: Luma3DS commit `d30ac8d` (after v13.4).
- Working runtime baseline: V195.
- Overlay shortcut: `L + Y`.
- HOME Menu navigation is suppressed while the overlay is open.
- Hooks recover after returning from system applications such as Notifications.
- Sorting updates the persistent HOME Menu layout and guarded live icon-index
  maps without replacing system, special, gap, or unknown map entries.
- The original root firmware is not replaced during development. Test builds
  are chainloaded from `/luma/payloads`.

The source still contains `cthulhu_` internal prefixes and SD log paths from the
prototype. These names are intentionally retained in this baseline because the
working hook ABI and diagnostic history depend on them. They can be renamed in
a separate, testable cleanup change.

## Building

Build with a current devkitARM/libctru environment supported by Luma3DS:

```sh
make -j2
```

The repository's normal Luma3DS output is `boot.firm`. During development,
rename a verified build to a versioned `LumaHome*.firm` and place it in
`/luma/payloads`; do not overwrite the console's known-good root `boot.firm`.

## Status and scope

V195 is a hardware-tested development checkpoint, not an upstream Luma3DS
release. Persistent sorting, live title movement, overlay input, resume from
Notifications, and power-off have been tested on the development console.
Live folder movement and search/filtering remain future work.

## License and attribution

LumaHome is derived from Luma3DS and remains licensed under GNU GPL v3. See
[`LICENSE`](LICENSE) and the upstream project at
<https://github.com/LumaTeam/Luma3DS>.
