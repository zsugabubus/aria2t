# aria2t

Stupid TUI for `aria2c(1)` with Vim-like keybindings. Strives to be
minimal and scriptable.

## Features

* **Multiple views.** Downloads list, files list and peers/servers list.
* **Event driven.** TUI never hangs because of slow Internet.
* **Efficient.** Requests and updates only what needed.
* **Scriptable.** Bind an executable to any (free) key.
* **Batteries included.** ~90% of RPC commands covered.

## Dependencies

* `ncursesw`

## Installation
```
git clone https://github.com/zsugabubus/aria2t &&
cd aria2t &&
make bootstrap &&
make &&
make install
```

## Usage

```
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2t
```

For available commands please refer to `aria2t(1)`.

## License

Released under the GNU General Public License version v3.0 or later.
