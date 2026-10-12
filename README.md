# YAPI

Yet Another ( PS5 ) PKG Installer payload.

It accepts a content ID, an HTTP or HTTPS pkg URL and an optional task name, submits a self-contained loopback URL to AppInstUtil and streams package bytes from the upstream server.

This technique is necessary to use any url with the PS5's AppInstUtil.

## Build

Build with the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk):

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk make
```

Load the resulting elf with a PS5 ELF loader such as elfldr.

The default build is optimized and strips debug information and the static symbol table.
To build a debug payload, use `make -B DEBUG=1`. It includes DWARF debug information
and static symbols, enables klog output with the `[PS5-PROXY]` prefix, and sends
console notifications for lifecycle events and errors. Use `make -B DEBUG=0` to
switch back to release mode. Dynamic symbols required by the loader remain in both variants.

To run the host stream/control tests and build both variants with symbol checks:

```sh
PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk bash tools/build.sh
```

This produces `dist/yapi-debug.elf`, `dist/yapi-release.elf`, and `dist/SHA256SUMS.txt`.
The script uses LLVM 18 by default; set `HOSTCC` and `LLVM_READOBJ` for another version.
The host tests mock PS5 APIs and do not replace testing on a real console.

## GitHub builds and releases

The **Build and test** workflow runs for pushes to `main`, pull requests, and manual
checks. It tests both build modes, compiles both payloads, and verifies their symbol
policy. CI uses the checksum-verified ps5-payload-dev SDK **v0.44** and LLVM **18**.

To publish a release, open **Actions → Publish release → Run workflow**, select
`main`, and enter a new version tag such as `v1.0.0`. The workflow runs all checks
again and uploads both payloads and their checksums directly to GitHub Releases.
It accepts an optional prerelease flag and refuses to overwrite existing tags.

No workflow uploads Actions artifacts. Pushes, pull requests, and manual CI checks
do not save payload builds; only manually dispatched release runs publish them.

## Control protocol

TCP port **54309** accepts one command per connection, terminated by a newline:

```text
install <content-id> <package-url> [task-name]
stop
```

For example, using a placeholder content ID:

```sh
printf '%s\n' 'install XX0000-CUSA00001_00-ABCDEFGHIJKLMNOP https://example.org/game.pkg?token=abc' | nc PS5_IP 54309
printf '%s\n' 'install XX0000-CUSA00001_00-ABCDEFGHIJKLMNOP https://example.org/game.pkg?token=abc Test Update' | nc PS5_IP 54309
```

Supply the actual content ID of the package.

Package URLs may be at most 4095 ASCII characters and must use `http://` or `https://`.

The reply is `0\n` when AppInstUtil accepts the submission, or its nonzero signed
decimal error code. Malformed commands receive `-1\n`. Acceptance means that
the install was submitted, not that the asynchronous download completed.

Every install command creates a new submission, including identical commands.
There is no duplicate detection, version field, special retry command or
accepted-request history. After a lost reply, check Downloads before submitting
again. Clients should only repeat an uncertain submission after user action.
Plain URL commands and the former version-based protocol are unsupported.

## Contributing

Contributions are welcome, but tests for changes ( if applicable ) are required.

Changes must be tested on a real console before submitting pull-requests.

AI pull requests are welcome, but changes must be tested and confirmed beforehand.

## AI Disclosure

This project is mostly AI-coded with human speccing, testing and supervision.

Testing on real hardware is always done before merging any change in main.

## License and attribution

see [LICENSE](LICENSE).

The installer ABI declarations is inspired/follows [phantomptr/ps5upload's sceAppInstUtil.h](https://raw.githubusercontent.com/phantomptr/ps5upload/refs/heads/main/payload/include/sceAppInstUtil.h).

Building this payload requires the latest version of [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk).
