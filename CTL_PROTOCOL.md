# psp2shell control protocol (port 3336)

A line-based protocol for scripts, next to the interactive shell (3333/3334) and the log stream
(3335). Up to 4 clients at a time; commands are executed one at a time.

## Framing

Request: one line, `\n`-terminated, at most 2047 bytes. Arguments are separated by spaces. An
argument containing spaces or quotes is written in double quotes, with `\"` and `\\` as escapes:

    mkdir -p "ux0:/data/my dir"

Response: any number of data lines starting with `* `, then exactly one status line:

    OK [text]
    ERR <code> <message>

`<code>` is the Vita error code (hex, e.g. `0x80010002`) or `0x80000000` for protocol errors.

Paths are absolute (`ux0:/...`, `ux0:data/...`); there is no working directory.

## Transfers

    get <path>
    -> DATA <size>\n  <size raw bytes>  OK
    -> ERR ...                           (nothing else follows)

    put <path> <size>
    -> READY            client then sends <size> raw bytes
    -> OK | ERR ...     after all bytes were received (on write errors the rest is still drained)
    -> ERR ...          instead of READY if the file can't be created; send nothing

`put` creates or truncates the file; parent directories must exist (`mkdir -p` first).

## Commands

| command | data lines | notes |
|---|---|---|
| `hello` | `* psp2shell-ctl 1` | protocol version |
| `ls <dir>` | `* d <size> <mtime> <name>` / `* f <size> <mtime> <name>` | mtime = `YYYY-MM-DDTHH:MM:SS` |
| `stat <path>` | `* <d\|f> <size> <mtime>` | ERR if missing |
| `md5 <file>` | `* <32 hex digits>` | |
| `mkdir [-p] <dir>` | | `-p`: create parents, no error if it exists |
| `rm <file>` | | refuses directories |
| `rmdir [-r] <dir>` | | `-r`: recursive |
| `mv <src> <dst>` | | rename; falls back to copy + delete across devices |
| `get` / `put` | see above | |
| `install <dir>` | | promote an unpacked VPK directory (needs `sce_sys/param.sfo` and `sce_sys/package/head.bin`) via ScePromoterUtil. The directory is consumed. |
| `destroy` | | close all running apps (`sceAppMgrDestroyOtherApp`) |
| `appstate <titleid>` | `* running <module>` / `* stopped <module>` | `sceAppMgrGetStatusByName`, looked up at runtime (`<module>` = where it was found); `ERR` if not available |

Legacy shell commands also work and return their usual text as data lines; anything they print
as an error turns the status into `ERR`: `launch <titleid>`, `title`, `reset`, `reboot`,
`modlist`, `modlistp <pid>`, `modinfo <uid>`, `modinfop <pid> <uid>`, `modstart <path>`,
`modstartp <pid> <path>`, `modstop <uid>`, `modstopp <pid> <uid>`, `kmodstart <path>`,
`kmodstop <uid>`, `moddump <pid> <uid> <dir>`, `thlist`, `memr <addr> <size>`, `memw <addr> <hex>`.

## Implementation notes

- `psp2shell_m/source/ctl.c`. Build with `-DCTL_TRACE` to trace every step into the kernel log
  (log port); each trace point pauses 100 ms so the line is sent before the next step runs.
- libk caveats (see `common/p2s_va.h`): its `va_start` is wrong on ARM, its printf has no
  precision (`%.*s`) and mishandles 64-bit arguments. Use `P2S_VA_*`, print 64-bit values as
  strings.
