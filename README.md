# aria2-tools

`aria2-watch(1)`: Minimal TUI for `aria2c(1)`.

`aria2-fuse(1)`: Soon...

## Installation

### Requirements

```
ncursesw
```

### Build from Source

```sh
git clone https://github.com/zsugabubus/aria2-tools &&
cd aria2-tools &&
make bootstrap &&
make
make install
```

## Running

See `aria2-watch(1)` for details.

```sh
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2-watch
```

## License

Released under the GNU General Public License version v3.0 or later.
