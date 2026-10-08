# Patches

`thirdparty/moonlight-embedded` is upstream code, vendored verbatim at the
commit pinned in `scripts/upstream.lock.json`. This directory holds every local
change made to it.

Keeping the changes as patches — rather than only as edits in the vendored tree
— is what makes an upstream upgrade tractable: bump the commit, re-apply, and
each patch either applies or tells you exactly what it conflicted with.

## Current patches

### `0001-mlp1-replace-avahi-with-built-in-mdns-resolver.patch`

The MLP1 sysroot has no avahi, and the device runs no avahi daemon, so host
discovery upstream's way cannot work. Rather than stub `gs_discover_server` out
and lose the feature, `libgamestream/discover.c` is rewritten to query
`_nvstream._tcp.local` over multicast DNS directly — roughly 300 lines of C, no
dbus, no daemon. Both entry points (`gs_discover_server` for a named host,
`gs_discover_servers` for listing) keep their original signatures and
behaviour, so nothing above them changed. `libgamestream/CMakeLists.txt` drops
the `avahi-client` requirement accordingly.

## Regenerating

After editing anything under `thirdparty/moonlight-embedded`:

```sh
(cd thirdparty/moonlight-embedded && git diff) > patches/<name>.patch
```

The vendored tree has no `.git` of its own. To regenerate with a diff, either
diff against a pristine checkout of the pinned commit, or temporarily
`git init` inside `thirdparty/moonlight-embedded`, commit the pristine state,
make the edits, then diff.
