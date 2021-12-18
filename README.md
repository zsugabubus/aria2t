# aria2t

Stupid TUI for `aria2c(1)` with Vim-like keybindings. Strives to be
minimal but complete.

## Features

* **Multiple views.** Downloads, files, peers.
* **Efficient.** Request and update only what needed.
* **Scriptable.** Bind an executable to any key.
* **Batteries included.** Most RPC commands covered.

## Installation

### Dependencies

* `cc -std=c11` (Build only.)
* `coreutils` (Build only.)
* `gperf` (Build only.)
* `meson` (Build only.)
* `sed` (Build only.)
* `ncursesw`

### Build from source

```
git clone https://github.com/zsugabubus/aria2t &&
cd aria2t &&
meson build &&
meson compile -C build &&
meson install -C build
```

## Getting started

```
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2t
```

For available commands please refer to `aria2t(1)`. You can open manual page
using `man aria2t` after installation.

## License

Released under the GNU General Public License version v3.0 or later.
