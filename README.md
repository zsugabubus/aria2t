# aria2t

Stupid TUI for `aria2c(1)` with Vim-like keybindings. Strives to be
minimal but complete.

## Features

* **Multiple views.** Downloads, files, peers.
* **Efficient.** Requests and updates only what needed.
* **Scriptable.** Bind an executable to any key.
* **Batteries included.** Most RPC commands covered.

## Installation

### Dependencies

* `cc -std=c11`
* `coreutils`
* `gperf`
* `make`
* `ncursesw`
* `sed`

### Build from source

```
git clone https://github.com/zsugabubus/aria2t &&
cd aria2t &&
make &&
make install
```

## Getting started

```
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2t
```

For available commands please refer to `aria2t(1)`. You can open manual page
using `man aria2t` after installation.

## License

Released under the GNU General Public License version v3.0 or later.
