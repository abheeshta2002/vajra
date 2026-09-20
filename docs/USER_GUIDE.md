# Vajra OS — user guide

Everything a person can do in Vajra today: the desktop, the shell, files and
directories, the editor, pipes and redirection, every tool, packages, getting
files in and out, and what the security model means in day-to-day use. For the
*why* behind these choices see `docs/ROADMAP.md` ("The usable-for-real-work
track") and `docs/PHILOSOPHY.md`.

---

## 1. Starting it

```
tools/run.ps1                 build, then boot with the desktop window, 2 cores
tools/run.ps1 -Smp 4          more cores (up to 16)
tools/run.ps1 -Net            attach a virtual network card (see the Fabric app)
```

The desktop is text-mode (80×25). The taskbar lists the apps; the **keyboard**
switches between them:

| Key | App | What it is |
|---|---|---|
| F1 | Log | the system log: everything the demo actors print |
| **F2** | **Shell** | the command line (most of this guide) |
| F3 | Files | a live list of every stored object |
| F4 | About | version and memory |
| F5 | Security | the Security Lab: attack Vajra on purpose, watch the hardening hold |
| F6 | Fabric | actors talking to another Vajra over the network (`h` hello, `p` ping, `r` run a program on the peer) |
| F7 | Cores | every CPU core live, a parallelism test (`b`), a kill test (`s`) |
| F8 | — | the bare desktop |

F1–F8 are handled by the kernel itself and never reach an application, so no
program can steal or fake a window switch. The mouse works too (click a tab), but
its sensitivity is untuned.

**Text cursor**: the focused window shows a solid block where the next
character will land.

---

## 2. The shell (F2)

The prompt is `vajra>`; inside a directory it shows the path: `vajra:docs/>`.

### Line editing

| Key | Does |
|---|---|
| ← → | move the cursor |
| Home / Ctrl-A, End / Ctrl-E | start / end of line |
| Backspace, Delete | delete before / at the cursor |
| Ctrl-U | clear the whole line |
| ↑ ↓ | the last six lines you typed |
| (typing) | inserts at the cursor, not only at the end |

A line holds up to 95 characters.

### Built-in commands

`help`, `clear`, `echo <text>`, `date`, `cd`, `pwd`, `mkdir`, `rmdir`, `run <name>`,
`count`, `pipe` (two demo actors), `jobs`, `stop <slot>`, `kill <slot>`, `exit`,
`pkg list`, `pkg install <name>`. Everything else is a **program** (section 6).

---

## 3. Files, names and directories

A file is an **object**: up to **12288 bytes (12 KB)**, with a name of up to **39
characters**, a size, a created time, a modified time, a trust state and a
read-only flag. The store holds up to **96 objects**.

**Directories are name paths.** `docs/notes.txt` is one object whose name contains
a slash; `docs/` (size 0) is the marker that makes `docs` a directory. The shell
keeps a current directory and turns every path you type into a full object name
before a program sees it, so *every* tool understands paths without knowing it.

```
mkdir docs                make a directory (the parent must exist)
mkdir docs/2026
cd docs                   go in; `cd` alone goes to the root; `cd ..`; `cd /abs/path`
pwd                       /docs
rmdir docs/2026           only if empty (and you are not inside it)
ls                        the current directory (ls docs, ls /)
tree                      everything below, indented
```

Rules: `.` and `..` work; a path that starts with `/` is absolute; a name
(including the path) may not exceed 39 characters. Moving a file between
directories is just renaming it: `mv a docs/a`.

**Trust states** shown by `ls`/`stat`: *untrusted* (any file you create),
*trusted* (a program the kernel or the installer vetted), *REJECTED* (failed
inspection; permanently unreadable). Only a *trusted* object can be run as a
program, so a file you write is never executable by accident.

**Domains**: *user* objects are yours (the shell may read, write, rename and
delete them); *system* objects (the built-in programs, demo data) are not, and
the shell cannot alter them — `rm ls` is refused.

---

## 4. The editor: `edit <name>`

A full-screen editor (it takes over the shell window; the status bar is the
bottom row). Creates the file if it does not exist. Files up to 12 KB.

| Key | Does |
|---|---|
| arrows, Home, End, PgUp, PgDn | move (Ctrl-A / Ctrl-E: start / end of line) |
| type, Enter, Tab (4 spaces), Backspace, Delete | edit anywhere |
| **Ctrl-S** | save |
| **Ctrl-Q** | quit — asks you to press it twice if there are unsaved changes |
| Ctrl-Z / Ctrl-Y | undo / redo (a run of typing is one step) |
| Ctrl-F, Ctrl-G | find, find next (wraps around) |
| Ctrl-R | replace all (asks for the text, then the replacement) |
| Ctrl-T | go to line |
| Ctrl-C, Ctrl-K, Ctrl-V | copy the line, cut the line, paste |
| Ctrl-L | line numbers on/off |

The clipboard is an ordinary file called **`.clipboard`** — so it survives between
sessions and you can `cat .clipboard`. Control characters in a file are *shown*
as `.` (they are kept if you save). The editor keeps its buffers in its own
heap; nothing it does can touch another program.

---

## 5. Pipes and redirection

```
cmd > file          send output to a file (replacing it)
cmd >> file         append
cmd < file          give the command a file as its input
a | b | c           feed a's output to b, and b's to c   (up to 3 stages)
```

How it works, and its limits: each stage's output is redirected into an object;
the next stage is handed that object as its last operand; hidden `.pipe0`,
`.pipe1` objects are deleted afterwards. Stages run one after another, so all
data must fit an object (12 KB). There is **no quoting**: `|`, `>`, `<` always
mean operators. Builtins usable in a pipeline: `echo`, `pwd`, `date`; everything
else must be a program. Redirecting into a file needs write authority over it:
a read-only or system file refuses (`redirect: refused`).

```
echo hello world > a.txt          create it
seq 30 | tail 3                   29 30 (and 28)
cat log.txt | sort -r | head 5
grep error < log.txt
```

---

## 6. Programs (all are separately loaded, start with no authority)

The shell hands each one exactly the capabilities that command needs (read on
*that file*, delete on *that file*, …) and nothing else — which is why
`cat payload.bin` says "permission denied": the shell holds no authority over a
system object, so it has none to give.

**Reading and text**

| Command | Does |
|---|---|
| `cat f` | print a file |
| `more f` | a page at a time (space/Enter: next, q: quit) |
| `head [N] f`, `tail [N] f` | first / last N lines (default 10) |
| `wc f` | lines, words, bytes |
| `sort [-r] f`, `uniq f`, `tac f`, `rev f`, `nl f` | sort, drop adjacent duplicates, reverse line order, reverse each line, number lines |
| `grep <text> f` | lines containing text (text may contain spaces) |
| `hexdump f`, `strings f`, `cksum f`, `file f` | hex view, printable runs, CRC-32, what kind of file |
| `cmp a b`, `diff a b` | identical? / lines that differ (line by line, not a minimal edit script) |

**Files**

| Command | Does |
|---|---|
| `ls [dir]`, `tree [dir]` | list, recursive list |
| `cp a b`, `mv a b`, `rm f`, `touch f` | copy, rename/move, delete, create or refresh the time |
| `protect f`, `unprotect f` | read-only guard on/off (an accident guard: anyone with write authority can undo it) |
| `stat f`, `find text`, `du` | metadata, names containing text, how full the store is |

**System and small helpers**

| Command | Does |
|---|---|
| `ps` | your background jobs (see below) |
| `uptime`, `cores` | time since boot; each CPU core and what it is running |
| `seq [a] b`, `sleep n`, `expr a op b`, `cal` | numbers, wait, integer arithmetic (`+ - x / %`; write multiply as `x`), this month's calendar |

`ps` shows only **your own descendants** (visibility is a capability); there is
no global process table to read.

**Packages** — `pkg list`, `pkg install <name>`: the package is *staged untrusted*,
scanned in a sandbox, then promoted to trusted or rejected for good. Try
`pkg install greeter` then `run greeter`, and `pkg install trojan` (rejected).

---

## 7. Getting files in and out

**Into Vajra from Windows/Linux** (QEMU stopped; edits `build/disk.img`):

```
tools/vajrafs.ps1 -Put C:\docs\report.txt -As docs/report.txt
tools/vajrafs.ps1 -List
tools/vajrafs.ps1 -Get docs/report.txt -Out C:\temp\report.txt
tools/vajrafs.ps1 -Remove docs/report.txt
```

Imported files are ordinary untrusted user files. `-As docs/report.txt` stores the
path in the name; to `cd docs` you also need the directory marker, so `mkdir docs`
inside Vajra first (the tool does not create markers).
Limits: 12288 bytes, 39-character names, 96 objects. **Note**: `tools/build-c.ps1`
rebuilds `disk.img` from scratch, which discards everything you stored.

**Paste text, or script a session, over the serial line.** Bytes sent to COM1 are
typed as keystrokes into the focused window. With QEMU's `-serial tcp:…` or
`-serial unix:…` a script can log in, run commands, and read the answers — how the
project's own tests drive it. (Ctrl codes work: byte 0x13 is Ctrl-S.)

---

## 8. What "permission denied" means

Vajra has no users and no root. A program can do only what it was handed:

* The **shell** is *your* agent. It holds authority over your files (the *user
  domain*) and can run the built-in programs.
* For each command it delegates the minimum — usually one capability naming one
  file — to a brand-new program, then waits for it.
* A **system** object is outside your authority, so nothing the shell starts can
  be given authority over it. That is a refusal by the kernel, not by the tool.
* Files you create are **untrusted** and can never be run as programs; only the
  installer (after inspection) or the kernel (for the built-ins) can make a
  program trusted.
* `cat` and the editor print control bytes as `.`, so a file cannot rewrite your
  screen.
* Programs cannot run code from their own stack or heap (no-execute), and each
  has a small, capped amount of memory.

The **Security Lab** (F5) lets you try to break these rules: keys 1–9, `b`, `a`.

---

## 9. Limits and known gaps

* Files ≤ 12 KB, names ≤ 39 characters (path included), 96 objects, no more than
  two programs loaded at once, three-stage pipelines, six-line history.
* No tab completion, no scrollback, no shell scripting, no environment variables,
  no aliases; `sort` handles ≤ 500 lines; `diff` is line-by-line.
* Only one window is visible at a time.
* `kill`, `stop`, `count`, `pipe` are builtins, not programs.
* Timestamps come from the CMOS clock (UTC) and are second-granular; there is no
  way yet to *set* the clock.
* No networking beyond the Fabric app (Ethernet-level actor messaging).

---

## 10. For developers

* **Test loop**: `python sess.py "cmd" "cmd" …` style drivers boot QEMU, wait for
  the prompt, focus the shell (F2 via the monitor) and type over the serial line,
  waiting for the prompt between commands — a whole session takes seconds.
* **Ring-3 rules** (kernel-linked actors and loaded programs): never dereference a
  string *literal* in a kernel-linked actor (compare characters); a console write
  is ≤ 511 characters; no writable globals in loaded programs (W^X) — use the
  stack or `SYS_HEAP_GROW`.
* **On-disk format** (directory v3): magic `VDR3` at LBA 400, 13 sectors of 64-byte
  entries `[in_use, trust, user, flags, size, created, modified, name[40], …]`,
  object data at LBA 420 + id·24 (24 sectors = 12 KB each). An older disk reads as
  blank.
* **Boot image**: the boot loader reads `KERNEL_CHUNKS × KERNEL_CHUNK_SECTORS`
  sectors (currently 6 × 64 = 384 = 196,608 bytes); the build refuses a kernel
  larger than that and refuses to build if `KERNEL_SECTORS` and the chunk
  constants disagree.
