# aria2t

Minimalistic TUI for `aria2c(1)` with Vim-like keybindings.

## Installation

```
git clone https://github.com/zsugabubus/aria2-tools &&
cd aria2-tools &&
make bootstrap &&
make &&
make install
```

## Usage

```
env ARIA_RPC_SECRET=mysecret ARIA_RPC_PORT=12345 aria2t
```

For available commands refer to `aria2t(1)`.


### Requirements

* `ncursesw`

## License

Released under the GNU General Public License version v3.0 or later.
