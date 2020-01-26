# aria2t

`Aria2c(1)` frontend for terminal guys. See `aria2t(1)`.

## Installation

### Requirements

```
ncursesw
```

### Build from Source

```sh
git clone https://github.com/zsugabubus/aria2t &&
cd aria2t &&
make bootstrap &&
make
make install
```

## Running

See `aria2t(1)` for details. (Or `man ./aria2t.1` if you didn't run install yet.)

```sh
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2t
```

## License

Released under the GNU General Public License version v3.0 or later.
