# dmg07-libretro

Nintendo's DMG-07 4 Player Adapter as a libretro core, for RetroXR. No CPU and no
game: it attaches four link ports (RETRO_ENVIRONMENT_GET_LINK_INTERFACE, the
`gb-sio` wire the RetroXR builds of gambatte and mGBA speak), one per player, and
drives the serial clock for every Game Boy cabled to it, as the real adapter does.
Port k is player k+1; player 1's cable powers it.

The protocol (`src/dmg07.c`) is written from Pan Docs' "4-Player Adapter"
chapter: ping packets of `FE STAT STAT STAT`, the `AA` switch to transmission and
its `CC` indicator, data packets of SIZE x 4 bytes relayed a packet late, and the
`FF` restart. What Pan Docs leaves open is marked "chosen here" in the source.

## Build

```sh
cmake -S . -B build && cmake --build build --config Release
ctest --test-dir build -C Release      # Godot-free protocol test
```

## License

MIT.

## Status (2026-11-03)

Wave Race on four gambatte Game Boys: all four players join, names and menus
cross to every machine, and the starting grid agrees on every screen. The race
itself does not start yet (the countdown holds at 3). Paced 59-60 fps with four
linked; 104-114 fps unthrottled on a desktop PC.

`DMG07_TRACE=1` (or `=N`, from packet N) logs every slot's exchange.
`DMG07_TIMING=gbe|bytes` tries GBE+'s packet pacing or no 17 ms floor; both put
Wave Race's menus out of step, so Pan Docs' pacing is the default.

## Release

Push a `retroxr-dmg07-libretro-vN` tag: `.github/workflows/retroxr-release.yml`
builds Windows, Android arm64, Linux and macOS and publishes them.
