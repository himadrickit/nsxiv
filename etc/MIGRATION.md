# Migrating from `sxiv`

`rsxiv` is *mostly* a drop-in replacement for `sxiv`, but not fully.
This document outlines some key differences to be aware of if you're migrating
from `sxiv`.

### Configuration directory

`sxiv` looks for config files under the directory
`${XDG_CONFIG_HOME:-${HOME}/.config}/sxiv`. E.g
`~/.config/sxiv/exec/key-handler`.

`rsxiv` uses the same logic to find the config dir but uses the name "rsxiv".
E.g `~/.config/rsxiv/...`.

The "exec" scripts such as `key-handler` and `image-info` in `rsxiv` has some
more features, but all previous argument order are preserved. And so if you have
any exec scripts, you can simply copy them over and they should just work.

### Xresources

The xresources config for `rsxiv` is under the "Rsxiv" namespace whereas `sxiv`
uses the "Sxiv" namespace. Some of the variables are also different between
`rsxiv` and `sxiv`, below is a table that shows the old and new names:

| sxiv                | rsxiv                    |
| :--                 | :--                      |
| Sxiv.background     | Rsxiv.window.background  |
| Sxiv.foreground     | Rsxiv.window.foreground  |
| Sxiv.barBackground  | Rsxiv.bar.background     |
| Sxiv.barForeground  | Rsxiv.bar.foreground     |
| Sxiv.font           | Rsxiv.bar.font           |

### Default window class

The window class of `rsxiv` is set to "Rsxiv" by default (can be overwritten via
`-N` flag). This usually shouldn't matter, unless you have scripts that search
for "Sxiv" window class.

### Thumbnail cache directory

Similar to config dir, the thumbnail cache dir of `rsxiv` is under the "rsxiv"
name instead of "sxiv". E.g `~/.cache/rsxiv`.

The "caching structure" in `rsxiv` is the same as `sxiv`. Which means that you
can simply rename the directory to `rsxiv`:

```console
$ mv  ~/.cache/sxiv  ~/.cache/rsxiv
```

If you want to have both `sxiv` and `rsxiv` installed at the same time, you can
even use symlink to avoid duplicate cache.
