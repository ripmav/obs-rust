# Conformance suite

Executable definition of the behaviour specified in `docs/rust-rewrite/specs/`.
A *script* (input) drives an area through a tiny command language; the committed
*vector* (`vectors/<area>/<name>.expected`) is what the C reference printed for it.
A conforming implementation implements its own runner for the same language and
produces byte-identical output, subject to the comparison rules below.

```
scripts/<area>/<name>.script      input, hand-written
vectors/<area>/<name>.expected    output of the C reference runner (authoritative)
gen/<area>_runner.c               reference runner (links the C implementation)
run.sh generate | check           (re)generate or verify vectors; needs a C compiler
```

Areas: `bmem` (S-001), `calldata` (S-002), `decl` (S-003), `signal` (S-004),
`darray` and `deque` (S-005).

**Who uses what.** Spec authors use `run.sh` and `gen/`. Implementers of a clean-room
port need only `scripts/`, `vectors/`, this README, and the spec; they write a runner
in their own language (50–150 lines per area) and compare output.

## Assumptions of the committed vectors

- 64-bit little-endian target: `size_t` is 8 bytes, pointers are 8 bytes.
  Byte-exact dumps (`dump bytes=…`) depend on this. Implementations for other targets
  parameterise the width (spec S-002 gives the formulas) and must regenerate vectors
  from the C reference on that target.
- POSIX build of the reference (Linux). Where Windows or macOS differ, the spec says so.
- Output is line-oriented text, `\n` line endings. Files are marked `-text` in
  `.gitattributes` so that Windows checkouts keep them byte-exact.

## Comparison rules (the conformance procedure)

For each script, run your runner and compare its stdout with the `.expected` file,
line by line:

1. **Normal lines** must be byte-identical.
2. **Log lines** (`log <level> <message>`): the first two words must match (the level);
   the message text is informational and must not be compared. Exception: a message
   marked "MUST" in the spec.
3. **`crash` results.** In `decl`, a result line `parse crash` means *the reference
   implementation crashed on this input*. A conforming implementation MUST NOT crash;
   it MUST print `parse fail` instead. The comparison treats `parse crash` in the
   expected file as equal to `parse fail` in your output, and **ignores any `log`
   lines your runner prints for that input** (the reference died before it could
   log; a conforming implementation logs normally). These cases are listed in
   the spec as DEFECT cases.
4. **Conformance classes.** Each script belongs to a class stated in the spec:
   - **CORE** – must match.
   - **QUIRK** – behaviour of the reference that is arguably accidental
     (e.g. tolerated trailing garbage). Should match (the spec says SHOULD);
     a deviation must be documented in the implementation's `DEVIATIONS.md` with
     a reason.
   - **DEFECT** – reference crashes or invokes undefined behaviour; required
     behaviour is described in the spec.
5. Any runner exit status other than 0, or any stderr output, is a failure.

Scripts never depend on pointer values, time, thread scheduling or uninitialised
memory. If you find one that does, that is a bug in the suite: file a spec question.

## Script language (common)

- One command per line. Tokens are separated by spaces/tabs.
- `#` starts a comment (at the start of a token).
- A token may be double-quoted; inside quotes `\n \t \r \\ \" \xHH` are escapes and
  spaces are literal. Quotes only delimit; they carry no other meaning.
- `@null` means NULL (where the command accepts a pointer); `-` means "empty"
  where a hex string is expected.
- Hex strings are lower- or upper-case pairs of digits, no separators.
- Integers: decimal, or `0x` hex (`strtoll(…, 0)`/`strtoull(…, 0)` rules).

Strings printed by the runners use the same escaping: `\\`, `\"`, `\n`, `\t`, `\r`,
and `\xHH` for every other byte < 0x20 or ≥ 0x7f; printing stops at the first NUL.

## `calldata` runner (spec S-002)

State: one calldata `cd` (all-zero before `init`).

| Command | Effect | Output |
|---|---|---|
| `init` | initialise as an empty, growable calldata | – |
| `init_fixed N` | fixed-size calldata over an N-byte caller buffer | – |
| `clear` / `free` | clear / free (free also resets the object to all-zero) | – |
| `set_int NAME V` | 64-bit signed integer | – |
| `set_float NAME V` | 64-bit IEEE double (`strtod`) | – |
| `set_bool NAME 0/1` | one byte, 0 or 1 | – |
| `set_ptr NAME V` | pointer-sized value | – |
| `set_string NAME S` | NUL-terminated string; `@null` = NULL string | – |
| `set_data NAME HEX` | raw payload; `-` = empty payload | – |
| `get_int/get_float/get_bool/get_ptr NAME` | typed read | `get_x NAME ok V` or `get_x NAME fail`; float printed `%.17g`, pointer `0x%x` |
| `get_string NAME` | | `get_string NAME ok "…"`, `ok @null`, or `fail` |
| `get_data NAME N` | read exactly N bytes | `get_data NAME N ok HEX` / `fail` (`-` for N = 0) |
| `val_int NAME` / `val_string NAME` | "call if you know your data is valid" forms | `val_int NAME V`, `val_string NAME "…"`/`@null` |
| `dump` | the live stack bytes | `dump size=S bytes=HEX` (`-` if none) |
| `capacity` | | `capacity C` |
| `echo T` | | `echo T` |

## `decl` runner (spec S-003)

`parse "<declaration>"` prints `decl "<input>"`, then either
`parse ok name=N params=K` followed by K lines
`  param I name=N type=T in=0|1 out=0|1`, or `parse fail`, or `parse crash`
(reference only). Log lines may appear before the result line.

## `signal` runner (spec S-004)

See the header comment of `gen/signal_runner.c` for the complete command list:
`handler`, `destroy`, `add`, `callback`, `gcallback`, `connect`, `connect_ref`,
`disconnect`, `connect_global`, `disconnect_global`, `emit`, `remove_current`,
`procs`, `proc_add`, `proc_call`. Scripted callbacks print a trace line when
invoked (`call <label> [n=V]`, `gcall <label> signal=<sig> [n=V]`,
`pcall <label> [n=V]`) and then run their *actions* in order:

| Action | Effect |
|---|---|
| `remove_current` | call `signal_handler_remove_current()` |
| `disconnect:H:SIG:CB` / `connect:H:SIG:CB` / `connect_ref:H:SIG:CB` | change connections of callback `CB` |
| `connect_global:H:GCB` / `disconnect_global:H:GCB` | change global connections |
| `emit:H:SIG` | emit again with the same parameter block (nesting is limited to depth 6 by the runner and prints `nested-emit SIG`) |
| `inc:NAME` | add 1 to int parameter NAME in the parameter block (missing counts as 0) |
| `ret:V` | set int parameter `return` |

`emit H SIG name=int…` creates a parameter block of int parameters, signals, prints
`emit SIG` first and `emit-done [n=V]` last. Callback identity for connect/disconnect is
the pair (callback function, user data); a scripted callback label is one such pair.
`proc_add P DECL LABEL [action…]` prints `proc_add <decl>`; `proc_call P NAME [k=v…]`
prints `proc_call NAME ok [return=V]` or `proc_call NAME fail`.

## `darray` runner (spec S-005)

Element type is 32-bit signed int. `b:` prefix selects the second array.
Commands: `init free clear reserve N ensure N resize N push V push_new
push_array V… insert I V insert_new I insert_array I V… erase I erase_item V
erase_range S E find V FROM copy_from_other move_from_other
push_darray_from_other dump`. `dump` prints `a|b dump num=N capacity=C items=…`.
Preconditions that `assert()` in the reference (indices in range, non-empty arrays)
are never violated by scripts.

## `deque` runner (spec S-005)

Commands: `init free reserve N push_back HEX push_front HEX push_back_zero N
push_front_zero N place POS HEX upsize N peek_front N peek_back N pop_front N
pop_back N pop_front_discard N pop_back_discard N dump`. `dump` prints
`size= capacity= start= end= data=` where `data` lists every byte of the backing
buffer as two hex digits, using `..` for bytes outside the live ring (undefined).

## `bmem` runner (spec S-001)

Commands: `alloc zalloc realloc free fill check dup strdup strdup_n aligned count
alignment log fatal`. `count` prints the change of `bnum_allocs()` since the start of
the script. `fatal <kind>` runs a zero-size allocation in a child process whose crash
handler prints `crash <message>` and exits with status 77; the parent prints
`fatal <kind> exit=77`.

## `mediaconv` runner (spec S-013)

Pure parts of `libobs/media-io`: pixel-format converters, video frame layout, YUV matrices,
fourcc, audio/video enum helpers, audio time maths, frame rates. Integers follow the common
rules (decimal or `0x`); enum arguments are given as their **numeric value** (spec section 3).
Hex arguments are byte strings; `-` is the empty string. Plane output is printed as hex rows of
`linesize` bytes separated by single spaces. Floats print with `%.9g` (doubles `%.17g`);
`nan`, `inf`, `-inf` print literally. Compare floats with the tolerances of spec section 2.

Any command may be prefixed with `forked`: the reference runs it in a child process and the
runner prints `result crash` if the child died or exited non-zero (zero-size allocation, division
by zero). A conforming implementation's runner prints `result crash` for such a command when the
library call returns an error (spec D-1, D-3) and must print nothing else for it.

| Command | Prints |
|---|---|
| `enums` / `layouts` | one `NAME=value` per line: every enumerator and constant, then `sizeof(...)` / `offsetof(...)` of the plain structs (reference list in `scripts/mediaconv/01-enums-layouts.script` output) |
| `vinfo FMT` | `vinfo F name=<get_video_format_name> yuv=<0/1> default_range=<Full/Partial> resolve(default)=N resolve(partial)=N resolve(full)=N` |
| `rangename FMT RANGE` | `rangename fmt=F range=R: resolved=N name=<Full/Partial>` |
| `csname CS` | `csname CS=<get_video_colorspace_name>` |
| `fourcc TOKEN` | `fourcc 0x%08x -> N (<format name>)`; TOKEN is four characters (little-endian packed, first char = lowest byte) or `0xHEX` |
| `linesizes FMT W` / `heights FMT H` | `linesizes fmt=F w=W: a b c d e f g h` (8 values, 0 for unused planes) / `heights fmt=F h=H: ...` |
| `frame_init ID FMT W H` | initialises frame slot ID (0..7); prints `frame ID aligned32=<0/1/-1> linesize: x8 offsets: x8` where offsets are `data[i]-data[0]` or `-` for NULL planes; aligned32 is `-1` if `data[0]` is NULL |
| `frame_fill ID FMT H const V` / `seq V` | fills every plane (`linesize*height` bytes) with byte V, or with byte `(V + plane*64 + index) mod 256` |
| `frame_dump ID FMT H` | per plane: `frame ID plane P ls=L rows=R: <rows>` |
| `frame_copy DST SRC FMT H` | calls `video_frame_copy`, prints `frame_copy done` |
| `frame_free ID` | frees the slot; prints `frame_free zeroed=1` then `null-frame ok` (free/init of a NULL frame pointer are no-ops) |
| `compress FN START END ILS OLS0 OLS1 OLS2 H0 H1 H2 INHEX [OFF]` | FN = `i420`, `nv12`, `i444`. Output plane i is `OLSi*Hi` bytes pre-filled with `ee` (plane absent if Hi = 0). The input buffer is the hex string followed by zero bytes, placed at byte offset OFF (default 0) from a 32-byte aligned address. Prints `compress FN` then per present plane `  plane P ls=L rows=R guard=<intact/overrun>: <rows>` (guard = 64 bytes after the plane were not modified; a conforming implementation always prints `intact` for in-contract calls) |
| `decompress FN START END ILS0 ILS1 ILS2 OLS OH P0 P1 P2 [OFF]` | FN = `420` or `nv12`; the three plane hex strings are copied to separate buffers at offset OFF; output `OLS*OH` bytes pre-filled with `ee`; prints `decompress FN ols=L rows=R guard=...: <rows>` |
| `decompress422 lead\|trail START END ILS OLS OH INHEX [OFF]` | same for `decompress_422` (`lead` = leading_lum true); prints `decompress422 V ols=L rows=R guard=...: <rows>` |
| `vparams CS RANGE FMT\|- [nomin] [nomax]` | `-` calls `video_format_get_parameters`, a format number calls `..._for_format`; all outputs are pre-set to the sentinel `7`, `nomin`/`nomax` pass NULL; prints `vparams cs=C range=R fmt=F ok=<0/1>`, four lines `  rowN a b c d` (the 16 matrix floats row-major), `  min a b c`, `  max a b c` |
| `ainfo FMT SPK FRAMES` | `get_audio_channels/bytes_per_channel/is_audio_planar/get_audio_planes/get_audio_size/get_total_audio_size` |
| `frames_to_ns RATE N` / `ns_to_frames RATE N` | decimal unsigned 64-bit result |
| `mul_to_db X` / `db_to_mul X` | `%.9g` of the 32-bit result |
| `fps NUM DEN` | `fps N/D: interval=<double> fps=<double> valid=<0/1>` |

Unknown commands or malformed lines exit with status 2. Scripts only call the reference with
inputs inside the documented preconditions (spec section 5) except the deliberate `forked`
cases; the `decompress` alignment script (`24`) places planes at unaligned addresses, which
is undefined behaviour in C but fine on x86-64 and in Rust.

## props runner (spec S-015)

Drives `obs_properties` / `obs_property`. Objects are named by the script: *tokens* (opaque
labelled pointers used as param, priv and context data), *behaviors* (scripted callbacks),
*props* containers, *data* settings objects, *obj* context objects and property *handles*.
Names are at most 31 bytes. A property reference `H` is a handle bound with a trailing
`as NAME`, or `P:NAME` (look up `NAME` in container `P` with `obs_properties_get` at use
time), or `@null`. A container reference is a props name or `@null`. Enum arguments are
the lowercase names below or plain integers (printed back as `?N` if out of range).
Enum names: kinds `invalid bool int float text path list color button font editable_list
frame_rate group color_alpha`; combo format `invalid int float string bool`; combo type
`invalid editable list radio`; editable list `strings files files_and_urls`; path `file
file_save directory`; text `default password multiline info`; info `normal warning error`;
number `scroller slider`; group `invalid normal checkable`; button `default url`.
Bool arguments are `0`/`1`. Strings may be `@null`.

| Command | Effect / output |
|---|---|
| `echo T…` | prints `echo T…` |
| `token T` | create token labelled T |
| `behavior B action…` | define a scripted callback behaviour (actions below) |
| `props P [TOKEN [destroy\|nodestroy]]` | `obs_properties_create`, or `create_param(TOKEN, cb)` (cb prints `param-destroy LABEL`; default `destroy`) |
| `destroy P` | `obs_properties_destroy` (a container adopted by a group is destroyed with it, never directly) |
| `set_param P TOKEN\|@null destroy\|nodestroy` / `get_param P` | `set_param`; prints `get_param LABEL\|@null` |
| `flags P V` / `get_flags P` | prints `get_flags N` |
| `first P [as H]` | prints `first NAME\|@null` |
| `get P NAME [as H]` | prints `get NAME found\|null` |
| `next H [as H2]` | `obs_property_next` on a copy; prints `next -> RET NAME\|@null`; binds `H2` (default: rebinds `H`) |
| `parent P` | prints `parent CONTAINERNAME\|@null` (`obs_properties_get_parent`) |
| `remove P NAME` | `remove_by_name` (handles to removed properties become invalid; scripts must not reuse them) |
| `apply P D\|@null` | prints `apply P`, runs `apply_settings`, prints `apply-done` |
| `data D`, `data_set D KEY int\|str\|bool\|dbl V`, `data_show D`, `data_free D` | settings objects; `data_show` prints `data D {k=v,…}` with keys sorted, strings quoted, doubles `%.17g` |
| `obj O [TOKEN]` | a context object whose `data` pointer is TOKEN (or NULL) |
| `add KIND P NAME DESC args… [as H]` | prints `add KIND NAME ok\|null`; `args`: `int/int_slider/float/float_slider` MIN MAX STEP; `text` TYPE; `path` TYPE FILTER DEFAULT; `list` COMBOTYPE FORMAT; `button` BEHAVIOR\|@null; `button2` BEHAVIOR\|@null PRIV-TOKEN\|@null; `editable_list` TYPE FILTER DEFAULT; `group` TYPE G (a props name, or `@null`); `bool color color_alpha font frame_rate` none. A button's behavior runs on click. |
| `set H WHAT args…` | `visible`/`enabled` 0\|1; `description`/`long_description` S; `int_limits`/`float_limits` MIN MAX STEP; `int_suffix`/`float_suffix` S; `text_monospace`/`text_word_wrap` 0\|1; `text_info_type` T; `button_type` T; `button_url` S; `modified` B\|@null; `modified2` B\|@null TOKEN\|@null; `click` B (runner side: attach click behavior to an existing button) |
| `info H` | prints `info` then the property block (below) |
| `dump P` | prints `dump P flags=N param=LABEL\|@null` then every property (groups recursed, 4 more spaces per level) |
| `probe H` | calls every kind-specific getter (index 0 for indexed ones) on any property, 9 `probe …` lines |
| `list_add H FMT NAME VAL` | prints `list_add FMT -> INDEX`; FMT is `string int float bool` |
| `list_insert H FMT IDX NAME VAL`, `list_remove H IDX`, `list_clear H`, `list_disable H IDX 0\|1` | – |
| `list_item H IDX` | prints name, disabled and every typed getter |
| `fr_option_add H NAME DESC`, `fr_range_add H N D N D` | prints `… -> INDEX` or `invalid` |
| `fr_option_insert H IDX NAME DESC`, `fr_range_insert H IDX N D N D`, `fr_clear H`, `fr_options_clear H`, `fr_ranges_clear H` | – |
| `modified H D\|@null` | `obs_property_modified`; prints `modified -> RET` |
| `click H O\|@null` | `obs_property_button_clicked(H, &O)`; prints `click -> RET` |

Property block: `  prop NAME type=T desc=… long=… enabled=E visible=V`, then one line (or several for
list items, frame-rate options and ranges) with the kind details: `min= max= step= numtype= suffix=`
(int/float), `texttype= monospace= infotype= wrap=`, `pathtype= filter= default=`,
`listtype= format= count=` then `item I name= disabled= value=`, `buttontype= url=`,
`listtype= filter= default=` (editable list), `options= ranges=` then `option I name= desc=` and
`range I min=N/D max=N/D`, `grouptype= content=NAME`, or `-`. Strings print quoted or `@null`;
doubles `%.17g`. Property names print raw.

Callback traces (printed when a scripted callback runs, then its actions in order):
`modified B prop=NAME props=CONTAINER settings={…}` (v1), `modified2 B priv=TOKEN prop=… props=…
settings={…}`, `click B prop=NAME props=CONTAINER data=TOKEN|@null`. `CONTAINER` is the name of
the container the callback received; settings print as in `data_show` or `@null`.
Behavior actions (colon separated, no spaces in values): `ret:V` (return value, default 0),
`set_int:K:V set_str:K:V set_bool:K:V` (on the settings), `visible:NAME:V enabled:NAME:V
desc:NAME:TEXT` (look up NAME in the container the callback received), `self_visible:V
self_enabled:V`, `add_bool:NAME` (prints `  action add_bool NAME ok|null`), `remove:NAME` (prints
`  action remove NAME`), `parent` (prints `  action parent NAME|@null`, parent of the received
container), `list_add:PROP:NAME:VAL` / `list_clear:PROP` (string lists).
At the end of the script every container the script still owns is destroyed silently.

## nal runner (spec S-012)

Byte buffers (`DATA`) are given as hex, `-` (empty), `@name` (buffer defined by `buf`/`cat`/`rep`)
or `@null` (NULL, size 0; the vectors treat it as an empty buffer, see spec D-5). Commands:
`buf NAME HEX…` (concatenate), `cat NAME DATA…`, `rep NAME COUNT HEX`, `trail N` (reference
runner only: N readable filler bytes after the input), `echo T`, `layout` (offsets of
`struct encoder_packet`), `find|avc_find DATA [START [END]]` (prints the offset of the returned
pointer), `avc_keyframe|hevc_keyframe|av1_keyframe DATA`, `avc_priority|hevc_priority DATA [PRIO]`,
`avc_packet|hevc_packet DATA [PRIO [KEY]]` (prints `size keyframe priority drop_priority refs
fields fresh data`; `fields=preserved` = all other packet fields copied unchanged, `fresh=1` = a new
buffer, `refs=1` = the reference counter stored in the 8 bytes before `data`), `avc_header DATA`
(`ret=N data=HEX`), `avc_extract|hevc_extract DATA` (`new= header= sei=`), `av1_extract DATA`
(`new= header=`), `av1_metadata DATA TYPE`, `av1_metadata_t35 DATA` (`size= data=`), and the two
helper probes `av1_obu DATA` (`start= size= type=`) and `av1_leb128 DATA` (`value= len=`).
`-` is printed for empty hex. Defect inputs (spec D-1..D-3, D-5) print the REQUIRED safe result,
not the reference's crash; there are no `crash` lines in these vectors. Scripts `16-ub-unspecified`
record x86-64 reference behaviour for undefined behaviour: only "must not crash, same output
length" is required there.

## math2 runner (spec S-009)

Covers vec2, matrix3, plane, bounds, math-extra, half, srgb, math-defs (reference: libobs graphics).
Typed variables by name; a `call` whose destination already exists with the same type keeps its
previous contents (so lanes/fields a function does not write are visible); new variables start zeroed;
using one name as source and destination aliases them.
Declarations: `float N v`, `vec2 N x y`, `vec3 N x y z [w]`, `quat N x y z w`, `aa N x y z angle`,
`mat3 N <16 floats>` / `mat4 N <16 floats>` (rows x,y,z,t as listed, 4 lanes each),
`plane N dx dy dz dist [dir_w]`, `bounds N minx miny minz maxx maxy maxz`, `f32bits N 0xHEX` (float from
bits), `half_bits N 0xHHHH`, `px N HEXBYTES` (one token, up to 64 bytes), `f4 N a b c d`.
Commands: `show N`; `call D fn args…` (args are variable names or number literals; numbers accept
`inf`, `nan`, hex floats such as `0x1p-24`); `rand_check POSITIVE N` and `vec3_rand_check POSITIVE N`
(print only range/variation checks, never values).
Functions are the C names (`vec2_add`, `matrix3_mul`, `plane_from_tri`, `bounds_transform`,
`polar_to_cart`, `calc_torque`, …); `half_from_float D literal` / `half_from_float_var D floatvar`;
`srgb_nonlinear_to_linear`, `srgb_linear_to_nonlinear`, `u8_to_float`, `float_to_u8`,
`u8x4_to_float4`, `float4_to_u8x4`, `float3_to_u8x3`, `premultiply_float4`,
`premultiply_xyza[_srgb][_restrict][_loop]` (`premultiply_xyza_* D SRC [count]`; in-place forms copy SRC
into D first, restrict forms write into the existing D), `const D NAME`, `RAD`, `DEG`, `close_float`.
Output lines: `N float|bool|vec2|vec3|quat|aa|mat3|mat4|plane|bounds|half|u8|f4|px …`; plane prints
`dir.x dir.y dir.z dir.w dist`; bounds prints `min(4 lanes) max(4 lanes)`; functions with an out
parameter print `N hit <0|1> <t>` where `t` was preset to -7777; `?` marks a lane the reference leaves
uninitialised; NaN prints `nan`. Comparison: floats relative-or-absolute 1e-6 (1e-5 after
sin/cos/asin/atan2/powf), `-0 == 0`, NaN equals NaN; bool/half/u8/px exact except `px` results of
`premultiply_xyza_srgb*` (±1 per byte).

## serial runner (spec S-010)

Covers the serializer interface and its helpers, the array/file/buffered-file serializers,
the bit reader and the text lookup. Files are created in `$CONFORMANCE_TMPDIR` and addressed
by plain names (the runner uses `<dir>/serial-<name>`; scripts never print paths). A file
that is open for writing is never inspected before `close` (stdio buffering is not
specified). Integers follow `strtoull(…, 0)` (avoid leading zeros); float/double arguments
are decimal text, or `bits:0x…` for an exact IEEE bit pattern (NaN payloads, -0).
`hex` arguments use the common conventions (`-` = empty).

**Serializer slots.** Four slots (`slot N`, default 0); every serializer command acts on the
current slot. `open KIND …` closes whatever the slot held and prints `open KIND ok|fail`.

| Command | Effect / output |
|---|---|
| `open array` | array serializer |
| `open fin NAME` / `open fout NAME` | file input / output serializer |
| `open fsafe NAME EXT` | `file_output_serializer_init_safe` (`@null` = NULL ext) |
| `open buf NAME [MAXBUF CHUNK]` | buffered file serializer (defaults when sizes are omitted) |
| `open custom FLAGS` | scripted double; flags `r w s p` choose which callbacks exist. write appends to a log and returns size; read returns `min(size,3)` bytes `01 02 03 …` (counter keeps running); seek returns `offset*10+mode` (start 0, cur 1, end 2); get_pos returns 777 |
| `open null` / `open empty` | a NULL serializer pointer / a serializer with all callbacks NULL |
| `close` | the matching `*_free` (array: free) |
| `w8 wl16 wl24 wl32 wl64 wlf wld wb16 wb24 wb32 wb64 wbf wbd V` | the header-inline helpers (value truncated by the C parameter type) |
| `write HEX [SIZE]` / `write @null N` | `s_write`; prints `write HEX N -> R` (`-` = zero-length non-NULL buffer; SIZE overrides the length) |
| `write_pat N [START]` | `s_write` of N generated bytes, byte i = `((START+i)*31+7) mod 256` |
| `serialize_w HEX [SIZE]` | `serialize` with that buffer |
| `read N` / `read @null N`, `serialize_r N` | `s_read` / `serialize` on a zero-filled N-byte buffer. Prints `… -> R data=HEX` where HEX are the first `min(R,N)` bytes of the buffer (so `serialize_r` on a write-capable serializer shows the zeros it wrote) |
| `seek start\|cur\|end OFF` | `serializer_seek`; prints `seek MODE OFF -> R` |
| `pos` | `serializer_get_pos`; prints `pos -> R` |
| `dump` | array: `dump array num=N pos=P bytes=HEX` (N = `bytes.num`, P = `cur_pos`); custom: `dump custom log=HEX` |
| `array_reset` | `array_output_serializer_reset` |
| `fput NAME HEX`, `fputs NAME "text"` | create/overwrite a file with bytes / the string's bytes |
| `fget NAME` | `file NAME size=N bytes=HEX` or `file NAME missing` |
| `fsum NAME` | `file NAME size=N fnv=<FNV-1a 64 hex>` |
| `funlink NAME` | delete (no output) |
| `echo …` | prints its arguments |

**Bit reader.** One reader: `bs_init HEX [LEN]` (LEN defaults to the byte count; may be smaller),
`bs_init_gen N [LEN]` (N bytes, byte i = `(i*31+7) mod 256`), `bs_bits N`, `bs_bit`, `bs_r8`,
`bs_r16`, `bs_skip N` (N silent r8 calls), `bs_state`. Output: `bs_bits N -> 0xHH`,
`bs_bit -> B`, `bs_r8 -> 0xHH`, `bs_r16 -> 0xHHHH`,
`bs_state pos=P sub=0xHH len=L` (the struct fields `pos`, `subPos`, `len`).

**Text lookup.** Four lookup slots: `tl_create I NAME` (`-> ok|null`; a previous lookup in the slot
is destroyed), `tl_add I NAME` (`-> true|false`; the slot must hold a lookup),
`tl_get I KEY` (`tl_get I "KEY" -> "value"` or `-> false`; works on an empty slot, where it
is false), `tl_destroy I`.
`tl_defect I NAME EQUIV` marks an input on which the reference has undefined behaviour
(spec D-1). The **reference** runner does not load NAME: it loads EQUIV into slot I (a file
whose *required* outcome equals the required outcome for NAME) and prints
`tl_defect I NAME -> ub`. A **conforming** runner loads NAME itself into slot I, must not
crash, and prints the same line; the following `tl_get` lines then verify the required
behaviour.

Debug log lines `log debug Final buffer capacity …` occur once per `close` of a buffered
serializer (level comparison only).

## lexer runner (spec S-011)

Area `lexer`: base lexer, C-like token layer with preprocessor, and the parser helper API.
State: one base lexer `L`, one error list `ED`, one parser `P` (lexer + preprocessor + error list + cursor).
`@null` = NULL; optional trailing arguments are omitted. All text is printed with the common quoting rules.
Files: `@tmp` at the start of a file argument means `$CONFORMANCE_TMPDIR`; printed file names are rewritten back to `@tmp…`.

**Base lexer.** `lstart TEXT|@null`, `lstart_move TEXT`, `lreset`, `lfree`.
`lnext [ignore]` / `lall [ignore]` (all remaining tokens) call `lexer_getbasetoken` (`ignore` = IGNORE_WHITESPACE) and print
`base I type=ALPHA|DIGIT|WHITESPACE|OTHER off=O len=L text="…" pws=0` or `base none`, each followed by `  offset=N|null`
(offset of the lexer after the call; `I` counts tokens printed by this command). `stroffset TEXT IDX|@null` prints
`stroffset row=R col=C` (999 = outputs untouched). `newline_size TEXT`, `chars TEXT` (`char HH ws= nl= st=`),
`is_newline_pair A B`. `cmp|cmpi TEXT LEN STR|@null`, `cmp_strref|cmpi_strref T1 L1 T2 L2` print the sign (-1/0/1) of the result;
`is_empty TEXT|@null LEN`, `is_empty_null`, `ref_add TEXT OFF1|@null LEN1 OFF2 LEN2` (prints resulting `off` and `len`),
`valid_int|valid_float TEXT|@null N`.
`ed_add FILE ROW COL LEVEL MSG`, `ed_add_null`, `ed_dump` (errors + `buildstring`), `ed_free` operate on `ED`.

**C-like layer.** `reset` frees and re-initialises `P` (required before a second `lex`/`parse`). `lex TEXT [FILE]` runs only the
cf lexer: prints `lex true|false tokens=N unexpected_eof=0|1` and `reformatted "…"`. `parse TEXT [FILE]` = `cf_parser_parse`
(lex + preprocess; cursor on first output token): prints `parse true|false`. `pp` runs the preprocessor on an already lexed `P`.
`tokens lex|pp` prints `tokens KIND N` then `tok I type=T str="…" src=OFF+LEN @ROW:COL [u="…"] [f="file"]` for every token
(incl. the final NONE token): `str` is the token text, `src` the span in the source text of the lexer that owns it
(offset + length, length may exceed the text, see spec), `@row:col` its position, `u` the unmerged source text, printed only
when it differs from `str`, `f` the file name of an included/API-defined token (not the main text). `src=-` means the token
was synthesised (blank inserted by the preprocessor). `errors` prints `errors N (errors=E warnings=W has_errors=B)` and
`err I LEVEL file=… row= col= msg="…"`; `errstr` prints the `error_data_buildstring` text.
`def NAME [BODY]` adds an API definition (`cf_preprocessor_add_def`; BODY is lexed, a terminating NONE token is appended;
no BODY = empty token list), `undef NAME`, `writefile NAME TEXT` creates `$CONFORMANCE_TMPDIR/NAME`.
`literal TEXT [COUNT]` prints `cf_literal_to_str`.

**Parser helpers** operate on the cursor (index into the preprocessed tokens); after most commands the runner prints
`  at #IDX <token>`. `cur`, `setcur IDX`, `next`, `next_valid`, `peek`, `peek_valid` (print the boolean result),
`is STR`, `is_type TYPE EXPECTED [GOTO]`, `should_be STR [G1 [G2]]`, `next_should_be …` (print the PARSE_* code name),
`goto S1 [S2]`, `goto_valid`, `goto_type TYPE`, `pass_pair IN OUT`, `get_name|next_name EXPECTED [GOTO]`,
`get_name_ref|next_name_ref …`, `copy`, `next_copy`, `adderror MSG LEVEL [V1 [V2 [V3]]]`, `expecting S`, `eof_error`,
`syntax_error`. TYPE is `NONE NAME NUM SPACETAB NEWLINE STRING OTHER`.

**Crash capture.** `try CMD args ; CMD args …` runs the commands in a forked child (state changes are discarded) and prints
`try-begin`, the child's output, and `try-end ok` or `try-end crash`. Where the expected file says `try-end crash` the reference
died; a conforming implementation MUST NOT crash, and everything printed between that `try-begin` and `try-end` is ignored
(print `try-end ok`, which compares equal).

## obsdata runner (spec S-014)

Reference: libobs `obs-data.c` with jansson 2.15 for JSON; `os_quick_*` file helpers in
`gen/obsdata_stubs.c`. Files used by scripts live in `$CONFORMANCE_TMPDIR`; script file names
are relative names (`conf.json`) that the runner maps into that directory (an implementation
may use any directory; the runner removes its files before and after a script).

**Handles.** Three separate namespaces of script-chosen names: *data* (`obs_data_t`), *arrays*
(`obs_data_array_t`), *items* (`obs_data_item_t`). Creating commands bind a name to an empty slot
(binding a name that already holds a value is a script error). Every slot **owns exactly one
reference**; `release`, `a_release`, `item_release` drop it and free the name. `@null` in a handle
position means NULL. At the end of a script the runner releases everything still held (so a
reference-counting implementation must not leak or double free). Output of commands that produce a
handle prints `ok` or `null` (and binds nothing when `null`).

Number arguments: decimal or `0x` integers (`strtoll`), doubles by `strtod` (`nan`, `inf` accepted).
Booleans: `0`/non-zero. Strings may be `@null`. Doubles are printed `%.17g`, floats `%.9g`.

| Command | Effect / output |
|---|---|
| `new D` / `release D` | create / drop an object (`obs_data_create`, `obs_data_release`) |
| `addref D E` | `obs_data_addref(D)`; E becomes a second handle to the same object |
| `newref D E` | `E = obs_data_newref(D)` (D may be `@null` -> a fresh object) |
| `refs D` | `refs D N` the reference count; `info_refs D` is the same but informational (below); arrays: `a_refs A` / `info_a_refs A` |
| `same D E` | `same D E 0/1` pointer identity of the two objects |
| `from_json D TEXT` | `from_json D ok/fail` (+ binds D on ok); `TEXT` may be `@null` |
| `from_json_deep D obj/arr N` | text of N nested levels (`{"a":{..1..}}` or `[[..1..]]`) |
| `from_json_file D FILE` / `from_json_file_safe D FILE BAKEXT` | `... D ok/fail` |
| `json` / `json_defaults` / `json_pretty` / `json_pretty_defaults` / `last_json` `D` | `NAME D "text"` or `NAME D @null` (`obs_data_get_json`, `..._with_defaults`, `..._pretty`, `..._pretty_with_defaults`, `get_last_json`) |
| `save_json D FILE` | `save_json 0/1` |
| `save_json_safe` / `save_json_pretty_safe D FILE TMPEXT BAKEXT` | `NAME 0/1` |
| `write_file NAME TEXT` / `read_file NAME` / `exists NAME` / `remove_file NAME` | helper file access; `file NAME "content"`, `file NAME nofile`, `exists NAME 0/1` |
| `apply T S`, `erase D NAME`, `clear D` | as the C functions |
| `get_defaults D E` | `E = obs_data_get_defaults(D)` |
| `[item_]set_[default_/autoselect_]KIND D NAME VALUE` | KIND = `string int double bool obj array` (obj/array VALUE is a handle or `@null`); `vec2 x y`, `vec3 x y z`, `vec4 x y z w`, `quat x y z w`; `frames_per_second NUM DEN OPTION` (OPTION string or `@null`). Item form: `item_set_KIND I VALUE` (no NAME; the handle follows the item if it moves) |
| `[item_]get_[default_/autoselect_]KIND D NAME` | prints `CMD D NAME VALUE`: string `"..."`, int, double `%.17g`, bool `0/1`; for `obj`/`array` the form is `get_obj D NAME E` printing `ok`/`null` and binding E; vec/quat print the floats after presetting every component to `7`; `get_frames_per_second D NAME [opt]` prints `RESULT NUM DEN OPTION` (preset `7 7`; with the word `opt` the option pointer is passed and printed `"<unset>"` if untouched; without it prints `-`). Item forms: `item_get_KIND I` |
| `has_user/has_default/has_autoselect D NAME` | `CMD D NAME 0/1`; item form `item_has_user I` |
| `unset_user/unset_default/unset_autoselect D NAME` | item form `item_unset_user I` |
| `dump D` | canonical state dump: `{name:type[user=V default=V auto=V],...}` in iteration order; V = `"str"`, `num(T,INT,DBL)` (T = `I`/`D` number type of the *effective* layer, `?` for a non-effective layer; INT is `~` if the double is outside +-9e18), `true/false`, `null` (NULL object/array), `{...}`, `[{...},...]` |
| `names D` / `iter D` | item names (resp. `name:type`) in iteration order |
| `first D I` / `byname D NAME I` | `first D ok/null` / `byname D NAME ok/null`; binds item handle I |
| `next I` | `next I 0/1 [NAME]`; on 0 the slot becomes empty |
| `next_null` | `obs_data_item_next` on a NULL item and a NULL pointer: `next_null 0`, `next_nullptr 0` |
| `item_release I` / `item_remove I` | drop the reference / `obs_data_item_remove` |
| `item_refs I`, `item_type I` (`string number bool object array null`), `item_numtype I` (`int double invalid`), `item_name I`, `item_dump I` | print `NAME I value` |
| `a_new A` `a_release A` `a_addref A B` `a_refs A` `a_count A` | array handles and counts (`@null` allowed for count) |
| `a_item A IDX D` | `a_item A IDX ok/null`; binds D |
| `a_push A D` | prints the index returned (`a_push A N`); `a_insert A IDX D`, `a_push_array A B`, `a_erase A IDX` |
| `a_enum A` | prints `enum I {dump}` per element then `a_enum A done N`; `a_enum_nullcb A` passes a NULL callback |
| `a_dump A` | `a_dump A [{...},...]` |
| `try CMD ARGS ; CMD ARGS ...` / `try_quiet ...` | runs the commands in a forked child process; prints their output (nothing for `try_quiet`) then `try CMD ok` (child exited 0), `try CMD crash` (killed by a signal) or `try CMD exit-N` |
| `echo TEXT...` | prints `echo TEXT...` |

**Comparison rules specific to this area.**

1. Log lines: level compared, message text ignored (jansson's error wording differs between versions).
2. Lines starting with `info_` (`info_refs`, `info_a_refs`) are informational: they show what the *reference* does in a defect
   case (S-014 D1) and are **not compared**.
3. `try X crash` in an expected file means the *reference* crashed (S-014 D2); a conforming
   implementation prints `try X ok` instead and the two are treated as equal. Output lines printed by
   a child before it crashed are compared.
4. Doubles are compared as exact strings (`%.17g`, no tolerance); floats `%.9g` exact.

## platform runner (spec S-023)

Reference: `libobs/util/utf8.c`, `platform.c` and the Linux half of `platform-nix.c` (real code, linked with
`!nostubs`; `gen/platform_stubs.c` only supplies `obs_get_video_info()` and a controllable `time()`).
The runner is `gen/platform_runner.c`; scripts are in `scripts/platform/`.

**Work directory.** All files live in `$CONFORMANCE_TMPDIR/platform` (W). The runner empties W before and
after a script and makes it the *current directory*, so scripts use **relative paths** only (an
implementation's runner must do the same; it may use any directory for W). Any text the runner prints that
contains W (absolute paths, log messages) shows it as `<W>`. The runner is in the "C" locale, TZ is `UTC0`
at the start, and it never sets a locale.

**Tokens.** Scripts use the common tokenizer (`#` comments, `"..."` with `\n \t \r \\ \" \xHH`;
single quotes are **not** quoting characters). `@null` = NULL pointer. Byte strings are hex (`-` = empty), code
point lists are comma separated hex (`63,e9,1f600`, `-` = empty, values are 32 bit; `ffffffff` is the
negative `wchar_t` -1). Length arguments: a number, `auto` (= 0, "up to the terminator") or `len` (= the byte /
element count of the given text). Buffer arguments: a capacity or `null` (= the NULL output pointer / sizing
call). Numbers: `strtoll(..,0)`; doubles `strtod`.

Query commands print a line that starts with their name (`NAME ARGS... RESULT`, or several lines for `ls`/`glob`); the setup commands `put`, `put_hex`, `put_pattern`, `setenv`, `now`, `tz`, `video`, `fflush` print nothing (`put` prints `put failed` on error). Library log messages (`log LEVEL "text"`), `info_` lines and `crash` lines appear where the library produced them. The whole output is compared as exact text (except as stated at the end of this section).

| Command | Effect / output |
|---|---|
| `u2w FLAGS INSIZE HEX OUT` | `utf8_to_wchar`; input bytes from `HEX` (a NUL byte is appended behind them for `auto`); prints `u2w rc=N` and, when OUT is a buffer and N>0, ` cp=c1,c2,...` (the first N output code points) |
| `w2u FLAGS INSIZE CPS OUT` | `wchar_to_utf8`; prints `w2u rc=N` and ` hex=...` (the first N bytes) |
| `os_u2w DSTSIZE LEN HEX`, `os_w2u DSTSIZE LEN CPS` | `os_utf8_to_wcs`, `os_wcs_to_utf8`; `DSTSIZE` capacity or `null`; prints `rc=N`, then (buffer given, source not NULL) ` cp=`/` hex=` of the N units and ` term=1` if the unit after them is 0 |
| `os_u2w_ptr LEN HEX`, `os_w2u_ptr LEN CPS` | `..._ptr` forms: `rc=N cp=...|hex=... term=1`, or `rc=0 ptr=null` for a NULL source |
| `os_mbs2wcs`, `os_wcs2mbs`, `os_mbs2wcs_ptr`, `os_wcs2mbs_ptr`, `os_u2mbs_ptr`, `os_mbs2u_ptr` | multibyte (locale) conversions, ASCII data only (see spec); same output shapes |
| `dtostr VALUE SIZE` | `os_dtostr` into a buffer of SIZE bytes: `dtostr V size=S rc=N str="..."` (`str` = the first N bytes; see D-2) or `rc=-1`. A line `info_dtostr_raw "..."` may follow: informational |
| `strtod TEXT` | `os_strtod`: `strtod %.17g` |
| `put PATH TEXT` / `put_hex PATH HEX` / `put_pattern PATH N` | setup helpers (plain libc): write a file; pattern byte i is `(i*7+3) & 255`. `put` prints nothing (`put failed` on error) |
| `symlink TARGET LINK` | prints `symlink 1/0` |
| `cat PATH` | `cat PATH size=N hex=...` (whole file, at most 65536 bytes) or `cat PATH nofile` |
| `info PATH` | `info PATH none|dir|link|file size=N fnv=XXXXXXXX` (FNV-1a 32 bit of the content; `lstat`, no link following) |
| `fopen H PATH MODE`, `wfopen H UTF8PATH MODE`, `wfopen_cp H CPS MODE` | `os_fopen`, `os_wfopen` (path converted from UTF-8 / given as code points); binds file handle name `H` (reopening an open name closes the old one); prints `CMD H ok|null` |
| `fclose H`, `fflush H` | `fclose` prints `fclose H RC` |
| `fwrite H TEXT`, `fread H N` | `fwrite H COUNT` (bytes written), `fread H COUNT hex=...` |
| `fseeki H OFF set|cur|end`, `ftelli H`, `fgetsize H` | `os_fseeki64` (`fseeki H rc=0|-1`), `os_ftelli64` (`ftelli H POS`), `os_fgetsize` (`fgetsize H SIZE`) |
| `fread_utf8 H`, `fread_mbs H` | `CMD H rc=N str="..."|@null` (rc is the function's return value; str is printed up to its first NUL) |
| `quick_read_utf8 PATH`, `quick_read_mbs PATH` | `CMD "text"` or `CMD @null` |
| `quick_write_utf8 PATH TEXT LEN MARKER` | `quick_write_utf8 0|1`; `TEXT` may be `@null`; MARKER 0/1 |
| `quick_write_utf8_safe PATH TEXT LEN MARKER TMPEXT BAKEXT` | `quick_write_utf8_safe 0|1` (extensions may be `@null` or `""`) |
| `quick_write_mbs PATH TEXT LEN` | `quick_write_mbs 0|1` |
| `file_size P`, `exists P`, `mkdir P`, `mkdirs P`, `rename A B`, `copyfile A B`, `unlink P`, `rmdir P` | `CMD args RESULT` (the function's integer/bool result) |
| `safe_replace TARGET FROM BACKUP|@null` | `safe_replace RC` |
| `ext PATH` | `os_get_path_extension`: `ext PATH @null` or `ext PATH offset=N ext="..."` |
| `abs_path PATH SIZE|null`, `abs_path_ptr PATH` | `os_get_abs_path(_ptr)`; prints `str=` with W as `<W>` and `rc_is_len`; when the result was truncated, `truncated rel_rc=N buflen=M` (N = rc minus the length of W; M = the length of the stored text); with SIZE 0 and a buffer, `size0 rel_rc=N` (or `size0 rc=0`); a failure prints `rc=0`. `glob` and `ls` evaluate the directory flag of every entry (and log the debug message for dangling links, in entry order) *before* the result lines are printed |
| `getcwd SIZE`, `chdir DIR` | `os_getcwd`, `os_chdir` |
| `ls DIR` | `os_opendir/readdir/closedir`: `ls DIR COUNT end_repeat=0|1`, then one line `  "name" d|f` per entry, **sorted bytewise** (entry order is not specified). `dir_nulls` checks `os_readdir(NULL)`/`os_closedir(NULL)` |
| `glob PATTERN` | `os_glob(pattern, 0xff, &g)`: `glob PATTERN rc=N count=M` and one line `  "path" d|f` each (in the order returned), or `rc=N ptr=null`; then `os_globfree`. `globfree_null` |
| `setenv NAME VALUE|@unset` | process environment |
| `config_path SIZE|null NAME|@null`, `program_data ...` | `CMD rc=N str="..."` (str only if a buffer of SIZE >0 was given); `config_path_ptr NAME`, `program_data_ptr NAME` print the string; `exe_path NAME` prints `ok= absolute= suffix=` flags |
| `now SECONDS`, `tz TZSTRING`, `video FPSNUM FPSDEN BASEW BASEH OUTW OUTH FORMAT` | fix `time()`, `TZ` (POSIX string), and the video settings (FORMAT = enum name without prefix: `NONE I420 NV12 ... R10L`) |
| `fname EXT|@null SPACES FORMAT|@null` | `os_generate_formatted_filename`: `fname "result" len=N` or `fname @null` |
| `monotonic N`, `sleep_ms MS`, `sleepto REL_MS`, `sleepto_fast REL_MS`, `nstime A B`, `nstime_null`, `free_space PATH`, `system`, `cpu_usage`, `uuid`, `dl NAME SYMBOL`, `dl_null` | property checks of machine dependent functions; every printed value is a 0/1 flag or a fixed word (see the scripts) |
| `try [quiet] CMD ARGS...`, `try_defect [quiet] CMD ARGS...` | run one command in a child process; prints the child's output (not with `quiet`), then `try CMD ok|crash|exit-N`. The runner installs a crash handler that prints `crash <message>` and exits with status 77, so a fatal error shows as `exit-77`; newlines in the message are replaced by spaces, so `bcrash("Could not get $HOME\n")` prints `crash Could not get $HOME ` with a trailing space |
| `echo TEXT...` | prints `echo TEXT...` |

**Comparison rules specific to this area.**

1. Log lines (`log LEVEL "text"`): level **and** text are compared; text is printed with W as `<W>`, and for
   `os_dlopen` failures only the part up to `):` is printed.
2. Lines starting with `info_` are informational and **not compared** (they show reference defect residue).
3. `try_defect X crash` in an expected file means the reference crashed (spec D-4); a conforming
   implementation prints `try_defect X ok` there and the two are treated as equal. Plain `try X crash` /
   `exit-77` lines must match exactly.
4. Doubles are printed `%.17g` and compared as exact strings.

## mp4 runner (spec S-022)

Script-driven wrapper around the MP4/MOV muxer (`plugins/obs-outputs/mp4-mux.c`, #included into
`gen/mp4_runner.c`). The muxer writes through the real file serializer into a file in
`$CONFORMANCE_TMPDIR` (name `mp4-out.bin`; paths are never printed). The harness replaces
`obs_output`, `obs_encoder`, `video_t`/`audio_t` accessors, the clock and the locale with the stubs in
`gen/mp4_stubs.c`; every stubbed input is a column of table 3.2 in the spec. Each script owns one
sequence of encoders and muxers; `reset` clears everything.

All numbers are decimal or `0x` hex (`strtoll`/`strtoul` rules). Hex payloads use the script hex form
(`-` = empty). `NAME` = script-chosen handle for an encoder.

| Command | Effect / output |
|---|---|
| `reset` | destroys the muxer, frees all encoders, restores stub defaults (version `33.0.1`, peak `1000`, start-chapter text `Start`, clock 1700000000) |
| `echo TEXT...` | prints `echo TEXT...` |
| `version S` / `peak F` / `start_chapter_text S` / `clock SECS` | stub inputs (`clock` is `time(NULL)` for the *next* `mux`) |
| `enc NAME video\|audio KEY VALUE ...` | creates a stub encoder; key/value pairs: `codec`, `id`, `name` (`-` = NULL), `width`, `height`, `fps N/D`, `cs default\|601\|709\|srgb\|pq\|hlg`, `range partial\|full\|default`, `rate` (encoder sample rate), `arate` (audio output rate), `speakers mono\|stereo\|2.1\|4.0\|4.1\|5.1\|7.1\|unknown`, `frame_size`, `extra HEX\|-`, `bitrate N` and `codec_type N` (int settings), `setting_str name=value`, `settings none\|empty`, `slot N`, `noslot 1`, `novideo 1`, `noaudio 1` (the accessor `obs_encoder_video/audio` returns NULL). Defaults: video h264 1920x1080 30/1 709 partial; audio aac 48000 stereo; id `stub_encoder`, name `Stub Encoder`. The encoder is placed in the next free slot of its kind unless `slot`/`noslot` is given. Nothing is printed |
| `mux mp4\|mov\|cmaf [mdta] [encinfo] [skipfin] [negcts]` | destroys a previous muxer, truncates the output file, `mp4_mux_create`; prints `mux FLAVOR tracks=N` |
| `pkt ENC PTS DTS KEY PAYLOAD [PRIORITY]` | submits one packet (`timebase_num/den` from the encoder: video `fps_den/fps_num`, audio `1/rate`; `type` from the encoder; `priority` default 0), then releases the runner's own reference. Prints `pkt ENC pts=P dts=D key=K size=N -> RET pos=X` where `RET` is the return value and `X` the serializer position (bytes handed to the file so far). `PAYLOAD` is hex or `gen:N` (below) |
| `chapter DTS_USEC NAME` | `mp4_mux_add_chapter` (`-` = empty name); prints `chapter DTS -> 0/1` |
| `finalise` | `mp4_mux_finalise`; prints `finalise -> 1` |
| `pos` | prints `pos N` (serializer position) |
| `size` / `hex` / `tree` / `dump` | flush and read the output file. `size`: `size N`. `hex`: rows `hex OFFSET8 BYTES` with 32 bytes per row (8 lowercase hex digits for the offset, bytes as lowercase hex, last row shorter). `tree`: the box tree between `tree` and `tree end` lines. `dump` = `size` + `hex` + `tree` |
| `refs ENC` / `destroy` | `refs NAME N` encoder reference count (1 = the runner's own, +1 per muxer track); `destroy` calls `mp4_mux_destroy` and prints `destroy` |
| `try CMD ARGS...` | runs one command in a forked child; its output is printed, then `try CMD ok` (exited normally) or `try CMD crash` (killed by a signal) |

**Generated payloads** (`gen:N`, N bytes). A global counter `seq` starts at 0 for each script and
is incremented once per generated payload. Bytes `i >= H` are `0x80 | ((seq + i) & 0x3F)` where `seq` is the value
before the increment and `H` the header length. Headers by encoder codec (video encoders only, and only if `N` is large enough,
else no header): `h264` (`N >= 5`): `00 00 00 01` and `0x65` (key flag set) or `0x41`; `hevc` (`N >= 6`): `00 00 00 01`, `0x26 0x01` (key) or `0x02 0x01`;
`av1` (`2 < N < 130`): `0x32`, `N-2`, then `0x10` (key) or `0x30`; `H` is 5, 6 or 3. All other payloads (audio, ProRes) have `H = 0`.

**Box tree.** Lines are `INDENT TYPE @OFFSET size=SIZE` with two spaces of indent per level, the top level at indent 2. `TYPE` prints
printable ASCII bytes as-is and every other byte (and `"` and `\`) as `\xHH`. Walking a byte range `[a, b)`: if fewer than 8 bytes remain print
`... N trailing bytes at OFF`; read `size` (be32), type; `size = 1` means a be64 largesize follows (header 16 bytes) when 16 bytes are available, `size = 0` means "to the end of the range"; a size smaller than the header or larger than the remaining range prints
`... bad box at OFF (size S, available A)` and ends the range. After printing a box its children are walked if it is a container: `moov trak edts mdia minf dinf stbl mvex moof traf tref ilst wave gmhd udta` (children start at the payload),
`meta` (children after 4 bytes), `keys`, `stsd`, `dref` (children after 8 bytes). Children of `ilst` are containers themselves (skip 0). Children of `stsd` are walked after the sample-entry header:
video tracks (handler `vide`) skip 78 payload bytes, audio tracks (`soun`) 28 bytes, 44 for MOV entries with `entry_version` 1 and 64 for version 2 (version = the be16 at payload offset 8; MOV iff the first top-level box is `ftyp` with major brand `qt  `), other handlers
(`text`) are leaves. The handler is the handler_type (payload offset 8) of the most recent `hdlr` whose parent is `mdia`. An `mdat` that directly follows a top-level `ftyp` is walked from payload offset 8 (the former placeholder). All other boxes are leaves. The two lines `tree` and `tree end` frame the output.

Log lines come from the muxer as `log LEVEL TEXT` (harness); for this area the level is compared and the text is informational. Files are compared byte for byte through `hex`.

## encopts runner (spec S-025)

Encoder option logic of obs-x264, obs-nvenc, obs-qsv11, obs-libfdk and the obs-ffmpeg encoders.
The reference runner links the REAL plugin code (the plugin `.c` files are `#include`d into the
wrapper files `gen/encopts_{x264,qsv,fdk,aud}.c` so that their static functions can be called;
`encopts_nvenc.c` and `encopts_vid.c` link the plugin files as separate objects) together with
the real `obs-data.c`, `obs-properties.c` and the shared option parser; the encoder libraries and
the host application are the recording fakes below (`gen/encopts_stubs.c` and the wrappers).
Notes for the reference build: `encopts.sources` uses `-I` paths relative to `test/conformance`,
so `run.sh` must be started from that directory; `-ffunction-sections -Wl,--gc-sections` lets the
link succeed without stubbing the encode paths; `gen/encopts_inc/ffnvcodec/` holds a
reconstruction of the NVENC SDK header (SDK 13.0 field names and bit widths).

Script syntax: the common one. Names (`D`, `P`, `X`) are chosen by the script; an unknown name or
command is a script error (exit 2). Everything printed by a command is listed here; every `log`
line is the common `log LEVEL TEXT` (compare level only). **Line order is significant**: library
fake lines, log lines and result lines appear in the order the reference produces them.

| Command | Effect / output |
|---|---|
| `echo TEXT...` | `echo TEXT...` |
| `video FMT CS RANGE FNUM FDEN W H` | host video: output format, colourspace, range, fps, and the *encoder* size. FMT = `none I420 NV12 I444 I010 P010 P216 P416 BGRA I422 YUY2 RGBA Y800`; CS = `default 601 709 srgb pq hlg`; RANGE = `full` or `partial`. Initial state: `NV12 709 partial 60/1 1920x1080`. |
| `pref_format FMT` | encoder's preferred video format (`none` = invalid/unset; initial `none`) |
| `audio CHANNELS SPEAKERS RATE` | host audio: channel count, `speaker_layout` enum number, sample rate. Initial `2 2 48000` |
| `cpu_platform N` / `hdr_peak N` / `tex_active nv12\|p010 0\|1` / `qsv_dgpu 0\|1` | QSV host state (cpu platform enum number: 0 unknown, 2 SNB, 3 IVB, 6 HSW; HDR nominal peak, initial 1000) |
| `data D` / `release D` | new empty settings object / drop |
| `set D KEY int\|str\|bool\|dbl V` | `obs_data_set_*` (user layer). `bool`: V != 0 |
| `setdef D KEY TYPE V` | `obs_data_set_default_*` |
| `unset D KEY` | `obs_data_unset_user_value` |
| `dump D` | `data D`, then per item in iteration order (S-014 §6.1 order, including its move-to-end quirk) `  KEY TYPE VALUE user\|default` (TYPE `string`+quoted, `int`, `double %.17g`, `bool 0/1`; `user` iff the item has a user value), then `end data D` |
| `props_dump P` | `props P`, per property `  NAME TYPE desc="..." visible=V enabled=E [long="..."]` + for `int`: ` min= max= step= numtype=N [suffix="..."]`, for `text`: ` texttype=N`, for `list`: ` combo=N format=N count=N` followed by one line `    item I "name" = VALUE [disabled]` per item (VALUE quoted string / `%lld` / `%.17g` / `0\|1`); then `end props P`. Enum numbers: combo type LIST=2; format INT=1 FLOAT=2 STRING=3 BOOL=4; number type SCROLLER=0. Type names: `bool int float text path list color button font editable_list frame_rate group color_alpha` |
| `props_vis P` | one line `vis P NAME:VE ...` (V visible, E enabled as digits) for every property in order |
| `props_apply P D` | `obs_properties_apply_settings`; prints `props_apply P` |
| `props_modify P NAME D` | `obs_property_modified` on one property; prints `props_modify P NAME -> 0\|1` |
| `props_free P` | destroy |
| `opts TEXT` | parse with the shared option parser (`@null` = NULL); prints `opts parse count=N ignored=M`, `  opt I "name" "value"` per option and `  ignored I "word"` per ignored word |
| `x264_defaults D` / `x264_props P` | the plugin's defaults / property list |
| `x264_settings D [update]` | `update_settings` on a context-less encoder object: prints fake lines/logs, then `x264_settings ok\|fail`, then the 4 `params` lines (below). `update` only suppresses the "banner" logs (it is **not** the update path; use `x264_create` + `x264_update` for that) |
| `x264_create X D` | `obs_x264_create`; prints `x264_create X ok\|null` and, if ok, the `params` lines |
| `x264_update X D` | `obs_x264_update`; prints `x264_update X 0\|1` then the `params` lines |
| `x264_destroy X` | destroy (prints the fake `x264.encoder_close`) |
| `x264_video_info FMT` | get_video_info callback: `x264_video_info FMT -> FMT2` |
| `x264_fail preset\|profile\|open [0\|1]` / `x264_fail param NAME` / `x264_fail clear` | switches of the x264 fake (below) |
| `nv_caps BFRAMES BREF_MODES ENGINES LOSSLESS UHQ TEN_BIT LOOKAHEAD` | NVENC capability fake (applies to all codecs); `nv_devices N`, `nv_av1_support 0\|1`, `nv_split_broken 0\|1` the other probe results (initial 1 device, AV1 supported, split OK; caps all zero until `nv_caps`) |
| `nv_defaults h264\|hevc\|av1 D` / `nv_props CODEC P` | defaults / property list |
| `nv_read D` | `nvenc_properties_read`: prints the 5 `nvprops` lines |
| `nv_apply CODEC D` | zeroed `NV_ENC_CONFIG`; read D's settings; `apply_user_args`; prints `nv_apply success=0\|1` and the `nvconfig`/`nvrc`/`nvcodec` lines (below) |
| `nv_userarg D NAME` | `get_user_arg_int`: `nv_userarg NAME found=0\|1 value=N` (value preset to 7777) |
| `qsv_defaults KIND D` / `qsv_props KIND P` | KIND = `h264v1 h264v2 av1 hevc` |
| `qsv_migrate D` | the three legacy-setting migrations in the order the rate-control callback calls them (latency, target usage, rate control) |
| `qsv_create KIND X D` | `obs_qsv_create`; prints `qsv_create X ok\|null`, the `qsvparams` lines if ok |
| `qsv_update X D` / `qsv_destroy X` | `obs_qsv_update` (prints `qsv_update X 0\|1`, params) / destroy |
| `qsv_video_info KIND FMT` | `qsv_video_info KIND FMT -> FMT2 WxH` (width/height are computed from the host encoder size) |
| `fdk_defaults D` / `fdk_props P` / `fdk_create D` / `fdk_fail N` | libfdk; `fdk_create` prints fake library lines then `fdk_state ...` and `fdk_create ok\|fail` (the created encoder is destroyed immediately); `fdk_fail N` (0 = off) makes the N-th fake library call of the next creations fail |
| `vid_update KBPS KEYINT_SEC OPTS` | `ffmpeg_video_encoder_update` on a zeroed codec context whose `gop_size` is preset to the sentinel 12345; `OPTS` may be `@null`; prints `av.opt_set` lines then two `vid` lines |
| `aud_defaults D` / `aud_props P` | ffmpeg audio encoder defaults / properties |
| `aud_create aac\|opus\|pcm\|pcm24\|pcm32\|alac\|flac D` | `*_create`; prints fake lines, logs, then `aud_create KIND ok type=TYPE` + 2 `aud state` lines, or `aud_create KIND null` |
| `aud_codec NAME 0\|1` / `aud_formats LIST\|none` / `aud_rates LIST\|none` / `aud_open_fail 0\|1` | switches of the libavcodec fake |

Result lines (printf-style formats; `%s` strings in the `"..."` quoted form where shown, `@null` for NULL):

```
x264 (4 lines, after x264_settings / x264_create / x264_update):
params csp=%d size=%dx%d fps=%d/%d timebase=%d/%d keyint_max=%d bframe=%d vfr=%d
params rc method=%d bitrate=%d vbv_max=%d vbv_buf=%d rf=%.9g filler=%d nal_hrd=%d
params vui sar=%dx%d fullrange=%d colorprim=%d transfer=%d colmatrix=%d
params headers repeat=%d annexb=%d aud=%d loglevel=%d
nv_read (5 lines; fields of the plugin's property struct; opts_str is @null unless "opts" had a user value):
nvprops bitrate=%lld max_bitrate=%lld keyint_sec=%lld cqp=%lld device=%lld bf=%lld
nvprops bframe_ref_mode=%lld split_encode=%lld target_quality=%lld
nvprops rate_control="%s" preset="%s" profile="%s" tune="%s" multipass="%s" opts_str=%s
nvprops aq=%d lookahead=%d disable_scenecut=%d repeat_headers=%d force_cuda_tex=%d
nvprops opts count=%d ignored=%d
nv_apply (zeroed config; all values unsigned unless a sign is shown by the type):
nv_apply success=%d
nvconfig gopLength=%u frameIntervalP=%d
nvrc avg=%u max=%u vbvbuf=%u vbvdelay=%u tq=%u tqlsb=%u cb=%d cr=%d lookaheadLevel=%u
nvrc constQP=%u/%u/%u minQP=%u/%u/%u maxQP=%u/%u/%u initialRCQP=%u/%u/%u          (P/B/I = qpInterP/qpInterB/qpIntra)
nvrc bits minQP=%u maxQP=%u initialRCQP=%u aq=%u lookahead=%u disableIadapt=%u disableBadapt=%u temporalAQ=%u aqStrength=%u
nvcodec h264 level=%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u filler=%u
nvcodec hevc level=%u tier=%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u filler=%u
nvcodec av1 level=%u tier=%u tiles=%ux%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u padding=%u     (tiles = columns x rows)
qsv (5 lines after qsv_create / qsv_update):
qsvparams usage=%d profile=%d rc=%d async=%d la=%d
qsvparams bitrate=%d max=%d qpi=%d qpp=%d qpb=%d icq=%d keyint_sec=%d bframes=%d
qsvparams size=%dx%d fps=%d/%d repeat_headers=%d fmt10=%d
qsvparams vui fmt=%d fullrange=%d prim=%d trc=%d matrix=%d chroma=%d/%d
qsvparams hdr dispx=%d,%d,%d dispy=%d,%d,%d white=%d,%d maxlum=%u minlum=%u cll=%d fall=%d
fdk:
fdk_state channels=%d sample_rate=%d frame_size_bytes=%d packet_buffer_size=%d
vid_update (2 lines; the first starts with "info_vid" instead of "vid" when KBPS > 2147483):
vid bit_rate=%lld rc_buffer_size=%d size=%dx%d time_base=%d/%d framerate=%d/%d pix_fmt=%s
vid color_range=%d primaries=%d trc=%d colorspace=%d chroma_loc=%d gop_size=%d height=%d
aud_create ok:
aud_create KIND ok type=TYPE
aud state bit_rate=%lld sample_rate=%d channels=%d order=%d mask=0x%llx sample_fmt=%d
aud state frame_size=%d frame_size_bytes=%d planes=%d audio_size=%d strict=%d flags=%d
```

`aud state order` is the channel-order enum (1 = native order) and `mask` the native channel mask
(0 otherwise); `pix_fmt` names: `nv12 yuv420p yuv444p p010le bgra rgba yuv422p yuv420p10le none other`;
`color_range` 1 = MPEG, 2 = JPEG; colour enums are FFmpeg's numbers. `x264_settings` prints the four `params`
lines even when it fails. For `qsv_update` the five `qsvparams` lines follow `qsv_update X 0|1`.
`aud_create` failure: `aud_create KIND null`.

### Contract of the fakes (a conforming runner must implement exactly this)

* **Host.** `obs_module_text(KEY)` returns `KEY` (the identity). `obs_encoder_get_name` returns `"enc"`.
  `obs_encoder_set_last_error(MSG)` prints `host.set_last_error "MSG"`. Encoder width/height come from
  the last `video` command; the *output* info has the same width/height/fps/format/colourspace/range.
* **libx264 (x264 fake).** `x264_param_default_preset(PRESET, TUNE)` prints
  `x264.param_default_preset preset=P tune=T` (`P`/`T` are the strings, empty strings print as nothing, a NULL tune prints `<null>`),
  zeroes the whole parameter struct, then sets the **sentinels** `i_keyint_max=-1111`, `i_bframe=-2222`,
  `rc.i_rc_method=-3333`, `rc.f_rf_constant=-3.5`; returns -1 iff `x264_fail preset` is active, else 0.
  `x264_param_parse(NAME, VALUE)` prints `x264.param_parse NAME=VALUE -> R` where `R` is -1 if NAME was registered
  with `x264_fail param`, else 0; on success it additionally applies exactly these names (exact match, `atoi`/`atof`):
  `keyint`→`i_keyint_max`, `bframes`→`i_bframe`, `bitrate`→`rc.i_bitrate`, `crf`→`rc.f_rf_constant`, `vbv-maxrate`→`rc.i_vbv_max_bitrate`,
  `vbv-bufsize`→`rc.i_vbv_buffer_size`; every other name is accepted and ignored.
  `x264_param_apply_profile(PROFILE)` prints `x264.param_apply_profile P -> R` (R = -1 iff `x264_fail profile`, else 0) and changes nothing.
  `x264_encoder_open` prints `x264.encoder_open -> ok|NULL` (NULL iff `x264_fail open`); `x264_encoder_headers` prints
  `x264.encoder_headers` and returns zero NALs; `x264_encoder_reconfig` prints `x264.encoder_reconfig`, returns 0;
  `x264_encoder_close` prints `x264.encoder_close`.
  x264 build constants: `X264_BUILD >= 139`, `X264_CSP_I420=2`, `NV12=4`, `I444=12`, `X264_RC_CRF=1`, `ABR=2`; preset/tune/profile/colour name tables are in the spec.
* **NVENC.** No library; the capability probe returns the values given by `nv_caps` etc. SDK constants used: version 13.0 (all
  `NVENC_*_OR_LATER` options exist); `NV_ENC_BFRAME_REF_MODE_{DISABLED,EACH,MIDDLE}=0,1,2`;
  `NV_ENC_SPLIT_{AUTO,DISABLE,TWO_FORCED,THREE_FORCED,FOUR_FORCED}_MODE=0,15,2,3,4` (harness assumptions, spec §11).
* **QSV.** `qsv_get_cpu_platform` returns `cpu_platform`. `qsv_encoder_open` prints `qsv.encoder_open codec=N tex=0|1`
  (codec enum: AVC 0, AV1 1, HEVC 2) and returns a non-NULL context; `qsv_encoder_close` prints `qsv.encoder_close`;
  `qsv_encoder_reconfig` prints `qsv.encoder_reconfig target_bitrate=N` and returns 1; headers calls return empty headers;
  `qsv_encoder_version` returns 2.9. The "is discrete GPU" flag consulted by get_video_info is `qsv_dgpu`.
* **libfdk-aac.** Every library call increments a counter (reset at the start of each `fdk_create`); the call whose
  number equals `fdk_fail N` prints `fdk.NAME -> INIT_ERROR` (after its normal line, listed next) and returns AACENC_INIT_ERROR (otherwise OK).
  Every call first prints its normal line: `fdk.aacEncOpen modules=0 channels=N`, `fdk.SetParam NAME=V` (NAME one of AOT SAMPLERATE CHANNELMODE
  CHANNELORDER BITRATEMODE BITRATE TRANSMUX AFTERBURNER), `fdk.aacEncEncode (init)`, `fdk.aacEncInfo`
  (reports frameLength 1024), `fdk.aacEncClose`. `fdk_state channels= sample_rate= frame_size_bytes= packet_buffer_size=`.
* **FFmpeg video.** `av_opt_set(NAME, VALUE, FLAGS)` prints `av.opt_set NAME=VALUE flags=F -> 0|notfound`
  (`notfound` iff NAME starts with `nosuch`). `av_pix_fmt_desc_get` knows only the chroma shifts of the formats the plugin can
  produce (4:2:0 -> (1,1); 4:2:2 -> (1,0); 4:4:4, RGB, gray -> (0,0)); any other format has no descriptor.
* **FFmpeg audio.** `avcodec_find_encoder_by_name(N)` prints `av.find_encoder N -> found|missing` (table: aac, libopus, opus,
  pcm_s16le, pcm_s24le, pcm_f32le, alac, flac, all present initially; `aud_codec NAME 0` removes one).
  Codec descriptors report LOSSLESS for alac flac pcm_*. `avcodec_get_supported_config` returns the list given by
  `aud_formats` (comma separated names of `u8 s16 s32 flt u8p s16p s32p fltp dbl`) / `aud_rates`, or NULL for `none`
  (initial: both NULL). `av_channel_layout_default(N)`: native-order masks 1 `0x4` (mono), 2 `0x3`, 3 `0x7`, 4 `0x107`, 5 `0x607`, 6 `0x60f`, 7 `0x70f`, 8 `0x63f`
  (other N: unspecified order with N channels, mask printed as 0). `avcodec_open2` prints
  `av.open2 codec= bit_rate= sample_rate= channels= sample_fmt= strict= flags=`, fails iff `aud_open_fail 1`, and sets
  `frame_size` 0 for codecs whose name starts with `pcm`, else 1024. `av_samples_alloc` prints
  `av.samples_alloc channels= nb_samples= fmt=`. `av_strerror(E)` yields the text `error E`. Sample-format numbers: u8 0, s16 1, s32 2, flt 3, dbl 4, u8p 5, s16p 6, s32p 7, fltp 8.

### Comparison rules specific to this area

1. Log lines: level compared, text ignored.
2. Lines starting with `info_` (the bitrate-overflow defect in `52-...`) are not compared.
3. Floats are printed `%.9g` (`rf`) and doubles `%.17g`; compared as exact strings.
4. All other lines must be identical.

## helpers runner (spec S-024)

One runner (`gen/helpers_runner.c`) for twelve small modules; the first word of a command
selects the module. Run `./run.sh` from `test/conformance` (the registration file uses a
relative `-I`). Scripts: `scripts/helpers/<module>-*.script`. Additional rules for this area:

- `crash` marker lines (`mul_div64_by_zero crash`, `hk_from @null crash`) mean the reference died
  on that input (spec DEFECT D-1/D-3). A conforming implementation must not crash; for
  `hotkeys-defect-null` it prints `hk_from @null 0` (and the comparison treats `crash` as equal).
- `args-defect-argf-empty`: an expected `argv[i]=@null` with `i < argc` equals `argv[i]=""`.
- `muldiv64-quirk-overflow`: only the `exact=` field is compared; `ref=` is the reference's wrapped value.
- Profiler report lines are printed as `text "..."` data lines and are compared byte for byte.
  Log lines (`log <level> ...`) follow the common rule (level only); pointers inside log text are masked as `0xPTR`.
- Numbers are printed in the formats shown in the vectors; `%g` means the C `%g` format of a double.

Commands (all arguments are tokens; `@null` = NULL):

| Module | Commands |
|---|---|
| general | `echo T`, `const` (prints the constants table `NAME value`), `semver MAJOR MINOR PATCH` (prints `semver a b c = 0x%08x` of `MAKE_SEMANTIC_VERSION`) |
| opts | `opts_parse STRING` prints `count/ignored_count`, which of the three pointers are non-NULL, each `opt I name= value=`, each `ignored I`, `input_words=N` and each `word I` |
| uint | `u128_layout`, `u128_add LOW HIGH LOW HIGH`, `u128_mul64 A B`, `u128_div32 LOW HIGH D`, `u128_shift32 A`, `u128_shift64 A`, `mul_div64 N M D` (prints `ref=` and `exact=`; `overflow` if the exact quotient exceeds u64), `mul_div64_by_zero`. Results print as `high=%016x low=%016x` |
| args | `args_create SLOT EXE`, `args_add SLOT S`, `args_addf_str SLOT FMT S`, `args_addf_int SLOT FMT N`, `args_addf_plain SLOT S` (format `%s`), `args_dump SLOT` (argc, then argv[0..argc], `@null` for NULL), `args_destroy SLOT`, `args_destroy_null` |
| hotkeys | `hk_mode null|core` (fallback path / core present), `hk_count`, `hk_name ID`, `hk_from NAME`, `hk_dump` (all ids as `ID NAME ID'` plus a mismatch counter), `hk_free` |
| missing files | `mf_list_new L`, `mf_file_new F PATH source|script [SRCNAME]`, `mf_add L F`, `mf_count L`, `mf_get L IDX` (path, source name, refcount, which file slot), `mf_append DST SRC`, `mf_ref F`, `mf_issue F NEWPATH` (prints the callback), `mf_release F`, `mf_release_null`, `mf_destroy L`, `mf_destroy_file F` |
| task | `task_create Q`, `task_queue Q LABEL`, `task_gate Q` (queue a task that blocks the worker; returns when it is running), `task_requeue Q LABEL CHILD`, `task_inside_q Q LABEL TARGETQ`, `task_main_inside Q`, `task_wait Q` (prints the result), `task_sync Q` (wait, result not printed), `task_wait_release Q` (wait; the gate is opened as soon as the wait has started), `task_destroy Q`, `task_destroy_release Q N` (destroy; gate opened once N tasks are pending incl. the stop marker), `task_log` (prints the worker's trace), `task_null` |
| profiler | `p_clock NS`, `p_tick NS` (fake clock), `p_name ID TEXT` (new name each call), `p_begin` / `p_stop` (`profiler_start/stop`), `p_reenable`, `p_root ID NS`, `p_start ID`, `p_end ID`, `p_call ID NS` (start, advance clock, end), `p_burst ID COUNT DUR0 STEP GAP`, `p_snap`, `p_dump`, `p_filter PREFIX`, `p_print [snap]`, `p_print_between [snap]`, `p_csv`, `p_snap_free`, `p_free`, `p_null` |
| bpm | `bpm_set KEY V` (`out_total out_dropped rendered lagged v_total v_skipped enc_out has_video codec`), `bpm_inject OUT TRACK PTS DTS KEYFRAME VIDEO HEXDATA CTS FER FERC PIR` (`@notime` as CTS = no timing; prints new size and data or `unchanged`), `bpm_destroy OUT`, `bpm_null`. The wall clock is `wall(ns) = ns` since 1970 |
| happy eyeballs | `he_order BINDLEN HOST PORT F...` (F = `4`, `6`, `x`; prints the resolver hints and the final order as `<position><a|b|x>`), `he_fail 1`, `he_consts` |
| file updater | `fu_put PATH TEXT` (in-memory file), `fu_cat PATH`, `fu_json_get PATH KEY`, `fu_ls`, `fu_http URL CODE BODY [HEADER]` (fake server; code -1 = transfer failure), `fu_confirm accept|reject|replace [TEXT]`, `fu_run LOCAL CACHE URLBASE UA [nocb]` (runs the update synchronously and prints the trace of file system calls `fs ...`, HTTP requests `http GET ... "header"...`, callback calls `confirm ...`), `fu_single URL`, `fu_get_path DIR FILE` |

## flv runner (spec S-021)

One runner (`gen/flv_runner.c`, scripts `scripts/flv/*.script`) for the FLV muxer
(`flv-mux.c`), the HEVC/AV1 configuration-record builders (`rtmp-hevc.c`, `rtmp-av1.c`) and the AMF0
codec of the vendored librtmp (`amf.c`). Scripts `01`-`09` are AMF0, `10`-`22` the muxer, `30`-`37`
HEVC, `40`-`43` AV1. `gen/flv_stubs.c` supplies the fake encoder/audio/video objects that `flv_meta_data`
queries (scripted with `cfg`, see spec 3.3). Build flags: `-DSSL_CTX=void -DENABLE_HEVC`.

Every *action* command runs in a forked child. Commands that receive a byte string get it in memory
whose last byte is followed by an inaccessible page; AMF encoders write into a buffer of exactly `cap`
bytes at the end of a page. A child that dies prints `<command> crash` in the expected file.

**Crash lines.** Besides the common rule, a conforming implementation MUST NOT crash and MUST print
for an input recorded as `crash` the following *failure line* (this is what the comparison accepts as
equal to `crash`; any `log` lines printed for that command are ignored):

| command | failure line |
|---|---|
| `decode`, `decode_array`, `prop_decode` | `<command> ret=-1` |
| `hevc_header`, `av1_header` | `<command> ret=0 header=unset` |
| `av1_packet` | for the single crash case (`3200`, a frame OBU with empty payload): `av1_packet size=2 keyframe=0 priority=0 drop_priority=0 pts=11 dts=12 fresh=1 data=3200` (spec D-9, R51) |
| `meta`, `fileinfo`, `flv_mux`, `ex_*`, `ax_*` | `<command> error` |
| `ms` | `ms error` |
| `getprop`, `dec_*` | no `crash` case exists in the committed vectors |

For `hevc_header` the reference also fails *without* crashing in a defective way (it returns a non-zero
size but leaves `*header` unset, spec D-7); the expected file records the reference line
(`hevc_header ret=N header=unset`, N > 0). A conforming implementation prints `ret=0 header=unset` there:
the comparison treats `ret=N header=unset` in the expected file as equal to `ret=0 header=unset`.
`log` lines: only the level is compared; the runner prints RTMP library messages of level WARNING or worse
(CRIT and ERROR as `log error ...`, WARNING as `log warning ...`; the reference only emits ERROR in the covered paths),
debug and info output of the reference is not part of the contract.

Common conventions of this runner: DATA = hex | `-` (empty) | `@name` | `@null`; strings (`enc_string`,
`add` names and values, `enc_named_*`) are literal text tokens or `buf:NAME` (raw bytes of buffer NAME).
Doubles are written as decimal, `inf`, `-inf`, `nan`, `-0` or `bits:HEX16`; they are printed as `%.17g`
followed by `bits=HEX16` (NaN prints `nan bits=...`). Integers are decimal or `0x`.

State commands (run in the parent, inherited by later commands):

| Command | Effect |
|---|---|
| `buf NAME HEX...`, `cat NAME DATA...`, `rep NAME COUNT HEX`, `trail N`, `echo TEXT` | as in `nal` runner (`echo` prints only its first token; quote the text) |
| `cfg KEY VALUE` | set a scripted input: `vcodec` (string), `width`, `height`, `vbitrate`, `fps`, `abitrate`, `arate`, `channels`, `version` (string), `version_rep N` (version = N times `v`). Defaults: `h264`, 1920, 1080, 2500, 60, 160, 48000, 2, `0.0.0-conformance` |
| `cap N` | capacity in bytes of the AMF encode buffer (default 4096) |
| `pkt NAME video|audio PTS DTS NUM DEN KEY DATA` | define an `encoder_packet` (`DATA` may be `@null`) |
| `obj NAME` | new empty AMFObject |
| `add OBJ PNAME KIND [V [W]]` | append a property to OBJ. `PNAME` `-` = no name. KIND: `number V`, `bool V`, `string S`, `null`, `object @O` / `ecma @O` / `strict @O` (nested copy of object O), `date V UTC`, `invalid`, `type N V` (raw `p_type` code N) |

Action commands. All print one result line (some followed by indented lines):

| Command | Calls | Output |
|---|---|---|
| `enc_int16 V`, `enc_int24 V`, `enc_int32 V`, `enc_number V`, `enc_bool V`, `enc_string S`, `enc_named_string N S`, `enc_named_number N V`, `enc_named_bool N V` | the `AMF_Encode*` function with an output window of `cap` bytes | `<cmd> len=N hex=...` or `<cmd> null` |
| `enc_obj OBJ [object|ecma|strict]`, `enc_prop OBJ INDEX` | `AMF_Encode`, `AMF_EncodeEcmaArray`, `AMF_EncodeArray`, `AMFProp_Encode` | same; `log error ...` lines precede the result when a property is dropped |
| `dec_int16|dec_int24|dec_int32|dec_bool DATA` | `AMF_Decode*` | `<cmd> VALUE` |
| `dec_number DATA` | `AMF_DecodeNumber` | `dec_number VALUE bits=HEX16` |
| `dec_string DATA`, `dec_longstring DATA` | `AMF_DecodeString/LongString` | `<cmd> len=N val="..."` (`val=null` when `av_val` is NULL) |
| `decode DATA [NAMES=1]` | `AMF_Decode` | `decode ret=N num=K` then the property tree (below), or `decode ret=-1` |
| `decode_array DATA COUNT [NAMES=0]` | `AMF_DecodeArray` | like `decode` |
| `prop_decode DATA [NAMES=0]` | `AMFProp_Decode` | `prop_decode ret=N` and the single property (tree), or `prop_decode ret=-1` |
| `getprop DATA SEL` | `AMF_Decode` (names on), then `AMF_GetProp` (SEL = a token starting with a decimal digit: index, else a name; the empty token is the empty name), `AMF_CountProp`, `AMFProp_*` accessors | `getprop decode=R count=C valid=V type=T` and `  name="..." number=X boolean=B string="..." object_num=K` where `name` is the property name (`""` for none/INVALID), `number` is `%.17g bits=HEX16` (`0 bits=0000000000000000` for INVALID) for NUMBER, BOOLEAN and INVALID and `n/a` otherwise, `boolean` is `AMFProp_GetBoolean` for those three types and `-1` otherwise, `string` is `AMFProp_GetString` (empty unless STRING), `object_num` is `o_num` of `AMFProp_GetObject` (0 unless OBJECT). `decode=` is the return value of the decode (`-1` on failure, then `count=0`) |

Property tree lines: `<indent>[I] name="..."|- type=NAME ...` where the indent is `2 * (depth + 1)` spaces; depth is 0 for the properties printed by `decode`, `decode_array`, `prop_decode` and 2 for the properties of an `amf` block inside a decoded tag (6 spaces); the children of a container have depth + 1;
`NUMBER value=%.17g bits=HEX16`, `BOOLEAN value=0|1`, `STRING|XML_DOC len=N value="..."`,
`DATE value=... bits=... utc=N` (signed 16 bit), `OBJECT|ECMA_ARRAY|STRICT_ARRAY num=K` followed by the nested
lines, `NULL`. Type names: NUMBER BOOLEAN STRING OBJECT MOVIECLIP NULL UNDEFINED REFERENCE ECMA_ARRAY OBJECT_END
STRICT_ARRAY DATE LONG_STRING UNSUPPORTED RECORDSET XML_DOC TYPED_OBJECT AVMPLUS INVALID. Strings are
printed with `\\ \" \n` and `\xHH` escapes for bytes < 0x20 or >= 0x7f.

FLV commands (output = `<cmd> size=N hex=...` plus the decoded tags).

*Long hex fields.* Every printed byte string is lower-case hex without separators, `-` when empty. A string
of more than 512 bytes (`hex=`, `header=`, `data=`) is abbreviated to its first 160 bytes, `...`, its last 16
bytes and the length in parentheses, e.g. `0a0b...0c0d(70020)`; `payload=` fields of decoded tags are
abbreviated the same way beyond 128 bytes, but with the first 48 bytes:

| Command | Calls |
|---|---|
| `meta H` | `flv_meta_data(output, &p, &n, H != 0)`, decoded as an FLV header (if H) and tags |
| `fileinfo DUR_MS SIZE [H=1]` | builds the file with `flv_meta_data(H)`, applies `write_file_info` at offset 58 and decodes the whole file. First line `fileinfo before=B after=A` (file size before/after the patch), then the usual `fileinfo size=N hex=...` block (the file header line is printed when H) |
| `flv_mux PKT DTS_OFFSET IS_HEADER` | `flv_packet_mux`; `flv_mux empty` when it produced nothing, `flv_mux untouched` if it left the outputs unchanged |
| `ex_start PKT CODEC IDX`, `ex_end PKT CODEC IDX`, `ex_frames PKT CODEC DTS_OFFSET IDX` | `flv_packet_start/_end/_frames` (CODEC `h264|av1|hevc|none` or a number) |
| `ex_raw PKT CODEC DTS_OFFSET TYPE IDX` | `flv_packet_ex` with an arbitrary packet type |
| `ex_metadata CODEC IDX BITS PRIM TRC SPACE MINL MAXL` | `flv_packet_metadata` |
| `ax_start PKT CODEC IDX`, `ax_frames PKT CODEC DTS_OFFSET IDX`, `ax_raw PKT CODEC DTS_OFFSET TYPE IDX` | `flv_packet_audio_start/_frames`, `flv_packet_audio_ex` (CODEC `aac|none`) |
| `ms PKT V`, `to_video TEXT`, `to_audio TEXT` | `get_ms_time`, `to_video_type`, `to_audio_type`; print `ms N`, `to_video N`, `to_audio N` |

The decoded block of a muxer result starts (only if the output has an FLV header) with `  flvheader sig=FLV version=1 flags=0x05 offset=9 prevsize0=0` (version, flags in `0x%02x`, offset and prevsize0 read from the bytes), and then has per tag: `  tag type=T datasize=D ts=TS streamid=S`
(`TS` = timestamp low 24 bits | extension byte << 24, as unsigned), then `    video ...` / `    audio ...` /
the AMF tree for script tags (`    amf ret=R num=K` + tree; when the AMF decode fails just `    amf ret=-1`) and `  prevsize=P expected=E` (E = 11 + D).
`video ex=0 frametype=F codec=C avcptype=A cts=S payload=...`; `video ex=1 frametype=F ptype=P
[multitrack=M inner=I] fourcc="XXXX" [track=N] [cts=S] payload=...` (`cts` shown for `avc1`/`hvc1`
packet type 1, with the sign extended 24-bit value); `audio ex=0 format=F rate=R size=S type=T aacptype=A
payload=...`; `audio ex=1 ptype=P [multitrack=M inner=I] fourcc="mp4a" [track=N] payload=...`. For Enhanced
RTMP video of (inner) packet type 4 the payload is also decoded with `AMF_Decode(names off)` and printed as
`    amf ret=... num=...` + tree. A decoded tag whose body does not fit prints `tag body truncated` and stops.

Codec-record commands: `hevc_header DATA`, `av1_header DATA` print `<cmd> ret=N header=HEX|unset`
(the runner pre-sets `*header` to a sentinel, `unset` = still the sentinel). `av1_packet DATA [PRIORITY
[KEYFRAME]]` prints `av1_packet size=S keyframe=K priority=P drop_priority=D pts=11 dts=12 fresh=1
data=HEX` (the input packet has pts 11, dts 12). `rbsp DATA HEADER_LEN` prints `rbsp len=N data=HEX
tail64zero=B`.

Modelled defects: the runner itself aborts (printing `crash`) for `meta`/`fileinfo` when the configured
version string is longer than 3699 bytes (spec D-5: the reference overruns a stack buffer) and for a
muxed tag whose 24-bit DataSize does not equal the real payload size (spec D-6).

**Crash results (`try ... crash`).** Vectors record reference crashes (spec D-1, D-2, D-3, D-5, D-6). A conforming
implementation does not crash there. When comparing, a result line `try CMD crash` in the expected file matches an actual
`try CMD ok`; lines the reference printed inside the child *before* the crash must be a prefix of the child's lines in the actual output
(the implementation may print more after that point; the lines between the start of the child and its `ok`/`crash` line are the child's output).
Scripts end the `try` cases with a `reset` before anything else is dumped.

## effect runner (spec S-020)

Area `effect`: the effect language (`effect-parser.c`, `shader-parser.c`, `effect.c`, plus the effect entry points of
`graphics.c`) running on a **fake graphics backend** (`gen/effect_stubs.c`). The backend models only what effects need:
shaders remember the text and location string they were created with; creation runs the real shader parser on the text
and exposes the text's global variables as shader parameters; texture and sampler handles are small integers.
Every backend call prints an event line (see below), so the exact shader text that the effect parser hands to the
backend is part of the expected output.

The runner reads the script with its own loop: all commands are single lines in the common token syntax, except the
block command `text NAME` ... `@end` (see below). Process state: one graphics context, up to 64 named text blocks and
64 named effects. The working directory is the repository root (`#include` paths are relative to the directory of the
effect's file name, else to the working directory).

**Text arguments (`SRC`).** A literal string, `@text:NAME` (a block defined with `text`) or `@file:PATH` (a repository
relative file, e.g. `@file:libobs/data/default.effect`). `@null` is NULL where a pointer is accepted (file names, handles).

**Setup.** `text NAME` starts a block: the following lines are stored verbatim (no tokenising, `#` is literal) until a line
that is exactly `@end`; the final newline is dropped. `textf NAME PATH` loads a repository file as a block.
`gfx [PREPROCESSOR|@null]` (re)creates the graphics context (all effects are destroyed) and sets the name returned by
`device_preprocessor_name()` (`_OPENGL`, `_D3D11`, ...; default NULL). `reset` destroys everything. `backend KEY VALUE`:
`text none|hash|full` (what a shader creation prints), `parse on|off` (the fake backend runs the shader parser / knows no
variables), `events on|off` (print shader loads, parameter uploads and shader destruction), `fail vertex|pixel MSG|off` (creation
returns NULL with this error text), `warn vertex|pixel MSG|off` (creation returns a shader plus this error text),
`ep brief|full` (omit function bodies in the `ep` dump). Defaults at the start of a script and after `gfx`/`reset` are NOT reset: `text full`,
`parse on`, `events on`, `ep full`, no forced failures (scripts set what they need). `echo WORDS...` prints the words. `cd @tmp|@repo` changes the working
directory (`@tmp` = `$CONFORMANCE_TMPDIR`), `mkdir NAME`, `writefile NAME SRC` operate in `$CONFORMANCE_TMPDIR`.

**Backend events** (printed as they happen, in call order):

```
backend vertexshader_create|pixelshader_create location="<file> (Vertex|Pixel shader, technique T, pass N)" len=BYTES fnv=HEX16
  text "<escaped shader text>"                  (only with backend text full)
  sparam I name=NAME type=TYPE array=N          (one per global variable of the text, in order; backend parse on)
  result ok | result ok (with error text) | result fail (forced) | result fail (shader_parse) errors="..."
backend shader_destroy location="..."
backend load_vertexshader|load_pixelshader "<location>"|"none"
backend set_val shader=vertex|pixel param=NAME size=N hex=...       (texture parameters: texture=ID srgb=0|1)
backend set_texture shader=... param=NAME texture=ID                (ID -1 = NULL)
backend set_next_sampler shader=... param=NAME sampler=ID
```
`len`/`fnv` are the byte length and the standard 64-bit FNV-1a hash of the shader text (offset basis `0xcbf29ce484222325`,
prime `0x100000001b3`, hex printed as 16 lower-case digits). `location` file names that start with
`$CONFORMANCE_TMPDIR` print as `@tmp...`. Log lines (`log LEVEL text`) follow the common rule (only the level is compared).

**Creation.** `effect NAME LABEL SRC` calls `gs_effect_create(SRC, LABEL, &error)` and prints `effect NAME result=ok|fail` and
`  error_string=...` (`@null` or the quoted error text). `effect_noerr` passes a NULL error pointer (prints no error line).
`effect_from_file NAME @tmp/FILE` calls `gs_effect_create_from_file` (prints `same_as=OTHER` when the cache returned an existing
effect). `destroy E` calls `gs_effect_destroy` (prints `cached=0|1`). Failed creations store NULL under the name. At `gfx`, `reset` and at the end of the script, the effects that are not cached (created with
file name `@null`) are destroyed first, in the order their names were first used, then the context is destroyed, which destroys the cached effects
newest first; shader destruction prints `backend shader_destroy` lines (`events on`) in pass order (vertex before pixel).
`ep LABEL SRC` runs only the effect parser (same as the first half of `gs_effect_create`) and dumps the parser's tables:

```
ep result=ok|fail
  errors count=N has_errors=0|1
    err I error|warning file="F" row=R col=C msg="..."
  ep params=N structs=N funcs=N samplers=N techniques=N
  ep_param I type=T name=N property=0|1 const=0|1 uniform=0|1 texture=0|1 array_count=N annotations=N
    ep_annotation J type=T name=N
  ep_struct I name=N vars=K / var type=T name=N mapping="M"|@null var_type=IN|INOUT|OUT|UNIFORM
  ep_func I name=N ret=T mapping=.. params=K / var ... / struct_deps=[..] func_deps=[..] param_deps=[..] sampler_deps=[..] / contents="body"
  ep_sampler I name=N states=K / state "NAME" = "VALUE"
  ep_technique I name=N passes=K / ep_pass J name="N"|@null / vertex=["tok",...] / fragment=["tok",...]
  error_string=@null|"..."
```
Default values are not part of the `ep` dump (the reference moves them into the effect parameters during the compile step); they are
checked through `dump` on successfully created effects. With `backend ep brief` `contents="..."` is replaced by `contents_len=N`. When the parse succeeded the created effect is dumped too (`effect (effect) ...`, below).
`sparse LABEL SRC` runs the stand-alone shader parser (`shader_parse`) and dumps `sp params/structs/samplers/funcs`:
`param|var I type= name= mapping= var_type=IN|INOUT|OUT|UNIFORM|CONST array_count= default_size= default=..`,
`sampler I name= states=K` with `state "N" = "V"` and an `info filter=.. address_u=.. address_v=.. address_w=.. max_anisotropy=..
border_color=%08x` line (the result of `shader_sampler_convert`), `func I name= return_type= mapping= params=K` with
`body_tokens=N body="..."`. `ptype NAME`, `filter NAME`, `addr NAME` print the converters' result (`FLOAT`, `VEC2`, ... / `POINT`,
`LINEAR`, ... / `CLAMP`, `WRAP`, ...).

**Effect structure** `dump E`:

```
effect NAME params=N techniques=N viewproj=PARAM|- world=PARAM|-
  param I name=N type=TYPE annotations=K default_size=BYTES default=HEX [f=[..] | l=[..]]    (string/texture defaults quoted)
    annotation J name=N type=TYPE default_size=.. default=..
  technique I name=N passes=K
    pass J name="N"|@null vertex_params=[names] pixel_params=[names] vertex=yes|no pixel=yes|no
```
Floats print with `%.9g`; the hex is the stored bytes (little endian; ints/bools are 8-byte C `long`s on LP64).

**Handles.** `E` is an effect name or `@null`. Parameters: `E:#IDX`, `E:NAME`, or an annotation `E:PARAM/#IDX`, `E:PARAM/NAME`
(PARAM as before); `@null` for a NULL parameter. A name or index that is not found yields NULL.

**Runtime commands** (one library call each; the result line shown): `nparams E` -> `nparams -> N`; `param H` -> `param H -> name=.. type=..|null`;
`nannotations H`, `annotation_idx H IDX` (prints `past-end` when the library returned a non-NULL pointer for `IDX == count`);
`viewproj E`, `world E`; `technique E NAME` (`found passes=N|null`); `current_technique E`; `tbegin E T` (`-> passes`), `tend E T`,
`pbegin E T IDX` (`-> 0|1`), `pbegin_name E T NAME`, `pend E T`, `update E`, `loop E NAME` (one `gs_effect_loop` call, `-> 0|1`),
`loop_all E NAME [MAX]` (calls until false, prints `iterations=N`), `pass_by_idx E T IDX` (`name=..|name=@null|past-end|null`),
`pass_by_name E T NAME` (`index=N|null`). Setters (no output): `set_bool|set_float|set_int H V`, `set_vec2 H x y`, `set_vec3`, `set_vec4`,
`set_color H 0xAARRGGBB`, `set_matrix4 H` + 16 floats, `set_matrix4_null H`, `set_val H HEX|-|@null`, `set_texture|set_texture_srgb H ID`
(`ID` -1 = NULL), `set_next_sampler H ID`, `set_default H`. Getters: `get_val H` / `get_default_val H` print `size=N hex=..|data=null`
(texture values: `texture=ID srgb=B`), `flags H` prints `changed=.. cur_size=.. next_sampler=ID|-1`.

**Crash capture.** `try CMD args...` runs one command in a forked child (its state changes are discarded) with a 10 s alarm and prints
`try-begin`, the child's output, and `try-end ok|crash|hang`. A conforming implementation MUST NOT crash or hang: a `try-end crash` (or
`hang`) in the expected file matches `try-end ok` in the actual output, and everything between the corresponding `try-begin` and
`try-end` is ignored. `ignore CMD args...` runs the command with its output discarded and prints only `ignore-begin` / `ignore-end`
(used where the reference reads uninitialised memory, so even its failure is not deterministic). The runner sets `M_PERTURB`
(glibc) so that such reads are reproducible while generating vectors.

**Comparison notes.** Lines `... -> past-end` (from `pass_by_idx` / `annotation_idx` when the index equals the count) in the
expected file match `... -> null` in the actual output (spec D-7). Everything else is byte-identical except `log` lines (level only).

## transitions runner (spec S-029)

Area `transitions` (`gen/transitions_runner.c`, `gen/transitions_stubs.c`, `gen/transitions.sources`).
The runner `#include`s the real plugin sources (`plugins/obs-transitions`) and replaces libobs/graphics by a
recording fake. Every fake graphics/core call prints one **event line** `  ev <text>` (two spaces, `ev`, one
space); the order and arguments of events are part of the expected output. The runner is run from any directory;
data files (`.effect`, `luma_wipes/`) are found relative to the source tree. Floats are printed `%.9g`
(NaN printed as `nan`); compare numerically with relative tolerance 1e-6 if your float arithmetic differs
(the vectors were produced with IEEE `float`; the sign of zero is printed, `-0`, and is significant when
comparing text). Module text keys are returned unchanged (`obs_module_text(K) == K`).

Names: transitions are named by the script (`NAME`); private media sources created by the stinger are
`media#K`, K = 1,2,... in creation order since the last `reset`. Texture handles are small integers (`-` = NULL);
placeholder/transparent texture = 9000 unless `gfx placeholder=` changes it.

Settings tokens: `TYPE:KEY=VALUE`, TYPE is `int`, `str`, `bool` (0/1) or `dbl`.

| Command | Effect / output |
|---|---|
| `reset` | forget all sources, registry and fake state (defaults below) |
| `load [quiet]` | call `obs_module_load()`; without `quiet` prints `load OK registered=N`, per source `  id=.. type=.. flags=0x.. version=.. name=".."`, `    callbacks: ...`, module description and `ver_matches` |
| `create NAME ID [SETTINGS..]` | object creation sequence of the core (user settings applied to a new settings object, then `get_defaults` fills defaults, then `create(settings, source)`); prints `create NAME ID`, events, `  instance=0/1` and the `fields` line(s) of the instance (swipe: a note, its memory is uninitialised until the first update) |
| `settings NAME SETTINGS..` | `obs_source_update(source, partial settings)`: settings merged; because transitions have `OBS_SOURCE_VIDEO` the update is *deferred*: prints `settings NAME` and `ev source_update NAME deferred (count N)` |
| `tick NAME [sec]` | models `obs_source_video_tick`: if a deferred update is pending call `update(data, settings)` and clear the count, then call `video_tick(data, sec)` if present; prints `tick NAME`, events, `fields` |
| `fields NAME` | print instance fields (see spec) and the list of private sources with refs/active-child counters |
| `destroy NAME` | `destroy(data)` |
| `props NAME [null]` | `get_properties(data)` (`null`: called with NULL data); dumps flags and the property tree: `prop NAME type=T desc=".." visible enabled [long=".."] [numeric limits] [path info]`, list properties followed by `list type= format= count=` and `item I name=".." value=..`; group content indented |
| `modify NAME PROP` | find PROP (also inside groups) in the last `props` tree and call its modified callback with the instance's current settings; prints `modify PROP -> RESULT` and the dump again |
| `render NAME` | `video_render(data, NULL)` with the fake render helper inputs set by `view` |
| `mix NAME MIXERS CHANNELS RATE t..` | `audio_render(...)`; the fake `obs_transition_audio_render` evaluates both mix callbacks for each t (`ev   mix t= a= b=`, t as float32), writes `gfx base` into every enabled mixer/channel and `gfx ts_out` to `*ts_out`; then prints `  result= ts_out=` and `out mixM chC first= last=` for mixers 0..5, channels 0..1 (buffers pre-filled with 7) |
| `start NAME` / `stop NAME` | `transition_start` / `transition_stop` (stinger); prints fields |
| `cspace NAME TR COUNT P..` | `video_get_color_space(data, COUNT, [P..])` with `obs_transition_video_get_color_space` returning TR (numeric `enum gs_color_space`: 0 sRGB, 1 sRGB_16F, 2 709_EXTENDED, 3 709_SCRGB); prints the result |
| `missing NAME [IDX PATH]..` | stinger `missing_files`: count and paths; each pair `IDX PATH` issues the file's callback with the new path and prints the resulting `path`/`track_matte_path` settings |
| `ease T..` / `smoothstep MIN MAX X` | `cubic_ease_in_out` / the fade-to-color `smoothstep` helper |
| `effects` / `assets` | length and FNV-1a/64 of the six `.effect` files; name, PNG IHDR size/depth/color type, byte length and FNV of each `luma_wipes/*.png` (sorted by name) |
| `fork CMD..` | prints `fork CMD`, runs one command in a child process (its state changes are **discarded**, only its output counts); prints `crash` if it died abnormally. A conforming implementation never crashes |
| `echo ..` | |

Fake-world commands (no output, except `src` of an unknown name prints `src NAME missing`): `gfx key=value..` with keys `space` (srgb, srgb_16f, 709_extended, 709_scrgb;
`gs_get_color_space()`, default srgb), `fb_srgb`, `linear`, `effect_fail` (effect creation fails), `white` (SDR white
level, default 300), `begin_ok` (texrender begin result, default 1), `direct_ret` (render_direct result, default 1),
`audio_ret` (audio render helper result, default 1), `ts_out`, `base` (audio helper output timestamp / sample value,
default 0), `tr_space`, `placeholder`; `view T A B CX CY` (inputs of the fake video render helper: progress,
textures, size); `src NAME key=value..` (`w h active state pending ats space dur_ns frames fill time`; NAME is a
transition or `media#K`); `mdef key=value..` (same keys, defaults for private sources created afterwards);
`missing_file REL` (module file REL is not found); the command used for module-file lookups never prints paths.

## audiofilters runner (spec S-027)

`gen/audiofilters_runner.c` links the real `plugins/obs-filters` audio filters (`gain`, `invert-audio-polarity`,
`compressor`, `limiter`, `expander` + upward compressor, `noise-gate`, `async-delay`, `eq`, `noise-suppress`) through
one wrapper per source file (`gen/audiofilters_w_*.c`, which `#include` the plugin and print its private state), the
real `obs-data.c`/`obs-properties.c`, and a fake OBS core / recording DSP libraries (`gen/audiofilters_stubs.c`,
fake headers in `gen/audiofilters_inc/`). Filters are driven exactly like the host drives them: through the
`obs_source_info` callbacks. One command per line (script syntax as above). Filter names: `gain invert compressor limiter
expander upward gate delay eq ns ns2` (`ns` = noise-suppress v1, `ns2` = v2). `ID` names a filter instance.

**Fake host.** `audio CH RATE` (fake audio output: channel count and sample rate, read by `create`/`update`),
`clock NS` / `advance NS` (fake `os_gettime_ns`, initial 10 000 000 000), `source NAME audio|video [mono]` (registers a
fake source, also visible to `obs_get_source_by_name` / `obs_enum_sources`; the optional word sets its speaker layout
to mono, `unknown` is also accepted), `source_dead NAME` (weak references to it stop resolving),
`refs NAME` (prints strong reference count, installed capture callbacks, live weak references), `events on|off` (print
one `host ...` line per fake host/library call, default off), `resampler_frames N` (force the fake resampler's output
frame count, 0 = formula).

**Settings.** `data D` creates an empty settings object; `set D KEY int|double|bool|string VALUE` sets a user value,
`setdef D KEY TYPE VALUE` a default; `dump D` prints every item sorted by key as
`KEY user=U default=D TYPE value def=value` (doubles `%.17g`); `@ID` names the live settings object of instance ID
(accepted wherever a data name is). `defaults FILTER D` calls the filter's `get_defaults` on D.

**Instances.** `create ID FILTER [D|-] [parent=SRC] [nodefaults]` copies D's user values into a new live settings
object, calls `get_defaults` on it (unless `nodefaults`) and then `create` (host order); prints `create ID -> ok|null`.
`parent=` sets the filter's parent source. `update ID D|-` applies D's user values to the live settings and calls
`update`; `reset_settings ID D|-` clears the live settings first (`obs_data_clear`). `state ID` prints the private derived
state (`state KIND key=value ...`, floats `%.9g`). `tick ID` calls `video_tick(0.0)`, `destroy ID`, `remove ID`
(`filter_remove` with the parent), `info FILTER` prints the registration data, `props ID` / `props_null FILTER` call
`get_properties` (with the instance data / NULL) and dump the properties, `props ID modified PROP D` additionally calls the
property's modified callback with settings D and dumps again.

**Audio.** `process ID FRAMES [ts=N] [out=SPEC] GEN...` builds one plane per GEN (`-` = NULL plane; at most 8; planes are
allocated with exactly FRAMES floats) at the instance's running sample position `pos` (starts at 0, advances by FRAMES per
process call, `seek ID N` sets it), calls `filter_audio` and prints `process` + input CRCs (FNV-1a over the float bit
patterns with -0 mapped to +0), then `ret null|same|own frames=N ts=T` (`own` = a pointer other than the input) and the
output planes according to `out=`: `all` (default, 16 per line, `[index]` prefix), `none`, `stats` (count, nan flag, CRC, min, max, sum,
sumsq), or `P:FROM:TO:STEP` (plane P only). `sc_push SRC FRAMES MUTED GEN...` calls every audio-capture callback installed on
SRC. `video ID TS` passes a fake video frame (timestamp TS, tag = sequence number, 1-based over the whole script) to `filter_video` and prints the returned
tag; **its line is printed after the call returns**, so `host release_frame` events caused by the call precede it (for `process`, `sc_push` the header line is printed before the call). Generators (value of sample n, n = absolute position; computed in double, rounded to float): `zero`, `const:V`,
`sine:A:F` (`A*sin(2*pi*F*n/rate)`), `step:LO:HI:N0`, `impulse:A:N0`, `pulse:A:N0:LEN`, `ramp:START:STEP`,
`noise:A:SEED` (`A*u`, `u = (mix(n + SEED*0x9E3779B97F4A7C15) >> 40)/2^23 - 1` in u64 wrapping arithmetic, where `mix(x)`: `z = x + 0x9E3779B97F4A7C15; z = (z ^ (z>>30)) * 0xBF58476D1CE4E5B9; z = (z ^ (z>>27)) * 0x94D049BB133111EB; return z ^ (z>>31)`; `SEED` is an integer), `list:v0,v1,...` (zero beyond the list); any of them may carry a suffix `@N0+LEN` (zero outside `[N0,N0+LEN)`).
Values `nan`, `inf`, `-inf` are accepted by `strtod`.

**Printing and comparison.** Floats print `%.9g` with `nan`, `inf`, `-inf` normalised and `-0` printed as `0`. Compare
line by line; split each line at spaces, `=`, `[`, `]`, `,`, `:`; tokens that parse as a number are equal when
`|a-b| <= 1e-5*max(|a|,|b|) + 1e-7` (integers, i.e. tokens without `.`/`e`/`nan`/`inf`, are compared exactly), all other
tokens exactly. Exceptions: the CRC values (`in-crc:` lists and `crc=` fields) are diagnostics only (a generator may differ in the
last float bit) and are not compared. `try CMD...` runs one command in a
forked child with a 10 s alarm and prints `try-begin`, the child's output, `try-end ok|crash|hang`; as in the other
runners a conforming implementation must not crash, a `crash` in the expected file matches `try-end ok` and the
child's output between the markers is ignored.

**Output formats (audiofilters).** `create` and `destroy` print their line after the call (host events first). `info F`: `info F id=ID version=V type=1 flags=0xH name="KEY"` then
`  has create=B destroy=B update=B filter_audio=B filter_video=B video_tick=B filter_remove=B get_defaults=B get_properties=B`.
`dump D`: header `data D (sorted by key)` then per item `  KEY user=U default=D string "S" def="S"` / `int N def=N` / `double X def=X` /
`bool B def=B` (`U`: has a user value, `D`: has a default; `def=` is the default value or 0/""). `props`: `props count=N`, per property
`  prop NAME type=T desc="D" enabled=E visible=V` + (float) ` min= max= step= numtype=N suffix="S"|"(null)"`, (int) the same with integer
min/max/step, (list) ` listtype=N format=N items=N` followed by lines `    item I name="N" value="V" disabled=D`; `T` is `bool int float text path
list ...`; `numtype` 0 = plain, 1 = slider; list type 2 = `OBS_COMBO_TYPE_LIST`, format 3 = string. `modified PROP D` prints `modified NAME -> 0|1`
and dumps again. `state` lines (fields in this order, floats `%.9g`):
- `state gain channels= multiple=`; `state invert`
- `state limiter channels= rate= buf_len= threshold= attack_gain= release_gain= output_gain= slope= envelope=`
- `state compressor` the same fields with `ratio=` before `threshold=`, then ` sidechain_name=NAME|(null) weak=0|1 max_sc_frames=N sc_queue0=BYTES check_time=N`
- `state expander channels= rate= detector=0|2 is_gate= is_upwcomp= buf_len= ratio= threshold= attack_gain= release_gain= output_gain= slope= knee= ch0[ env= runave= gain_db= ] ch1[ ... ]`
- `state gate channels= is_open= sample_rate_i= open_threshold= close_threshold= decay_rate= attack_rate= release_rate= hold_time= attenuation= level= held_time=`
- `state eq channels= lf= hf= low_gain= mid_gain= high_gain=`
- `state delay interval= samplerate= queued= last_video_ts= reached= reset_video= reset_audio=`
- `state ns level= latency= frames= channels= use_rnnoise= has_mono_src= last_ts= allocated=SPEEX,RNN resamplers=FWD,BACK in0=BYTES out0=BYTES info=BYTES`
  (`info` = bytes in the info queue, 16 per entry; `buf_len` of the compressor/limiter/expander is the internal envelope buffer length in frames: initial `rate*10/1000`, grows to the largest block seen).
`process` prints `process ID frames=N ts=T pos=P in-crc: C...`; `video` prints `video ID ts=T tag=N -> null|tag=N ts=T`; `create` prints
`create ID -> ok|null`; `destroy` prints `destroy ID`; `sc_push` prints `sc_push SRC frames=N muted=M callbacks=K`. Events (`events on`) print
`host NAME args...` lines, the names and arguments being exactly those of the fake in `gen/audiofilters_stubs.c` (`get_source_by_name N -> ok|null`,
`add_audio_capture_callback N`, `remove_audio_capture_callback N`, `source_release N`, `weak_get_source N -> ok|null`, `weak_release N`,
`release_frame parent=P tag=T ts=TS`, `speex.init id= frame_size= rate=`, `speex.ctl id= req= level=`, `speex.run id= level= in0= in1= inN=`,
`speex.destroy id=`, `rnnoise.create id=`, `rnnoise.process id= in0= in1= in479=`, `rnnoise.destroy id=`, `resampler.create id= src= dst= channels=`,
`resampler.resample id= in= out=`, `resampler.destroy id=`; ids count up per library from 0 per script).

## rtmp runner (spec S-031)

`gen/rtmp_runner.c` (+ `gen/rtmp_stubs.c`, `gen/rtmp_stubs.h`), scripts `scripts/rtmp/*.script`: the RTMP protocol engine of the
vendored librtmp (`rtmp.c`, `parseurl.c`, with `amf.c`/`log.c`). **Build requirements** (see `gen/rtmp.sources`): MbedTLS 3.x headers in
`/usr/include/mbedtls3` and libraries in `/usr/lib/mbedtls3` (the production configuration is `-DUSE_MBEDTLS -DCRYPTO`; the runner
uses it for HMAC-SHA256/MD5/base64), and `run.sh` must be started from `test/conformance` (the `-I../../shared/happy-eyeballs` flag is
relative). The fake transport replaces `send`, `recv`, `setsockopt`, `times`, `rand`, the happy-eyeballs connector and the CA loader.

**Fake environment (inputs of the engine, spec S-031 section 3).** `clock MS` sets the millisecond clock (`RTMP_GetTime` returns it;
use multiples of 10); `seed N` sets the generator state: `state = (state * 1103515245 + 12345) mod 2^31`, `rand()` returns the new
state. Bytes the server "sends" are queued with `feed`; when the queue is empty a read fails with timeout (`empty eagain`, default),
EOF (`empty eof`) or `ECONNRESET` (`empty reset`). `recvcap N` limits the bytes returned per read call; `feed_cut` marks a connection
boundary (a reconnect continues after it). `sendfail AFTER ERRNO` fails every send once AFTER bytes were sent on the connection,
`sendeintr` fails the next send with EINTR, `hevfail ERRNO` makes the connector fail with ERRNO.

**State/action commands.** `new` (RTMP_Alloc+RTMP_Init, resets the fake), `parseurl URL`, `playpath STR` (guarded child),
`seturl URL`, `enablewrite`, `addstream KEY|-`, `set NAME VALUE` (outchunk, chunkinfo, flashver, swfurl, swfurl_is_tcurl, pageurl, user,
pass, auth, authflag, usher, subscribe, live, playlist, bufx, swfv, token, swfsize, swfhash HEX, encoding, sendenc, clientbw, bufms,
seek, stop, sendcounter), `connect` (RTMP_Connect), `connectstream`, `read` (one RTMP_ReadPacket), `client` (ReadPacket +
RTMP_ClientPacket), `drain` (OBS reader: ReadPacket until failure), `write HEX IDX`, `writetag TYPE TS LEN IDX [FLVHDR]`,
`sendpacket CH FMT TYPE TS INFO2 HEX QUEUE`, `ctrl T OBJ TIME`, `sendfn createstream|pause A B|rpause A|seek A|serverbw|clientbw|
updatebuf`, `deletestream IDX`, `close`, `state`, `chts CH`, `gettime`, `feed HEX..`, `feed_echo` (a 1536-byte S2 equal to the C1
the engine sent), `feed_digest 0` (S2 with the digest signature for the client digest scheme), `feed_s1 ALG UPTIME VERSIONHEX`
(S1 with a valid server digest in scheme ALG), `try CMD`/`ignore CMD` (fork + 5 s alarm; `try-end ok|crash|hang`, `ignore` hides the
output; a conforming implementation must not crash; crash/hang in the expected file match ok).

**Output.** Everything the engine sent during a command is decoded by the *runner* (not the engine) with this state machine and
printed *before* the command's result line: after `connect`, the first 1537 bytes are `out c0c1 HEX` and the next 1536 `out c2 HEX`
(printed once complete); the rest is a chunk stream decoded with chunk size 128, changed when a complete type-1 message was sent
(chunk header formats 0..3 from the first byte, 1/2/3 byte basic header; message header inherited per chunk stream id; the 4 byte
extended timestamp is present if the 24 bit field is 0xffffff in a type 0/1/2 header and in every continuation chunk; a type-3 chunk
that starts a message has none). Each complete message prints
`out msg csid= fmt= chunks= ts= type= sid= len= hdr= body=` (`fmt` and `hdr` = the first chunk's format and header bytes, `ts` = the
timestamp field (extended value if present), `chunks` = number of chunks). A stale decoder that cannot parse prints nothing for the
incomplete tail. On a reconnect (second and later connector call) the previous output is flushed, a `reconnect` line is printed and the
handshake decoder restarts. Library log messages of level INFO or worse print as `log error|warning|info TEXT`; **only the level is
compared**. `read` prints `read ret=` and `pkt ready= ch= fmt= type= ts= abs= info2= len= read= [body=]`; `state` prints the engine
counters and the call queue `calls=[name:txn,..]`.

## rtmpservices runner (spec S-026)

`gen/rtmpservices_runner.c` links the real `plugins/rtmp-services` sources (`rtmp-common.c`, `rtmp-custom.c`,
`rtmp-services-main.c`, `service-specific/*.c`) with the real `obs-data.c` / `obs-properties.c`. Everything else is
faked in `gen/rtmpservices_stubs.c`: an in-memory file system, module data/config dirs, the en-US strings, the
protocol registry, `obs_get_video_info`, a fake clock (sleeps advance it, so "wait up to N seconds" loops are instant),
the file-updater and libcurl. Run `./run.sh` from `test/conformance` (the `.sources` file uses relative `-I`; it also needs
`-Wl,--allow-multiple-definition` so the fake `os_file_exists`/`os_safe_replace` win over `gen/stubs.c`). Scripts:
`scripts/rtmpservices/*.script`; corpus: `scripts/rtmpservices/data/services-ok.json` (hand-written, all fields). The
real production `services.json` is only used by script `27` (path relative to the script directory).

Default fake environment: data dir `/mod/data`, config dir `/cfg/rtmp-services/` (a trailing slash; paths are plain
concatenation), registered protocols `RTMP RTMPS`, no video info, locale en-US loaded, clock 0, no routes.
A `-` argument means "no settings object / no directory / empty protocol list"; NULL strings print as `@null`.

| Command | Meaning |
|---|---|
| `echo T...` | prints `echo T...` |
| `fs-put PATH TEXT` / `fs-load PATH FILE` / `fs-del PATH` | in-memory file (FILE is relative to the script dir) |
| `fs-cat PATH` / `fs-ls` | print content (quoted, `@missing`) / `fs PATH len=N` sorted |
| `dirs DATA CONFIG` | module data dir and config dir (`-` = NULL: `obs_module_file` / `obs_module_config_path` return NULL). `obs_module_file(f)` is NULL when `DATA/f` is not a file (the empty name returns DATA itself) |
| `protocols [NAME...]` | registered output protocols (exact, case-sensitive); `-` or none = empty |
| `video-info W H FPSNUM FPSDEN` / `video-info -` | `obs_get_video_info` answer (output size, fps) / failure |
| `locale en-US` / `locale none` | load / free the module locale (none => labels are the raw keys) |
| `advance MS` / `clock` | move the fake clock (delivers due updater answers) / print it |
| `curl-route URL STATUS BODY` / `curl-clear` | fake HTTP GET answer (STATUS 0 = transfer failure; body delivered in 5-byte chunks). Prints `fake curl GET url timeout=.. verifypeer=.. verifyhost=..` and `fake curl -> status=N bytes=N / no-route / connect-failure` |
| `updater-route URL DELAY_MS BODY` | answer for a *single-file* updater created later for URL: delivered after DELAY_MS of fake time (0 = at creation). Prints `fake update_info_create[_single] ...`, `fake updater-deliver URL len=N`, `fake update_info_destroy URL pending=0|1`, and the file writes `fake write PATH len=N`, `fake replace PATH` |
| `module-load` / `module-unload` | `obs_module_load` / `obs_module_unload` (prints `fake proc_handler_add`, `fake update_info_create`, `fake obs_register_service id=..`, `module-load -> 1 name=..`) |
| `confirm NAME BODY` | call the package updater's confirm callback; prints `confirm NAME -> 0|1` |
| `proc-call NAME SECONDS` | run the registered refresh proc whose declaration contains NAME with `seconds` |
| `settings S [JSON...]` | create (or replace) the settings object S; the remaining tokens are re-joined with single spaces and parsed as JSON (parse errors log and give an empty object) |
| `set S KEY str\|int\|bool\|dstr VALUE` / `unset S KEY` / `show S` | modify (`dstr` = default value) / remove user value / dump `settings S` with `KEY = "str" \| int N \| double D \| bool B \| object \| array count=N`, keys sorted |
| `new SVC rtmp_common\|rtmp_custom S` / `update SVC S` / `destroy SVC` | `create` / `update` / `destroy` on the service vtable |
| `get SVC FIELD...` | print `get FIELD = VALUE`; FIELD one of `protocol url key user pass can_connect connect:N resolutions maxfps maxbitrate vcodecs acodecs`. Absent optional callbacks give the host default (`[]`, 0, `video=0 audio=0`, `@null`). `resolutions` prints `[CXxCY,..]`, `maxbitrate` prints `video=N audio=N` (both pre-zeroed), codecs print `["a","b"]` or `@null` |
| `all SVC` | every field above (connect ids 0,2,4,6,8,10) |
| `apply SVC V A` | `apply_encoder_settings` with settings V / A (`-` = NULL; not forwarded when both are `-`); prints `apply SVC forwarded=0\|1` then the dumps of V and A |
| `props P SVC S` / `props-raw P SVC` | `get_properties` (+ `obs_properties_apply_settings` with S for `props`); prints the property list: `prop NAME type=bool\|int\|float\|text\|list.. desc="..." visible=B enabled=B [text_type=N]` and for lists `combo=N format=N items=N` followed by `item I name=.. value=.. disabled=B` |
| `props-show P` / `props-apply P S` / `props-free P` | re-print / re-apply settings / destroy |
| `modify P PROP S` | `obs_property_modified`; prints `modify PROP -> 0\|1` |
| `guard-begin [quiet]` ... `guard-end` | run the enclosed commands in a forked child (state changes are discarded); prints `guard-end ok` or `guard-end crash`. `quiet` discards the child's output. A reference crash MUST be matched by a non-crashing implementation (`guard-end crash` in an expected file equals `guard-end ok`; lines between `guard-begin` and `guard-end` are ignored then) |
| `smoke PATH` | structural summary of a catalogue file in the fake fs. Prints `smoke format_version=N`, `smoke services>0=B`, then `smoke keys service:`, `smoke keys server:`, `smoke keys recommended:`, `smoke keys bitrate-matrix:` each followed by the **sorted union** (byte order, space separated, leading space) of the object keys seen in all services / all server objects / all `recommended` objects / all `bitrate matrix` rows; then for every service object that has a string `name` (whether or not it is currently available): create `rtmp_common` from `{"service":name}`, open the properties (apply settings), take the server list, set `server` to the value of server item 0 (item 0 must exist, else counted as "no server"), update, and read url/protocol/key/codecs. Prints `smoke listed>0=B crashes=N` (N = services whose processing crashed, always 0 for a conforming implementation) and, **only if RTMPS is registered**, `smoke all-have-servers=B` (no service had an empty server list) and `smoke all-have-url=B` (every service except `Twitch`, `Amazon IVS`, `Nimo TV`, `SHOWROOM`, `Dacast` had a non-empty url). B is 0/1. The reference forks one child per service |

**Comparison.** `log <level> ...` lines compare by level only. Fake lines (`fake ...`) are compared byte for byte.

Event catalogue (text after `  ev `; `{x}` = value, floats `%.9g`, `tex` = integer or `null`):

- graphics: `enter_graphics`, `leave_graphics`, `effect_create file=@data/{name}.effect` (+ `  -> fail` as a following event
  `  ev   -> fail`), `effect_destroy file=@data/{name}`, `set_texture {param} tex={tex} srgb={0|1}`, `set_float {param} {v}`,
  `set_bool {param} {0|1}`, `set_vec2 {param} {x} {y}`, `set_vec4 {param} {x} {y} {z} {w}`,
  `effect_loop technique={name} pass=0` followed by `draw_sprite tex=null flip=0 w={cx} h={cy}`,
  `enable_framebuffer_srgb {0|1}`, `set_linear_srgb {new} (previous {old})`, `matrix_push|matrix_pop`,
  `matrix_scale3f {x} {y} {z}`, `matrix_translate3f {x} {y} {z}`,
  `clear flags={n} color={r},{g},{b},{a} depth={d} stencil={s}`, `ortho l={} r={} t={} b={} near={} far={}`,
  `blend_state_push|blend_state_pop`, `enable_blending {0|1}`, `get_base_effect {n}`,
  `texrender_create id={n} format={fmt} zs=0` (ids 1.. per reset; sRGB format 3, other spaces 9), `texrender_destroy id={n}`,
  `texrender_begin id={n} cx={} cy={} space={s} -> {0|1}`, `texrender_end id={n}`, `texrender_reset id={n}` or `texrender_reset null`;
  texture handles of texrenders are `100+id`, luma textures `300+k` (k counts successful `image_init_texture` since reset).
  `image_init file={@data/rel|(null)} alpha_mode={n}` then `  -> loaded {w}x{h} depth={d} color_type={c}` or `  -> failed`;
  `image_free loaded={0|1}`; `image_init_texture -> texture {n}` or `image_init_texture (not loaded)`.
- core: `transition_enable_fixed {0|1} duration_ms={n}`, `source_update {name} deferred (count {n})`,
  `transition_video_render|transition_video_render2 callback={yes|none} placeholder={tex}` then either `  default render (no callback)`,
  `  callback skipped (size 0)` or `  callback a={tex} b={tex} t={t} cx={} cy={}`; `transition_video_render_direct target={A|B} -> {0|1}`;
  `transition_audio_render mixers=0x{hex} channels={n} rate={n}` and `  mix t={t} a={a} b={b}`;
  `source_create_private media#{k} id=ffmpeg_source name="{n}"|name=(null)` followed by `  setting local_file="{p}"` /
  `  setting {key}={0|1}` for keys present in the settings, in the order local_file, hw_decode, looping, full_decode,
  is_stinger, is_track_matte; `source_release {label} refs={n}`, `add_active_child {parent} <- {child}`,
  `remove_active_child ...`, `source_set_muted {label} {0|1}`, `source_set_monitoring {label} {0|1}`,
  `source_video_render {label}`, `source_get_audio_mix {label} fill={v}`, `proc_call {label} {name}` or
  `proc_call (null handler) {name} -> false`. A label is the transition name or `media#K`.
- The fake private sources report `get_duration` = `dur_ns` and `get_nb_frames` = `frames` of the scripted source, and a
  new private source copies `mdef` at creation. `obs_source_get_width/height(NULL)` is 0.
- Missing-file lookups: the six `.effect` names and `luma_wipes/{name}` resolve to `@data/...` when the file exists in the
  plugin data directory and are NULL otherwise (or when listed by `missing_file`).
- `tick` also prints (stinger with track matte enabled) `texrender_reset` events for stinger_tex then matte_tex after a
  pending update ran. `create` of a swipe always prints `  fields (uninitialised until the first update)`.
- `log` lines: first two words compared only (`log error`/`log warning`).

## audiomix runner (spec S-032)

Area `audiomix`: mixer callback, per-source audio ingestion/render, faders and volmeters. The runner links the real
`obs-audio.c`, `obs-audio-controls.c` and `obs-source.c` against a fake core (`gen/audiomix_stubs.c`): fake clock,
fake 1024-frame audio output (float planar), a fake resampler (zero-order hold; **hook contract only**), scripted
sources. All floats are printed with `%.9g` (`inf`, `-inf`, `nan`, `-0` printed as `0`); comparison tolerance is
**relative or absolute 1e-5** for every float except values that are sums of up to 1024 samples (`sum=`), which use
**1e-4 relative**; integers, flags and timestamps compare exactly. Log lines: level only.

State: a fake core (`init`), a registry of named sources, numbered faders (0..31) and volmeters (0..31), the
`mixers` argument for ticks, and the output of the last tick (`mixbuf[6][8][1024]`, zeroed before every tick like
the audio thread does).

| Command | Effect |
|---|---|
| `init RATE SPEAKERS [MAX_TICKS [FIXED]]` | reset core: output rate, speaker layout enum value, `max_buffering_ticks` (default 45), `fixed_buffer` (default 0); forgets sources, buffering state, resampler hook; video mixes are NOT reset (call `init` once per script, first) |
| `clock NS` | fake `os_gettime_ns` |
| `source NAME KIND` | create a source, `KIND` = `input` (async audio), `custom` (audio_render + composite flag + children), `custom_audio` (audio_render, not composite), `submix` (audio_mix), `filter` (audio filter). Sources are registered in the global audio-source list at the head |
| `custom SRC OK TS VALUE` | scripted render/submix result: success flag, reported timestamp (`tick` = start of the tick being mixed, else ns), sample value (written as `VALUE*(ch+1)` ADDED into each requested mix; submix writes it) |
| `child PARENT CHILD` | parent enumerates child via `enum_active_sources` (custom only) |
| `filter SRC FILTER GAIN DROP` | append filter to SRC's filter list; GAIN multiplies the samples, DROP=1 returns NULL |
| `set SRC PROP VALUE` | `volume mute balance sync_offset mixers` use the public setters; `enabled active removed monitoring_enabled monitoring_type force_mono ptm ptm_delay ptt ptt_delay async_unbuffered async_decoupled audio_ts pending` set the field (`active` = activation refcount) |
| `hotkey SRC ptm\|ptt 0\|1` | push-to-mute / push-to-talk hotkey callbacks |
| `video_mix MIX_AUDIO` / `channel MIX IDX SRC\|-` | add a video mix with a view (`mix_audio` flag); put a source in channel IDX of video mix MIX |
| `dup_source SRC\|-` | `monitoring_duplicating_source` |
| `resampler OFFSET_NS FAIL` | fake resampler: reported ts offset, creation failure |
| `output SRC RATE SPEAKERS FRAMES TS GEN A B C` | `obs_source_output_audio` with float-planar data, generated per channel c with gain `1-0.25c`: `const A`, `sine FREQ AMP START_INDEX` (`AMP*sin(2*pi*FREQ*(START+i)/output_rate)`), `impulse IDX VALUE`, `ramp STEP START` (`START+STEP*i`) |
| `timeline NS` | start time and sample counter for `run` |
| `run N MIXERS` | N ticks like the audio thread: tick k covers `[T0+ns(1024k), T0+ns(1024(k+1)))`, `ns(f)=f*1e9/rate` (mul_div64); counter continues over several `run` commands |
| `tick START END MIXERS` | one explicit `audio_callback` |
| `render SRC MIXERS [TICKSTART]` | call `obs_source_audio_render` directly, print per-mix source output buffers |
| `dump_mix MIX CH FROM N` | print samples of the last tick's mix output |
| `state SRC`, `mixer_info SRC`, `task NAME`, `sig_halve SRC` | print internal state; queue an audio task; connect a "volume" signal handler that halves the volume in the calldata |
| `fader_new/set/get/attach/detach/cb/destroy`, `fader_conv TYPE def\|db V`, `fader_null` | fader API (see spec) |
| `meter_new/attach/detach/type/channels/cb/feed/misaligned/destroy` | volmeter API; `meter_feed ID SRC MUTED NPLANES FRAMES GEN A B C` calls the audio-capture callback directly |
| `db mul_to_db\|db_to_mul V` | exported conversions |

Tick output: `tick I ret=B out_ts=N wait=N total=N bufq=N`, `order ...| roots ...` (`*` = duplicated),
then one `mix M CH first= last= sum= peak=` line per non-silent mix/channel. A `task` line is printed by the task
callback while the tick runs, i.e. before the `tick` line of that tick.

## imagesource runner (spec S-030)

Drives the image, colour and slideshow (v1, mk2) sources through their `obs_source_info` callbacks against a
scripted fake host (spec S-030 section 3 defines the host exactly; a clean-room implementation must provide the
same host semantics). Source ids are the *versioned* ids: `image_source`, `color_source`, `color_source_v2`,
`color_source_v3`, `slideshow` (v1), `slideshow_v2` (mk2). Source names are script-chosen; JSON arguments are
obs-data JSON (`-` = none). Output lines: `ev ...` host events, `gfx ...` graphics calls, `state ...` plugin state,
`log <level> ...` (compare level only), `tick I clock=NS`, and command results. Floats `%.9g`.

| Command | Effect |
|---|---|
| `registered` / `info ID` / `defaults ID` | registry list; descriptor (flags hex, icon, callbacks present); default settings JSON |
| `create NAME ID [JSON]` | public source; `update NAME JSON` = `obs_source_update` (deferred to next tick for video sources) |
| `tick SECONDS [N]` | N host frames: clock += round(f32(SECONDS)*1e9) ns, then every source in creation order is ticked |
| `clock NS`, `core srgb_fb\|linear_srgb\|task_auto 0\|1`, `core base_size W H` | host inputs |
| `rand V...` / `rand_add V...` | scripted `rand()` results (exhaustion prints `fatal ...` and exits 3) |
| `fs_file PATH MTIME`, `fs_rm PATH`, `fs_dir PATH ENTRY...` (`name/` = directory) | scripted file system |
| `img PATH W H`, `img_anim PATH W H LOOPMAX FILESIZE DELAY_CS...` | scripted decoder |
| `refs NAME SHOW ACTIVATE` | set show/activate reference counts (propagates to active children) |
| `media NAME play\|pause\|restart\|stop\|next\|previous`, `mediastate NAME` | queue media action (run at next tick) / print state |
| `hotkey NAME HOTKEYNAME 0\|1`, `proc NAME PROC`, `connect NAME SIGNAL` | hotkey press, procedure call, signal logger |
| `render NAME`, `size NAME`, `state NAME`, `props NAME`, `props_type ID`, `colorspace NAME`, `missing NAME [IDX NEWPATH\|@null]`, `tasks`, `destroy NAME` | as named |
| `isolate CMD...` | run one command in a forked child; prints `isolate ok` / `isolate crash` / `isolate exit N` |

## videofilters runner (spec S-028)

Area `videofilters`: the 13 video filters of `plugins/obs-filters` (real C sources, linked) running against the real `obs-data.c` /
`obs-properties.c` and a **recording fake of the OBS core and of the graphics API** (`gen/videofilters_stubs.c`). Every call a filter makes
into the graphics API or the host is printed as one line `call NAME args` (floats `%.9g`, no pointers: effects `eN`, base effects
`base:NAME`, textures `tN`, texrenders `rN`, samplers `sN`, parameters `eN:name`). Effects are not compiled: the `.effect` file is
scanned for `uniform TYPE NAME` and `technique NAME`; looking up a missing parameter prints `-> null`, `tech_end` prints `exists=0|1`.
Run `run.sh` from `test/conformance` (effect files are read relative to it). Float comparison tolerance: relative 1e-6; `model` lines 1e-4
(the `model` command is the **spec author's own reference implementation of the shader pseudo-code**; it is not produced by the C filters).

**Host model (inputs).** `video W H FPS_NUM FPS_DEN`, `sdr_white NITS`, `space NAME` (current graphics colour space), `time NS`
(frame time), `knob NAME 0|1` (`effect_fail`, `begin` = result of `process_filter_begin*`, `texrender_fail`, `tex_fail`, `fb_srgb`).
Colour spaces: `SRGB SRGB_16F 709_EXTENDED 709_SCRGB`. `source S BW BH W H SPACE FLAGS` creates a plain source (FLAGS: `video`, `async`,
`custom_draw`, comma separated or `-`); `resize S BW BH W H`, `srcspace S SPACE`, `srcflags S FLAGS` change it.
`data D`, `dset D KEY int|dbl|str|bool VALUE`, `ddump D` build settings; string values starting with `@TMP/` are expanded to a file
in `$CONFORMANCE_TMPDIR` and printed back as `@TMP/...`. `file NAME TEXT` writes such a file, `touch NAME SEC` sets its mtime,
`image NAME W H FORMAT LOADED ANIMATED` defines a fake decodable image (by file base name; pixel `n` = bytes `n, n>>8, n>>16, 255`;
FORMAT `RGBA|RGBA16F|BGRA`).

**Filters.** `info ID VER`, `defaults ID VER` (version 0 = first version). `filter F ID VER DATA|@null [TARGET [PARENT]]` creates the
filter source (own settings = defaults + DATA) and calls `create` (prints `create F -> ok|null`); `retarget F TARGET PARENT`;
`fset F KEY TYPE VALUE` / `fdump F` edit and print the filter's own settings; `update F DATA` is `obs_source_update` (video sources defer the
update: `call source_update F deferred=1`); `tick F SEC` runs the deferred update (`call deferred_update F`) and then `video_tick`;
`render F`, `size F` (get_width/get_height or `-`), `cs F COUNT SPACE...` (`video_get_color_space`), `show F`, `destroy F`,
`missing F` + `resolve PATH` (missing-files list and its callback).
**Properties:** `props F` creates the property list and dumps it (`props ... end props`: name, type, desc, visible, enabled, int/float limits,
path type/filter/default, list items); `pvis F` prints `name:visible` for every property; `pmod F NAME` runs that property's modified callback with the
filter's settings (`pmod F NAME -> 0|1`); `papply F` runs `obs_properties_apply_settings`.
**Models:** `model chroma2|colorkey_v1|colorkey_v2|luma_v1|luma_v2|reinhard|maxrgb|maxrgb_sdr ARGS...` (argument lists are in the script headers of `26`/`27`)
print `model NAME -> r g b a`.
**Crash capture:** `try CMD...` runs one command in a forked child (`try-begin`, output, `try-end ok|crash|hang`); a conforming implementation MUST NOT crash,
so `try-end crash` in the expected file matches `try-end ok`. `ignore CMD...` runs it with the output and outcome discarded (`ignore-begin`/`ignore-end`):
used where the reference behaviour is undefined (spec S-028 D-1, D-3, D-4, D-6). Log lines: level compared only.

## hotkeys runner (spec S-034)

Scripts: `scripts/hotkeys/*.script`. The runner links the real `obs-hotkey.c` (included, so the 25 ms hotkey-thread step can be called
synchronously) with a **fake platform**: a set of "pressed" keys, a recording signal handler and counted fake weak references.
Output lines: `signal NAME id=ID` for every emitted hotkey signal, `cb LABEL id=ID same_ptr=0|1 pressed=0|1` for a hotkey callback,
`pcb LABEL pair=P hk=ID pressed=B -> RET` for a pair callback, `route id=ID pressed=B` for the re-routing function, `log LEVEL ...`.
Ids print as decimal, the invalid id as `INVALID`. In commands, `id:N` is a literal numeric id and `INVALID` the invalid id; a name is a
handle created by `reg`/`regp`. Comments `#` start only at a token start (do not write `#N`).

- `core new|off|on`: fresh core (strict modifiers on, as in the product) / pretend the core does not exist / bring the same core back.
- `ctx KIND NAME [JSON]` creates a fake context object (KIND `encoder|output|service|source|private-source`; JSON = its `hotkey_data` object).
  `release NAME` = `obs_hotkeys_context_release`.
- `reg frontend H NAME DESC` / `reg KIND CTX|- H NAME DESC` registers hotkey handle `H` (`-` as CTX = NULL object; DESC `-` = NULL) and prints `reg H -> ID`.
  `regp frontend P N0 D0 N1 D1` / `regp KIND CTX|- P N0 D0 N1 D1` registers a pair: prints `regp P -> pair=PID a=ID b=ID`; handles `P.a`, `P.b` name the halves.
  `pairret LABEL 0|1` sets what a pair callback returns (default 1); `reenter LABEL on|off` makes a callback call `hotkey_save` and `set_description` on itself;
  `quiet LABEL on|off` prints only `cb LABEL pressed=B`.
- `info H`, `setname H N`, `setdesc H D`, `pairnames P N0 N1`, `pairdescs P D0 D1`, `unreg H`, `unregp P`, `enum [LIMIT]`, `enumb [LIMIT]` (bindings; prints
  `b IDX hk=ID mods=0xM key=NAME pressed=B modmatch=B hkptr_ok=B`; LIMIT = stop after that many callbacks), `weaks` (live fake weak refs / releases of NULL).
- Bindings: `loadbind H MODS:KEYNAME ...` (`hotkey_load_bindings`; MODS number, e.g. `0x6`), `load H JSONARRAY|-`, `loadpair P JSON|- JSON|-`,
  `ctxload CTX JSON|-`, `save H`, `savepair P both|0|1|none` (prints `save... count=N` followed by one `  item JSON` per element, or `null`),
  `ctxsave CTX` (prints the obs_data JSON or `null`). JSON text is a single quoted token.
- Input: `down KEY...`, `up KEY...`, `upall` change the fake keyboard; `poll [N]` runs N polling steps (prints `poll` before each); `inject MODS KEY 0|1`;
  `strict on|off`; `bgpress on|off`; `reroute on|off`; `router on|off`; `routed H 0|1` (`hotkey_trigger_routed_callback`); `atomic`.
- `setnext ID`, `setnextpair ID` set the id counters (exhaustion tests); `hkfree` = `obs_hotkeys_free`.
- Translations: `trans [size=N] FIELD=VALUE ...` (quote the whole token when VALUE has spaces), `transnull`, `tget KEYNAME DEF|-`, `audiotrans` (6 strings),
  `sitrans` (2 strings), `showtrans`.

Scripts that depend on undefined reference behaviour: `13-defect-unregister-pressed` (use after free in the reference; the vector records the required behaviour;
exclude from sanitizer runs). All other scripts are clean under `-fsanitize=address,undefined`.

## scenemath runner (spec S-033)

Scripts: `scripts/scenemath/*.script`. The runner `#include`s the real `obs-scene.c` and fakes everything around it: a core with a
**main canvas** (default 1920x1080) and one **alternative canvas** (default 1280x720), fake sources whose size the script sets, no graphics,
a **signal log** instead of signal handlers, a **recording fake transition** and a fake clock. Names: sources and scenes share one namespace
(`source`, `scene`), scene items another (`add`, `group`). Numbers are C floats/ints (`strtod`/`strtol`, so `0x..` works for integers).
Floats print `%.9g`, NaN prints `nan` (sign dropped); compare with **abs 1e-3 + rel 1e-5**, integers/flags exactly, `nan`/`inf` as classes.

Environment: `reset`; `canvas W H` (main canvas size); `canvas_alt W H`; `canvas_mix_only W H` (harness-only: changes just the video-mix size);
`absolute on|off` (private-data `AbsoluteCoordinates` for scenes created afterwards); `fail_child on|off` (the core's active-child check fails).
Objects: `source NAME W H [audio]`, `resize_source NAME W H`, `source_removed NAME`, `scene NAME [main|alt|none|custom W H]` (`none` = no canvas),
`scene_resize SCENE W H` (custom size), `add SCENE SOURCE ITEM` (prints `add ITEM ok|fail`), `group SCENE GROUP [ITEM...]` (`obs_scene_insert_group`; the
group item is named GROUP), `ungroup GROUP` (new items inherit the old items' names), `group_add GROUP ITEM`, `group_remove GROUP ITEM`, `remove ITEM`.
Item setters (public API): `pos ITEM x y`, `rot ITEM deg`, `scale ITEM x y`, `align ITEM n`, `bounds_type ITEM n`, `bounds_align ITEM n`, `bounds ITEM x y`,
`bounds_crop ITEM 0|1`, `crop ITEM l r t b`, `set_info ITEM px py rot sx sy align btype balign bx by crop`, `set_scale_nan ITEM`, `order ITEM up|down|top|bottom`,
`order_pos ITEM n`, `reorder SCENE ITEM...`, `reorder2 SCENE TOKEN...` (TOKEN = `item` or `group/item`), `select ITEM 0|1`, `visible ITEM 0|1`, `lock ITEM 0|1`,
`transition ITEM show|hide MS [busy]`, `defer_begin|defer_end|defer_group_begin|defer_group_end|force_update ITEM`, `set_id ITEM n`.
Engine steps: `update SCENE` = what the render thread does before drawing a scene (size-change check, recompute flagged items, prune removed sources, resize groups);
`audio_flush ITEM` = the audio thread applies the queued visibility actions.
Output: `print ITEM` (all stored fields + `draw`/`box` matrices as four rows `[x,y,z,w]`), `get ITEM` (public getters; prints a `get` and an `info` line),
`order_of SCENE` (bottom-to-top names, groups as `g{a b}`), `get_order_pos ITEM`, `scene_size SCENE`, `reorder -> 0|1`, `reorder2 -> 0|1`, `visible ITEM -> 0|1`,
`lock ITEM -> 0|1`, `log` (print and clear signals `signal NAME scene=S item#ID [visible=..|locked=..]` and fake-transition calls `transition NAME ...`), `echo`.
Warnings printed by the core appear as `log warning ...` (level only is compared).

Scripts whose names start with `quirk_` demonstrate reference quirks; `harness_` scripts depend on the fake core and are optional for ports.
`zero_size` produces non-finite matrices (NaN class comparison). All scripts are clean under `-fsanitize=address,undefined` (leaks ignored: the harness never frees).

## modservice runner (spec S-035)

`gen/modservice_runner.c` links the real `libobs/obs-service.c` and `libobs/obs-module.c` (+ `obs-data.c`, `obs-properties.c`, `util/text-lookup.c`,
`platform-nix.c` for dlopen/glob) against a fake core and context layer (`gen/modservice_stubs.c`). Fake plugin modules are **compiled at run time** with `$CC`
(default `cc`) into a per-run temp directory and `dlopen`ed (the runner is linked `-rdynamic`; requires the libobs headers via the runner's own path). The temp directory
is written `@TMP` in all output and in script arguments (a token starting with `@TMP` is expanded). Log lines are printed as `log LEVEL text`; compare the **level only**
(texts are informative; `os_dlopen` messages are cut after the paths). Plugin callbacks print `cb TYPE FN dN ...`, module exports print `ev mod NAME ...`.

**Core:** `core LOCALE|- CONFIG_PATH|-`, `core_null 0|1` (hide the core), `core_modules BIN_TPL DATA_TPL NAME...` (the generated core-module list and the
templates the nix core loader would derive from the executable path), `mkdir P`, `write P TEXT`.
**Fake plugins:** `mkmod NAME PATH OPT...` compiles a module; options `noload noset nover` (omit a required export), `badsym` (unresolved symbol), `ver=api|minor+1|major+1|patch+5|major-1|zero`,
`load=0|1`, `unload postload setloc freeloc getstr loc` (optional exports; `loc` = the default locale macro, `setloc` = custom printing handler, exclusive),
`modname= desc= author=`, `svc=ID,.. out=ID,.. enc=ID,.. src=ID,..` (registered from `obs_module_load` with generic valid descriptors, or with the script type named `ID` for services).
**Service types:** `svc_def NAME ID OPT...` (options `update activate deactivate initialize[=0|1] apply defaults props props_null output_type[=S] resolutions max_fps max_bitrate vcodecs acodecs connect_info can_connect[=0|1] free_type_data create_fail protocol=S no_name no_create no_destroy no_protocol`),
`svc_register NAME [SIZE]` (`full`, a number, `upto:FIELD`, `before:FIELD`), `svc_types`.
**Service objects:** `service_create|service_create_private H ID NAME|- JSON|-`, `service_info/state/settings/list`, `service_update H JSON|-`, `service_destroy|release H`,
`service_get_ref NEW H`, `service_weak W H`, `weak_to_service NEW W`, `weak_addref|weak_release W`, `weak_refs W|- H|-`, `ref_counts H`, `q H what [arg]` (protocol outtype connect_info can_connect fps bitrate resolutions vcodecs acodecs typedata initialize),
`apply_enc H JSON|- JSON|-`, `display_name ID`, `defaults_by_id ID`, `props_by_id ID`, `props H`, `service_module ID`, `service_null_calls`, `null_vcodecs`, `null_acodecs`.
**Output wiring:** `out_new O`, `attach H O`, `out_service O`, `activate H`, `deactivate H 0|1`.
**Other kinds:** `out_register ID FLAGS|- [missing=a,b] [size=N]` (flags S E V A M), `enc_register ID video|audio CAPS|- [missing=..]` (caps P S), `src_register ID input|filter|transition|scene|bogus FLAGS|- VERSION [wh=w|h|wh|-] [missing=..]` (flags V A Y C), `loading M|-` (set `loadingModule`).
**Modules:** `open_module M PATH DATA|-`, `create_disabled M PATH DATA STATE`, `init_module M|-`, `free_module M|-`, `lists`, `get_module NAME`, `enum_modules`, `log_loaded`, `post_load`,
`mod_info M`, `mod_null_info`, `mod_meta M`, `mod_lists M`, `mod_add M|- KIND ID`; a module name `M` resolves to a handle from `open_module`, else to the enabled/disabled module of that `mod_name`.
**Discovery/loading:** `add_path BIN|- DATA|-`, `add_safe NAME|-`, `add_disabled NAME|-`, `find`, `find_core BIN DATA TYPE`, `load_plugins BIN DATA TYPE`, `load_core`.
**Files/locale:** `find_file M REL|-`, `config_path M FILE`, `config_path_null`, `load_locale L M DEFAULT|- LOCALE|-`, `lk L KEY`, `locale_string M KEY`, `locale_text M KEY`.
**Crash capture:** `try CMD...` runs one command in a forked child (`try-begin`, output, `try-end ok|crash`); a conforming implementation MUST NOT crash, so `try-end crash` matches `try-end ok`.

## ffmuxproto runner (spec S-037)

Scripts: `scripts/ffmuxproto/*.script`. The runner `#include`s the real `ffmpeg-mux.c` (its `main` renamed) and links the system
`libavformat/libavcodec/libavutil` (FFmpeg development headers are required; the vectors were produced with libavformat 63.1.102).
Every `run`/`inspect` forks a child that receives the accumulated "stdin" bytes and the argument vector; the parent prints the result.
Output lines: `exit N` (signed 8-bit exit status) or `signal N`; `stdout|"..."` / `stderr|"..."` for every line the child wrote
(quoted, scratch directory printed as `@`); `probe:`/`stream`/`pkt` lines from re-reading the produced file with libavformat.
`@NAME` in a file argument is `$CONFORMANCE_TMPDIR/ffmuxproto-NAME`; names without `@` are relative to the scratch directory after `chdirtmp`.
Tokens are limited to 32 per line: use `argsadd` to continue an argument list.

- `reset` clears argument list and stdin bytes. `args T...` sets, `argsadd T...` appends program arguments (after `argv[0]`).
- Building stdin: `raw HEX...`, `str TEXT`, `fill N HEXBYTE` append raw bytes. `info TYPE INDEX KEY PTS DTS SIZE` appends the 32-byte packet
  info (TYPE `video|audio|change` or a number; unused padding is zero). `hdr TYPE INDEX HEX|-` = info (pts=dts=0, key 0, size = payload length) + payload.
  `frame TYPE INDEX KEY PTS DTS N FILLHEX` = info + N filler bytes. `change NAME` = CHANGE_FILE packet carrying the (expanded) file name.
  `cut N` drops the last N bytes. `dump` prints the bytes (16 per row), `sizeof` prints size and field offsets of the info struct.
- `run`: run the real `main()` on it. `inspect`: run only the real initialisation, print the parameters of the created streams
  (read from the codec contexts the program built, not from the muxer-modified codec parameters), then the real shutdown.
- `probe FILE [rel]`: re-open the output with libavformat and print streams and packets (`rel`: timestamps relative to the first dts).
- `exists FILE`, `rmfile FILE`, `linkdev NAME TARGET`, `chdirtmp`, `echo TEXT...`.

Scripts `21-defects` record reference crashes (`signal 11`); exclude it from sanitizer runs. Everything else is clean under
`-fsanitize=address,undefined`.

## mediaplayback runner (spec S-036)

Area `mediaplayback`: `shared/media-playback` (media.c, cache.c, decode.c, media-playback.c, closest-format.h) compiled **unchanged**
against a scripted FFmpeg container/decoder (`gen/mediaplayback_stubs.c` defines `av_read_frame`, `avformat_open_input`, `av_seek_frame`,
`avcodec_send_packet`/`receive_frame`, ... so that every packet, frame, decode/read error and seek position is dictated by the script;
real libavutil, libswscale and libavcodec packet helpers are linked) and a **fake clock** (`os_gettime_ns`; `os_sleep_ms` advances it and
prints `sleep MS`). Needs the system FFmpeg dev libraries (`-lavdevice -lavformat -lavcodec -lswscale -lavutil`). `media.c` and `cache.c`
are `#include`d by the runner so file-local helpers can be called; the **synchronous** commands drive the state machine without threads;
the `t_` commands use the real threaded public API (`media_playback_*`). All output of library/thread code is queued and printed after
each command, so vectors are independent of thread interleaving. Log lines: compare the level; for FFmpeg-supplied error texts
(`Invalid argument`, `Input/output error`, ...) only the text before the FFmpeg part. Tolerance: `mat0 min0 max0` 1e-5 (rest exact).

**Media definition** (between `media` and `endmedia`; pts values in the stream time base, `none` = AV_NOPTS_VALUE):
`media NAME DURATION_US|none START_US|none`; `video TBN TBD W H PIXFMT [frames=N fps=N/D cs=709|170m|470bg|240m|fcc|2020|rgb trc=709|g22|g28|170m|240m|srgb|pq|hlg pri=2020 range=1 codec=h264|vp8|vp9|png|none alpha=1 cll=N mdcv=N latency=N flip=1 openfail=1 dectb=N/D]`
(PIXFMT = FFmpeg name); `audio TBN TBD RATE CHANNELS SAMPLEFMT [codec=.. latency=N openfail=1]`; `pkt STREAM PTS|nopts [size=N key=0|1 fdur=N bet=N|nopts samples=N decerr=1]`
(one packet = one frame; `fdur` frame duration, `bet` best-effort timestamp override, `samples` audio samples, `decerr` decoder rejects it);
`rderr eio|exit` (demuxer error at that position); `fail open|info|seek`. The media is found by path = NAME.
Misc: `clock NS`, `libvpx 0|1`, `pix on|off` (print first byte/line size of frames; off for converted frames), `trace_sleep 0|1`, `echo TEXT`.

**Pure helpers:** `closest PIXFMT`, `cv_pixfmt`, `cv_sample`, `cv_speakers N`, `cv_space CS TRC PRI`, `cv_range jpeg|mpeg`.

**Synchronous streaming player (`m...`)**: `minit PATH [local=1 speed= buffering= format= options=k=v_k2=v2 (underscore = space) range=full|partial linear=1 request_preload=1 cbs=v,a,pre,seek,stop init2=0]`
(no thread; `minit2` runs the open later), `mreset`, `mprepare`, `mnextpkt`, `mdecode v|a`, `mnextv [preload]`, `mnexta`, `meof`, `mcalc`, `mfull 0|1`,
`mplay LOOP [RECONNECTING]`, `mstop`, `mpause 0|1`, `mseek MS`, `mpreload` (the public control calls), `mset FIELD VALUE` (internal fields),
`mtick [N]` / `mtickw ACTIVE PAUSE` (one iteration of the thread loop without the semaphore wait; `mtickw` supplies the stale snapshot a
sleeping thread would hold), `mstate`, `mtime`, `mframes`, `mduration`, `mint` (call the interrupt callback), `mfree`.
Trace lines: `ff open|read|seek|decoder ...` (what the fake FFmpeg saw), `cb video|audio|preload|seek|stop ...`, `tick -> idle|played|eof|reset|seek|reset_ts|paused|timeout|thread-exit|kill`.

**Synchronous cache (`c...`)**: `cinit PATH [..same options..]` (replays the constructor without the thread, then decodes), `cplay/cstop/cpause/cseek/cpreload`,
`creset`, `cseekraw N` (internal `seek_to`), `ctick/ctickw`, `cset`, `cstate`, `cdump`, `ctime`, `cframes`, `cduration`, `cfree`.
`cset thread_valid 1` (also `mset`) fakes a live thread for `preload_frame`.

**Threaded public API (`t_`)**: `t_create PATH [local=1 full=1 ...]`, `t_wait N` (until N callback events), `t_wait_stop N` (until the N-th stop callback),
`t_stop_after N` (stop from inside the video callback after N frames), `t_play LOOP [REC]`, `t_stop`, `t_pause`, `t_seek MS`, `t_preload`, `t_loop`, `t_linear`,
`t_query`, `t_null` (NULL handles), `t_destroy`.

## scripting runner (spec S-039)

Scripts: `scripts/scripting/*.script`. The runner links the real `obs-scripting.c`, `obs-scripting-logging.c`, `cstrcache.cpp` and the header-inline
callback records, built with `-DLUAJIT_FOUND` (no Python) against a **fake Lua backend** (`gen/scripting_stubs.c`) that prints a `backend ...` line for every call.
Handles: scripts `S0..S7`, callbacks `C0..C15`, owner lists `L0..L3`; `-` = NULL script. Lines starting with `info_` are informational and not compared.
- Fake backend: `fake_define PATH [load=ok|fail] [desc=TEXT_NO_SPACES] [props=none|N]` (before `create`).
- Subsystem: `load`, `unload`, `formats`, `python` (the Python fallbacks), `set_module PTR|-`, `cur_module`.
- Scripts: `create S PATH|@null [K V]` (initial string setting), `info S`, `settings S KEY`, `props S`, `save S`, `update S [K V]`, `reload S`, `destroy S`, `forcetype S lua|python|unknown`, `free_backend S` (release a script that `destroy` cannot).
- Logging: `logcb on|off`, `slog S LEVEL TEXT`, `slog_fmt S LEVEL`, `slog_long S LEVEL N` (handler prints `len=` for messages over 100 bytes).
- Deferred queue: `defer_post LABEL...`, `defer_wait` (barrier), `defer_dump`, `defer_try_wait [info]` (500 ms bounded wait).
- Callback records: `cb_add L C EXTRA [S]`, `cb_hook C`, `cb_remove C`, `cb_free C`, `cb_removed C`, `cb_extra_set/get C KEY [V]`, `cb_tail_set C OFF HEX`, `cb_tail_get C OFF N`, `lists`, `cb_links`.
- String cache: `cstr I TEXT|@null`, `cstr_many N`, `cstr_check`.
`unload` also prints `info_detached_head_after_unload dangling|null` (spec D-2) and resets the head. Script `90-defects` records reference defects only as `info_` lines; it leaks by design (exclude from sanitizer runs).

## textft2 runner (spec S-038)

Scripts: `scripts/textft2/*.script`. The runner `#include`s the plugin's own C files (text-freetype2.c, text-functionality.c, obs-convenience.c)
and drives the `obs_source_info` callbacks. FreeType is **faked** (header shim in `gen/textft2_inc`, no library or font file needed), the font
lookup and the graphics subsystem are faked with scripted data (`gen/textft2_stubs.c`); obs-data, obs-properties, obs-missing-files and the
platform layer are real. Not linked: FreeType, fontconfig, GPU. Output lines: `> command` echo, `ev ...` fake-layer events (`events on|off`),
`log <level> <text>` (scratch dir shown as `$TMP`), results. Floats `%.9g`.

Setup: `reset`; `texdim W H` (atlas size, before the first font load); `ascii W H LEFT TOP ADV` (glyphs 0x20..0x7e, index = code point, space empty;
ADV in 26.6); `glyph CP INDEX W H LEFT TOP ADV`; `notdef W H LEFT TOP ADV`; `pitch_pad N`; `font FACE STYLE|- FLAGS PATH` (lookup table);
`fail_init|fail_face|fail_effect on|off`; `linear on|off`; `clock MS` (fake clock); `file NAME "text"`, `filehex NAME HEX`, `touch NAME EPOCH`, `rm NAME`
(files live in the scratch dir; settings use `@TMP@/NAME`).
Host: `load`, `unload`, `info VER`, `defaults VER`, `props VER`, `props_modify VER 0|1`, `create NAME VER`, `set NAME KEY VALUE` (true/false, integer or string),
`sets NAME KEY STRING`, `setfont NAME FACE STYLE|- SIZE FLAGS`, `update NAME`, `tick NAME`, `render NAME`, `destroy NAME`, `try CMD...` (forked; prints `crash signal=...`).
Inspection: `state`, `size`, `glyphs NAME [INDEX...]`, `verts NAME [MAXQUADS]`, `texbuf NAME X Y W H`, `texsum`, `cache NAME UTF8`, `width NAME UTF8`, `missing NAME [NEWPATH]`, `echo`.
`22-defect-tick-no-face` records a reference crash (excluded from sanitizer runs); all other scripts are clean under ASan/UBSan (leaks ignored).
Scripts are Linux-only in one respect: UTF-16 files are not exercised (reference needs a 16-bit wchar_t).

## encoutput runner (spec S-041)

Scripts: `scripts/encoutput/*.script`. The runner links the real `obs-encoder.c`, `obs-output.c` (via `gen/encoutput_out.c`, which `#include`s it) and
`obs-output-delay.c` with a **fake core** (`gen/encoutput_stubs.c`: context layer, fake `video_t`/`audio_t` whose raw-frame connections are driven by the script, a
settable clock, `rand_float` = 0) and **scripted fake encoder / output plugins** (`gen/encoutput_runner.c`). Threads: the end-of-capture thread is held back
until the command that created it finishes (or the main thread blocks on it); the reconnect timer never really elapses - the `fire` command expires it.
After every command the runner waits until all reference threads are idle (`settle`), so output order is reproducible. A SIGSEGV/SIGFPE/SIGABRT prints `result crash`,
a 30 s hang prints `result timeout`. Lines: `log <level> <text>` (core log), `signal <out> <name> [...]` (signal handlers on all 10 output signals),
`plugin enc <e> create|destroy|create fail`, `plugin out <o> start|stop ts=N|begin_data_capture -> B|pkt <V|A><track> pts= dts= usec= kf= size= tb=N/D b0=HEX|packet NULL|destroy`,
then command results. `usec` is `dts_usec` as seen by the output plugin.

Setup: `video FPSNUM FPSDEN W H`, `audio RATE` (fake media, stereo float planar), `clock NS` (fake `os_gettime_ns`).
Encoders: `venc NAME [kf=N] [dts_delay=N] [hold=N] [fail_at=N] [size=N] [sei=HEX] [create_fail=1] [caps=N] [nomedia=1] [divisor=N]`,
`aenc NAME [frame_size=N] [dts_delay=N] [mixer=N] [fail_at=N] [create_fail=1] [caps=N] [nomedia=1]` (the encoder id equals NAME; `kf=0` means only the first packet is a keyframe;
`hold=N` the first N encode calls return no packet; `dts_delay=N` gives `dts = pts - N*step`; step = one frame / one audio frame size), `enc_raw video|audio ID NAME` (arbitrary id),
`enc_info E`, `enc_init E`, `enc_set_video|enc_set_audio E`, `enc_divisor E N`, `enc_scaled E W H`, `enc_start_ts E NS` (test hook: poke the encoder's start timestamp), `encstate E`.
Outputs: `out NAME FLAGS [start_ok=0|1] [autobegin=0|1] [autoend=0|1]` (FLAGS = comma list of `video audio av encoded service multi_a multi_v pause`; the output id equals NAME;
`autobegin=1`: the fake plugin start callback does what a real plugin does - `can_begin_data_capture`, `initialize_encoders`, `begin_data_capture`; `autoend=1`: the plugin stop
callback calls `end_data_capture`), `out_unknown ID`, `plugin OUT key=value...` (reconfigure start_ok, autobegin, autoend, `fail_code=N` = on a failing start call `signal_stop(N)`),
`set_venc|set_aenc OUT ENC|- [IDX]`, `set_delay OUT SEC [FLAGS]`, `reconnect_cfg OUT COUNT SEC`, `reconnect_cb OUT allow|veto|off`, `poke_exp OUT X` (test hook: the reference randomises the
exponent in [1.5, 1.55)), `set_service OUT`.
Actions: `start|stop|force_stop|begin|end|can_begin|init_encoders|destroy OUT`, `signal_stop OUT CODE`, `fire` (let the pending reconnect timer expire), `lasterr OUT TEXT|-|?`,
`vframe NS` / `aframe MIX NS FRAMES` (deliver a raw frame to every connected callback), `emit ENC PTS DTS KF [SIZE [TBNUM TBDEN]]` (inject one packet through the real packet path of that encoder;
for video a frame-timing record is queued first, as `do_encode` does), `emit_fail ENC`.
Inspection: `state OUT`, `queue OUT` (interleaver queue as `<V|A><track>@dts_usec`, and the number of delay-queue entries), `info OUT`, `encstate E`, `conns`, `echo TEXT...`.
Sanitizer note: the fake output plugin's `start` precondition (encoders must be initialised before `begin_data_capture`) is kept; a real plugin that skips it hits a divide by zero (spec S-041 D-2).

## obssource runner (spec S-040)

State: a fake core (no graphics, scripted clocks), a table of plug-in *kinds* and a table of *sources* addressed by a script token (the
token is not the source's name). Sources hold the script's strong reference until `release`; after the last release the
object stays addressable until `drain` (deferred destruction). Everything printed is plain text; floats are `%.9g` (tolerance 1e-5),
`log` lines are compared by level only (section "Comparison rules"). Lines starting with two spaces are trace output from fake plug-in
callbacks (`  cb <source name> <event>`), signal handlers (`  sig <signal> source=<name> ...` for source signals, `  gsig` for global ones)
and fake host services.

World: `clock NS` (os_gettime_ns), `vtime NS` (render clock `video_time`), `half NS` (half frame interval), `drain` (run deferred
destruction, prints `drain ran=N`), `pending`, `echo T`.
Kinds: `kind ID TYPE FLAGS [HOOKS]` with TYPE `input|filter|transition`, FLAGS comma list of `video audio async asyncvideo customdraw
composite nodup controllable` (or `-`), HOOKS comma list of `create destroy update activate show tick enum size defaults filteradd fvideo
tstart tstop nocreate media` (`enum` = reports the children set with `child`, `size` = reports `fksize`, `fvideo` = async video filter
that returns the frame or NULL per `fkdrop`, `nocreate` = create returns NULL).
Sources: `create TOK KIND NAME [private]`, `addref/release/refs/getref TOK NEW`, `weak TOK W`, `weak_get W TOK`, `weak_expired W`,
`weak_release W`, `remove/removed/rename/info/enable/hidden/set_flags/set_default_flags/set_mixers`, `setting TOK K INT`, `get_setting TOK K`,
`update TOK [K INT]`, `reset_settings`, `update_props`, `deferred TOK`, `fksize TOK W H`, `fkdrop TOK 0|1`, `child TOK CHILD`, `child_clear`,
`size TOK`, `watch TOK` (trace all source signals), `watch_global`, `duplicate TOK NEWTOK NAME [private]`, `media TOK play|pause|restart|stop|next|prev|settime N|started|ended|query`.
Activation: `activate|deactivate TOK main|aux`, `inc_showing/inc_active/dec_showing/dec_active TOK`, `state TOK`, `tick TOK SECONDS`,
`add_active_child P C`, `remove_active_child P C`, `enum_active|enum_tree|enum_full TOK` (prints `(parent>child)` pairs).
Filters: `filter_add/filter_remove S F`, `filter_order S F up|down|top|bottom`, `filter_index S F`, `filter_set_index S F N`, `filters S`
(array order with target and parent), `names S`, `filter_count S`, `filter_by_name S NAME TOK`, `filter_target F`, `enum_filters S`,
`copy_filters DST SRC`, `copy_filter DST F`, `backup S`, `restore S`.
Async video: `video|video2|preload|set_video_frame TOK FMT W H TS FULL FILL FLIP` (FMT `i420 nv12 yuy2 uyvy rgba bgra bgrx y800 i444`;
for `video2` FULL is the range 0 default, 1 partial, 2 full; the pixel data is FILL repeated), `video_null TOK`, `show_preloaded TOK`,
`astate TOK` (`queue=[timestamps] cache=N used=M [used/unused_count,...] cur= prev=` ...), `get_frame TOK`, `release_frame TOK`,
`unbuffered TOK 0|1`, `rotation TOK DEG`, `deint_mode TOK N`, `deint_field TOK N`, `deint_get TOK`.
Transitions: `tr_start T MODE MS DEST|-`, `tr_set T SRC|-`, `tr_clear T`, `tr_state T`, `tr_time T`, `tr_manual T V`, `tr_torque T TORQUE CLAMP`,
`tr_fixed T 0|1 MS`, `tr_fixed_get`, `tr_scale T N`, `tr_align T N`, `tr_size T W H`, `tr_get T`, `tr_source T 0|1`, `tr_active_source T`,
`tr_matrix T 0|1`, `tr_video T` (obs_transition_video_render with a tracing callback `  vcb t=.. size=..`), `tr_video_direct T 0|1`,
`audio_ts TOK TS PENDING DUP`, `audio_fill TOK VALUE`, `tr_audio T MIXERS CHANNELS RATE`, `tr_swap A B`, `tr_force_stop`, `tr_save T`,
`tr_load T SRCNAME ALIGN MODE SCALE CX CY`, `tr_enum T`.
Source tokens may be `-` where a NULL source is meaningful. A command that needs a missing source token aborts the runner with exit status 2.
The vectors depend on these fake-host behaviours: name uniqueness is **not** enforced; `obs_get_source_by_name` searches the public sources;
`obs_save_source` stores `id` and `name`; `obs_load_source` creates a private source of that id/name.

## obscore runner (spec S-042)

`gen/obscore_runner.c` #includes the real `obs.c`, `obs-canvas.c`, `obs-view.c` and `obs-display.c` and drives the core context
(startup/shutdown, video and audio reset, canvases, views, displays, registries, lookups, task queues). Real code is also linked for
`video-io.c`, `audio-io.c`, `obs-data.c`, signal/proc handlers, the task queue, the profiler name store and the platform layer.
Fakes (`gen/obscore_stubs.c`): a **recording graphics device**, a graphics thread that only runs queued tasks, the audio-thread
callback (task queue part only), the hotkey subsystem, the source/output/encoder/service object layer (reference counting is the
real libobs one) and the module registration entry point. Fake calls print `ev <text>` lines when their category is traced.
Log lines are printed as `log <level> <text>` with tabs/newlines turned into spaces, generated UUIDs masked as `<uuid>` and a NULL
string rendered as `<null>`. One command per line, tokens as in the common script format (`-` = NULL where stated).

**Handles.** Objects live in a name table: `mk_source|mk_output|mk_encoder|mk_service H NAME|- [private=1] [id=ID] [type=input|scene|filter|transition] [uuid=U] [canvas=C] [nodata=1]`,
`canvas_create[_private] H NAME|- [flags=a|b|..] [video=1 + video keys]`, `view_create V`, `display_create D CX CY BG|nogfx`. The
handle `main` is the main canvas, `-`/`null` is NULL. Flag names: `main activate mix_audio scene_ref ephemeral program preview device removed`
(or a number). `rel H` = `obs_object_release`; `ref H` = `obs_object_get_ref`; `refs H` prints the raw counters
(`strong=0` means ONE owner: the counter is "owners minus one").

**Video keys** (`reset_video`, `canvas_create video=1`, `canvas_reset_video`, `view_add`): `base=WxH out=WxH fps=N/D fmt=<name|number> cs=default|601|709|srgb|pq|hlg
range=default|partial|full scale=disable|point|bicubic|bilinear|lanczos|area gpu=0|1 adapter=N module=NAME|-` (defaults 1920x1080, 1280x720, 30/1,
NV12, 709, partial, bicubic, 1, 0, `fake-gfx`). Format names are those of `get_video_format_name` (`NV12`, `BGRA`, ...). `reset_video` prints the result
code name (`success fail not_supported invalid_param currently_active module_not_found`) and the (possibly aligned) struct as `after_call`.

**Lifecycle/state:** `startup LOCALE|- CONFIG|- [store]`, `shutdown`, `initialized`, `state`, `version` (major/minor/patch), `locale_set|locale_get`,
`cmdline_set ARGS...|cmdline_get|cmdline_argc`, `name_store`, `free_ext_store`, `data_path_add|data_path_remove P` (`TMP` = temp dir with
trailing slash, `TMPNOSLASH` without), `touch NAME`, `find_data NAME`, `report` (graphics context balance), `live_mark`/`live_delta` (change of the
number of live fake graphics objects), `echo`.

**Fault injection** (`fake KEY VALUE`): `gs_error success|fail|module_not_found|not_supported|N`, `device d3d11|opengl`, `effect_fail FILE`
(repeatable, `effect_fail_clear 1` resets), `tex_fail_at N` / `surface_fail_at N` (1-based call index, counters restart when set, 0 = off),
`swap_fail 0|1`, `hotkey_platform_fail 0|1`, `present_ready 0|1`, `color_space srgb|srgb16f|709ext`, `nv12 0|1`, `p010 0|1`, `data_found 0|1`.
`trace gs|sub|src on|off` enables the `ev` lines of graphics calls, subsystem calls (hotkeys, profiler reset) or source activation/render.

**Video/audio:** `reset_video [keys]`, `probe_reset_video [keys]` and `probe_reset_audio [keys]` (fork a child: prints `crash signal=SIGFPE|...` or
`returned success|failure`; no worker thread may exist at that moment), `video_info`, `mix_info C` (conversion flags, technique names,
`wi`/`hi` = 1/width and 1/height factors, NV12/P010 use, render space, mix_audio, colour matrix as 16 `%.9g` floats; **`-0` and `0` compare equal**),
`mixes` (list owners/sizes), `mix_active C raw=N gpu=N`, `video_active`, `intervals`, `set_levels SDR HDR`, `levels`, `base_effect`,
`reset_audio sps=N speakers=unknown|mono|stereo|2.1|4.0|4.1|5.1|7.1|N [max_ms=N] [fixed=0|1]`, `reset_audio_v1`, `reset_audio_null`, `audio_info`
(prints v1, v2 and the NULL-argument forms), `audio_state`, `audio_connect|audio_disconnect` (a never-called consumer: makes the output active).

**Registries/enumeration:** `reg_source ID type=input|filter|transition|scene [version=N]`, `reg_output|reg_encoder|reg_service ID`, `enum_types
source|input|filter|transition|output|encoder|service`, `enum_input2 IDX [noid] [nouv]`, `latest_input ID|-`, `protocol_add|protocol_registered|protocols`,
`enum sources|all_sources|scenes|outputs|encoders|services|canvases|canvas_scenes C [stop=N]` (prints `visit NAME`; the callback returns false
at the N-th visit), `get KIND KEY [C]` with KIND `source_by_name source_by_uuid canvas_by_name canvas_by_uuid transition_by_name transition_by_uuid
output_by_name encoder_by_name service_by_name canvas_source_by_name canvas_scene_by_name main_canvas` (KEY `uuid:H` = UUID of handle H; the
returned reference is released after printing), `obj_api H`, `uuid_reset H...`, `src_rename|src_canvas_remove|src_canvas_insert|src_removed`.

**Canvases/views/displays:** `canvas_info|canvas_release|canvas_ref|canvas_weak W C|weak_get W|weak_release W|canvas_name C NAME|canvas_remove C|
canvas_reset_video C [keys|null]|canvas_video_info|canvas_valid_ovi|canvas_save C|canvas_load H name uuid|- private flags|canvas_set_channel C CH SRC|-|
canvas_get_channel|set_output_source CH SRC|get_output_source CH|canvas_render`, `sig_obs NAME...` and `sig_canvas C NAME...` (print `signal NAME` plus the
decoded parameters; canvas-level lines are prefixed `C:`), `view_set|view_get|view_render|view_channels|view_add V [keys]|view_add2_null|view_remove|view_enum V [stop=N]|
view_destroy`, `display_resize|display_render|display_enable|display_bg|display_color_space|display_cb_add D LABEL|display_cb_remove|display_info|display_list|
display_destroy|display_null`.

**Callbacks/tasks:** `tick_add|tick_remove a|b`, `render_cb_add|render_cb_remove LABEL`, `rendered_cb add|remove`, `callbacks`, `ui_handler on|off`,
`queue_task graphics|audio|ui|destroy WAIT LABEL` (tasks print `task LABEL thread=graphics|audio|ui|other`), `wait_destroy`, `in_task_thread`,
`private_data set|apply|get KEY [VALUE]`.

Scripts that use `probe_*` crash children; under AddressSanitizer those children report through the sanitizer instead of a signal, so
`021_reset_video_fps_format` and `031_reset_audio_edge` differ in that mode (expected). The reference leaks in some failure paths
(see S-042 Q-9), so run sanitizer builds with `detect_leaks=0`.

## avio runner (spec S-044)

`gen/avio_runner.c` `#include`s the real `libobs/media-io/video-io.c` and `audio-io.c`, so the **real video and audio threads run**. Everything
non-deterministic is replaced by fakes in `gen/avio_stubs.c`: `os_gettime_ns` returns a settable value, `os_sleepto_ns_fast` blocks the audio
thread until the driver releases one tick, `profile_end` (called once per loop iteration) is the "iteration finished" signal, and the scaler and
resampler are logging fakes. A script command therefore never returns before the thread has finished the work it triggered, and the output is
independent of scheduling. Every command is echoed as `> command args`; results follow as `= value` lines. Library log lines appear as
`log LEVEL text`. Names (`I420`, `FLOAT_PLANAR`, `STEREO`, `PARTIAL`, `709`...) are the enum names without prefix; `-` is 0/UNKNOWN/NONE/NULL.

**Video** (outputs are named; up to 8): `vopen V FMT NUM DEN W H CACHE [RANGE CS]` (prints the result code), `vclose V`, `vinfo V`, `vnull` (NULL-safe
getters), `vconnect V a|b PARAM [DIV|-] [FMT W H RANGE CS]` / `vconnect2` (callback `a` or `b` plus an integer param form the input identity; the
conversion is given only when all five fields are present), `vconnect_nocb V`, `vdisconnect|vdisconnect2 V cb PARAM`, `vdisconnect_nocb V`,
`vlock V COUNT TS [FILL]` (prints success and the cache slot; FILL is written to the first byte of plane 0), `vunlock V` (synchronous: waits for
the thread iteration), `vunlock_async V`, `vwait N` (wait for N iterations), `vgate on|off` / `venter` / `vrelease N` (an input callback blocks
inside the thread until released: simulates encoder lag), `vhook cb PARAM none|disconnect_self|disconnect cb2 PARAM2` (callback side effect),
`vstop V`, `vstate V` (active, stopped, skipped, total), `vdump V` (ring indices, available count, per-slot count/skipped/timestamp), `vinputs V`
(per-input divisor, counter, scaler presence, stored conversion, rotating buffer index), `vtex V inc_enc|dec_enc|frames|skipped`,
`vdivider NEW V DIV`, `vfree_divider NEW`, `vfree_divider_root V`, `scaler create ok|bad|fail` / `scaler scale ok|fail`.
The video callback prints `vcb a|b param=P ts=T ls0=LINESIZE0 d0=<first two bytes of plane 0 as hex> src=cacheN|convI.K` (`convI.K`: converted
buffer K of input I; the fake scaler writes `(plane0[0] xor ff, scale-call-count)` into the first two bytes of every plane).

**Audio** (one output at a time): `aclock NS` (value the thread reads as its start time; set before `aopen`), `aopen NAME|- RATE FMT SPEAKERS`
(prints the result; on success the thread is running and waiting for the first tick), `atick [N]` (releases N loop iterations, each waits until the
iteration finished and prints `tick sleepto=<due time>`), `aclose`, `ainfo`, `aconnect a|b PARAM MIX [RATE FMT SPK CLIP]` (conversion given only if
all four are present), `aconnect_null`, `adisconnect cb PARAM MIX`, `adump`, `afeed MIX PLANE START COUNT VALUE set|add` / `afeed_clear` (what the
fake input callback writes into the mix buffers; VALUE may be `nan`, `inf`), `anewts none|delta N|abs N` (value stored to `*new_ts`), `ainret
true|false`, `aprobe I...` (sample indices printed by output callbacks), `ahook cb PARAM none|disconnect_self|disconnect cb2 PARAM2`, `resamp
create ok|fail`, `resamp resample ok|fail`, `resamp offset NS`, `resamp frames N` (fake resampler: output samples = input * 0.5, frame count =
N or in_frames * dst_rate / src_rate).
The input callback prints `in start=S end=E active=0xMASK nonnull=[per-mix count of non-NULL plane pointers] clean=yes|no` (`clean`: all buffers
were zero on entry); output callbacks print `out a|b param=P mix=M ts=T frames=N buf=clamped|unclamped|resampled p0=[..] p1=[..]`.

`crash NAME` forks a child that calls a NULL-argument function and prints `crash NAME signal=N` or `nocrash NAME exit=N` (see S-044 D-1..D-3).
The reference leaks nothing in these scripts; run sanitizer builds normally (the `crash` children report through the sanitizer instead of a signal).

## rtmpstream runner (spec S-045)

`gen/rtmpstream_runner.c` `#include`s the real `plugins/obs-outputs/rtmp-stream.c` and `net-if.c` and drives their static functions.
Fakes (all in the runner): libobs output/encoder/service/settings objects, librtmp (`RTMP_*`), the FLV muxer (each `flv_*` call prints its
arguments and returns a buffer of `fake write_len` bytes, default 10), `getifaddrs`, `ioctl(FIONREAD)`, the clock (`fake now`), and **threads**:
`pthread_create/join/detach` are fakes (print `fake pthread_*`), the send-thread body runs synchronously via `run_send`, the connect thread
via `connect_thread`. Not linked: real librtmp transport (S-031), real FLV muxer (S-021), `rtmp-windows.c`, real sockets/threads.
`reset` (implicit at start) creates a stream, applies the registered defaults to the output settings. `alarm(20)` kills a script that would
block the synchronous send loop. Output lines: `> command` echo, `fake ...` (outward calls), `log LEVEL text`, results.
JSON-parser error text in the "interpolation table failed" warning is masked as `<json-error>`.

Commands: `reset`, `opt int|bool|str KEY VAL` (user setting), `no_defaults`, `defaults`, `service URL KEY USER PASS` (`-` = NULL), `no_service`,
`venc IDX CODEC KBPS [CAPS] [NAME]`, `aenc IDX CODEC KBPS`, `enc_flag v|a IDX no_video|no_extra 0|1`, `colorspace CS FORMAT`, `peak N`, `delay N`,
`fake KNOB N` (`setup_ok connect_ok connectstream_ok connect_err write_fail_at write_len pthread_create_fail pending readpacket_ok can_begin
init_enc_ok now advance`), `set FIELD VALUE` (stream fields, see source), `codec IDX NAME`, `state`, `dbr`, `table`, `queue`, `init_connect`,
`dbr_json TEXT`, `dbr_frame BEG_NS END_NS SIZE`, `dbr_lower`, `dbr_inc`, `dbr_set`, `check_drop b|p`, `pkt video|audio DTS_US PRIO KF [TRACK]`,
`pkt_null`, `err CODE`, `try_connect`, `start`, `connect_thread`, `stop TS_NS`, `post N`, `run_send`, `send_headers`, `send_footers`, `getters`, `info`,
`props`, `destroy`, `drop_frames PRIO`, `find_first`, `can_shutdown SYS_DTS_US`, `ifa NAME v4|v6|other|null ADDR`, `ifa_clear`, `ifa_fail`, `netif_get`,
`str_to_addr TEXT|-`, `addr_to_str v4|v6|other HEXBYTES LEN`, and `probe CMD...` (runs one command in a forked child; prints `probe crash signal=...`
or `probe exit=0`).

## graphics runner (spec S-043)

`gen/graphics_runner.c` drives the real `libobs/graphics/graphics.c` and `texture-render.c` against a tracing fake backend
(`gen/graphics_backend.c` hand-written state, `gen/graphics_backend_gen.c` generated by `scripts/graphics/gen_backend.py`
from `struct gs_exports`; `gen/graphics_syms.inc` lists the GS_* constants). Every backend call prints a line `  dev NAME arg=val ...`
(object handles print as `#N`, N counting from 1 in creation order; device `#1`; in a fresh graphics object the immediate buffer is `#2`,
sprite `#3`, subregion `#4`, flipped `#5`). Frontend log messages print as `log LEVEL text`. Each command is echoed `> cmd args`.
Numeric arguments accept numbers or `GS_*` names, joined with `|`. `-` = NULL. Results print `ret ...`.

Commands: every `gs_*` function by its C name with positional arguments (see runner for argument orders; matrices are 16 floats,
`gs_render_stop MODE`, `gs_texture_create W H FMT LEVELS FLAGS [data]`, `gs_vertexbuffer_create N FLAGS NORM COL NTEX`, ...). Append `> TOK`
to store a returned object in token `TOK`; tokens are passed by name. `gs_create G [MODULE=fake] [ADAPTER]` (modules not starting with `fake`
fail to open), `gs_destroy G`, `gs_enter_context G`, `gs_get_context`.
Backend scripting: `ret FN VALUE` (scripted result of backend function FN, `-` = NULL string), `fail FN 0|1` (pointer-returning FN returns NULL),
`failnth FN N`, `missing FN 0|1` (export absent at the next gs_create), `size W H`, `adapter NAME`, `file PATH CONTENT` (virtual file system),
`dump full|flags`, `trace 0|1`, `reset_backend`. Images: file name `WxH` decodes to an RGBA pattern, anything else fails.
Harness: `echo TEXT`, `same A B`, `crash CMD...` (forked child, 2 s alarm; prints `child exit=N` or `child signal=N`, 14 = hang),
`thread CMD ... ; CMD ...` (run on a new thread, joined), `inline_format_info F`, `inline_format_from_space S`, `inline_total_levels W H D`.
Floats print `%.9g`; tolerance 1e-6.

## ffsources runner (spec S-047)

The obs-ffmpeg media source (`obs-ffmpeg-source.c`) and the runtime halves of the obs-ffmpeg audio and
video encoders (`obs-ffmpeg-audio-encoders.c`, `obs-ffmpeg-video-encoders.c`, `obs-ffmpeg-formats.h`).
The create-time codec options of these encoders are covered by the encopts area (S-025) and not repeated.
The reference runner links the REAL plugin code (`gen/ffsources_src.c` and `gen/ffsources_aud.c` `#include`
the plugin files so that static functions can be called; `obs-ffmpeg-video-encoders.c` is linked as an
object) with the real `obs-data.c`, `obs-properties.c`, `calldata.c`, the real libavutil (`-lavutil`)
and recording fakes (`gen/ffsources_stubs.c`) for libavcodec, the OBS host and media-playback.
`ffsources.sources` uses `-I` paths relative to `test/conformance` (start `run.sh` there).
Not linked: libavcodec (all codec calls are fakes), libavformat, the real media-playback library (S-036),
the real libobs source/hotkey/proc/missing-files modules (fakes record calls), real threads: in
`ffsources_src.c` the macros `pthread_create/join`, `os_event_*`, `pthread_mutex_init` and `os_file_exists`
are redefined to deterministic single-threaded substitutes. The reconnect "thread" runs when it is joined
(its stop event is then signalled, so it exits at once) or on `thread_run` (its wait times out).
`obs_module_text(KEY)` returns KEY except `Encoder.Error` ("Failed to open %1: %2") and `Encoder.Timeout`
("Encoder %1 is taking too long to encode (timeout: %2 seconds)"). `av_err2str` text of codes <= -1000 is libavutil's
"Error number N occurred" (use only such codes in scripts). Log lines are `log LEVEL TEXT`; compare the level only.

Objects are named by the script (`D` data, `P` properties, `S` source, `A` audio encoder, `V` video encoder);
unknown names/commands are script errors (exit 2). Line order is significant: fake-library lines, host lines,
log lines and result lines appear in the order the reference produces them. `@null` is NULL where noted.

| Command | Effect / output |
|---|---|
| `echo TEXT` | prints `echo TEXT` |
| `data D`, `set_bool D k 0/1`, `set_int D k n`, `set_str D k s`, `dump_data D` | obs_data object; dump lists items (`name type value user/default`) |
| `dump_props P` | `props P flags=N`, one line per property (type, description key, visible, enabled, long description, int min/max/step/numtype/suffix, text type, path type/filter/default_path, list items), `end props P` |
| `host_int KEY N` | host state: `active showing media_fail media_has_video event_init_fail mutex_init_fail thread_create_fail open_rc fill_rc frame_size tb_num tb_den extradata initial_padding duration_us frames time now_ns pause_offset` |
| `host_settings D` | settings returned by `obs_source_get_settings` |
| `host_video W H FMT CS RANGE FPSN FPSD` | encoder size, format (`none I420 NV12 I444 I010 P010 BGRA I422 YUY2 RGBA I40A YUVA Y800`), colour space (`default 601 709 srgb pq hlg`), range (`full`/other) and frame rate |
| `host_audio CH SPEAKERS RATE` | channel count, speaker layout number (`SPEAKERS_*`), sample rate |
| `host_last_error TEXT/@null`, `show_last_error` | set / print the encoder's last error |
| `exists PATH 0/1` | scripted `os_file_exists` |
| `src_table` | prints the source registration record |
| `src_defaults D` | calls get_defaults on D |
| `src_create S D` | create (+ first update); prints `src_create ok/null` |
| `src_update S D`, `src_destroy S`, `src_tick S`, `src_activate S`, `src_deactivate S` | the callbacks |
| `src_play_pause S 0/1`, `src_stop S`, `src_restart S` | media callbacks |
| `src_duration S`, `src_time S`, `src_state S`, `src_set_time S MS` | prints `duration_ms N`, `time_ms N`, `state N` |
| `src_missing S`, `missing_cb S IDX PATH` | missing-files report (`host.missing_file`) / invoke a report's replacement callback |
| `src_props P S/@null`, `props_modified P PROP D` | get_properties (S may be `@null`) / run a property's modified callback (`modified PROP -> 0/1`) |
| `src_dump S` | internal state: input, input_format, ffmpeg_options, numbers, flags, media present, destroy_media, reconnecting, reconnect_thread_valid |
| `hotkey S restart/play/pause/stop PRESSED` | invoke the registered hotkey callback (`hotkey NAME pressed=N [-> result]`) |
| `proc S restart/preload_first_frame/get_duration/get_nb_frames` | invoke the registered proc; prints results |
| `media_cb video/preload/seek/audio/stop` | invoke a callback the source gave to the (fake) player in the last `mp.create` |
| `thread_run` | run the pending reconnect thread with an unsignalled event (the wait times out) |
| `av_codec NAME 0/1`, `av_lossless NAME 0/1`, `av_formats LIST/none`, `av_rates LIST/none` | fake libavcodec catalogue (`u8 s16 s32 flt dbl u8p s16p s32p fltp`) |
| `av_send RC...`, `av_recv RC SIZE PTS DTS FLAGS`, `av_clear` | queue results of the next `avcodec_send_frame` / `avcodec_receive_packet` calls (empty receive queue = EAGAIN, empty send queue = 0); `av_clear` empties both |
| `audio_table`, `audio_defaults KIND D`, `audio_props KIND P` | encoder registrations (kinds `aac opus pcm pcm24 pcm32 alac flac`) |
| `audio_create A KIND D`, `audio_dump A`, `audio_encode A FILL`, `audio_info A KIND`, `audio_destroy A` | create (`audio_create KIND ok/null`), state, encode one frame (planes filled with `FILL + plane*16 + (i&15)`), info callbacks, destroy |
| `vid_init V LIB LIB2/@null NAME CBERR CBFIRST`, `vid_init_stack zero/ones LIB LIB2/@null NAME` | init (CB flags 0/1 pass a callback); `vid_init_stack` runs a *failing* init in a child with the stack pre-filled with 0x00 / 0xFF (reference defect D-1) and prints `child: ...` lines or `crash (child killed by a signal)` |
| `vid_update V BITRATE KEYINT OPTS/@null`, `vid_init_codec V`, `vid_dump V`, `vid_free V` | update (S-025 R60), init_codec, state, free |
| `vid_encode V PTS LS0 LS1 LS2 LS3` | encode a frame whose plane p has source linesize LSp (0 = NULL plane; bytes `p*0x40 + y*0x10 + x`); destination frame pre-filled with 0xEE; fake `av.send_frame video` lists per plane the first 4 bytes of each row and an FNV-1a hash of the whole row |

Fake libavcodec lines: `av.find_encoder N -> found/missing`, `av.open2 ...`, `av.fill_audio_frame ...`, `av.send_frame ... -> rc`,
`av.receive_packet -> rc ...`, `av.packet_unref`, `av.free_context`, `av.opt_set N=V flags=F`. Host lines: `host.*`, `mp.*`,
`event.*`, `thread.*`. Linear sizes of video frame buffers (`linesize=32` for 32-byte alignment) come from the real
libavutil `av_frame_get_buffer` with `base_get_alignment()` = 32.

## ffoutputs runner (spec S-046)

`gen/ffoutputs_runner.c` #includes the real `plugins/obs-ffmpeg/obs-ffmpeg-mux.c` (`ffmpeg_muxer`, `ffmpeg_mpegts_muxer`,
`replay_buffer`) and `obs-ffmpeg-hls-mux.c`; `gen/ffoutputs_out.c` #includes `obs-ffmpeg-output.c` (`ffmpeg_output`, commands `fo_*`).
libobs is faked in `gen/ffoutputs_stubs.c`: a typed settings store (user value over default), output/encoder/service objects, signal and
proc handlers, a virtual file system (existence only; `fs add PATH`), a fake name generator (`os_generate_formatted_filename`: the format with
`%N` replaced by a per-process counter, spaces to `_` unless allow_spaces, plus `.EXT`), the helper pipe, a fake clock, and gated worker threads.
libav* is the real system library (only the `null` muxer, which needs no file, is used by `ffmpeg_output`). Every output line is the echoed
command `> ...` or an event print: `wire ...` (a decoded pipe write: `V|A idx= key= pts= dts= size= data=<first 6 bytes hex>`, or
`CHANGE_FILE "name"`), `pipe create` + `argv[i]`, `pipe destroy -> CODE`, `signal NAME`, `signal_stop NAME`, `last_error "..."`, `begin_data_capture`,
`end_data_capture`, `mkdirs`, `unlink`, `log LEVEL text`. Argument tokens `key=value` are order independent; `-` is an empty value where noted.

Mux side: `reset`, `output NAME [service] [av]`, `size W H`, `mixers MASK`, `channels N`, `video_info FPSN FPSD CS RANGE FORMAT` (CS = DEFAULT 601 709 SRGB PQ HLG or
a number; RANGE = F or P; FORMAT = NV12 I420 I444 YUY2 UYVY BGRA P010 I010 I422 I210 NONE), `hdr_peak N`, `venc CODEC [bitrate=N] [codec_type=N] [keyint_sec=N]
[extra=HEX]`, `no_venc`, `venc_extra HEX|-`, `venc_set KEY N`, `venc_paused 0|1`, `aenc NAME CODEC RATE FRAME PRIMING [bitrate=N] [extra=HEX]` (consecutive
tracks), `aenc_extra IDX HEX|-`, `set int|bool|str KEY VALUE` (output settings), `service URL KEY|-`, `capture CAN_BEGIN [INIT_ENC_OK]`, `knob NAME VALUE`
(`pipe_fail write_fail_at destroy_ret err_text now_ns defer_threads`; `write_fail_at N` fails the Nth pipe write of the current pipe, `err_text -` = empty,
`now_ns` is the fake `os_gettime_ns`), `fs add|deny|clear PATH`, `create ffmpeg_muxer|ffmpeg_mpegts_muxer|replay_buffer|hls`, `defaults`, `settings`, `info`,
`getname`, `props`, `start`, `stop TS_NS`, `destroy`, `pkt v|a [pts= dts= dus= sys= key= size= track= tb= prio= dprio= data=HEX null=1]` (defaults: dts=pts,
dus=dts*1e6/tb, sys=dus, tb=1000, size=8; payload bytes are a deterministic pattern unless `data=`; `null=1` delivers a NULL packet = encoder failure; for HLS
the line `parsed drop_priority=` shows the priority computed by the output), `proc split_file|save|get_last_replay`, `run_pending` (let a deferred save thread run),
`pump N` (let the HLS/ffmpeg_output worker handle up to N queued items, then wait until it is idle), `state`, `state_rb`, `state_split`, `state_hls`, `queue`,
`total_bytes`, `dropped`, `set_field last_dts_usec|stream_key|muxer_settings|printable_path|keyint_sec|cur_size VALUE`, `echo TEXT`.
Threads: the replay-buffer save thread runs inline (or deferred with `knob defer_threads 1`); the HLS and ffmpeg_output workers are real threads that only
run when pumped, so output is deterministic. `refs` in `state` is the balance of `obs_encoder_packet_ref/release` calls.

ffmpeg_output side (`fo_*`, settings come from the same `set`/`names`/`video_info`/`mixers`/`size` commands; `names A B -`: settings array `audio_names` with one item per
word, `-` = item without a name): `fo_create`, `fo_info`, `fo_start` (the connect thread is deferred), `fo_connect` (run it; prints the received `cfg`, then
the created streams), `fo_pump N`, `fo_stop TS_NS`, `fo_destroy`, `fo_video TS_NS`, `fo_audio MIX FRAMES TS_NS`, `fo_state`, `fo_packets`, `fo_write_ret CODE|real`
(force the result of `av_interleaved_write_frame`), `fo_set stopping|stop_ts|pause_offset|total_bytes VALUE`, `fo_process`, `fo_sys_dts IDX`, `fo_track_order MASK MIX`,
`fo_mix_count MASK`, `fo_data_init URL FORMAT|- MIME|-` (direct call of `ffmpeg_data_init` with an otherwise empty config; `-` = NULL), `fo_formats`, `fo_sample_formats`,
`fo_rescale VAL NUM DEN NEWNUM NEWDEN`. Video packet sizes and byte counts are libav-encoder dependent and are never printed (`size=(video)`, `total_bytes_without_video`).
Run `./run.sh` from `test/conformance`; the FFmpeg development libraries (libavformat, libavcodec, libavutil, libswscale) must be installed.

## frontendfmt runner (spec S-048)

`gen/frontendfmt_runner.c` is a C transcription of the pure, non-Qt frontend logic (CLI parsing from `obs-main.cpp`; file-name helpers, config defaults and
migrations from `OBSApp.cpp` / `OBSBasic.cpp`) driving the real `libobs/util/config-file.c`. File existence is virtual. Three config objects exist: `app` (global.ini),
`user` (user.ini), `prof` (basic.ini). `@null` is the NULL argument. Commands: `cli PORTABLE_ALLOWED ARGS...` (prints the resulting flags or `exit help|version`),
`fs_add PATH` / `dir_add PATH` (virtual files / directories; `reset` clears them), `safe_name NAME`, `unused PATH EXT|@null`, `fmtstr FORMAT PREFIX|@null SUFFIX|@null`, `ext CONTAINER`,
`best PATH NOSPACE`, `outname DIR CONTAINER NOSPACE OVERWRITE STEM` (STEM stands for the S-023 `%`-expanded name without extension), `set|setdef CFG SECTION KEY string|int|uint|bool|double VALUE`,
`unset CFG SECTION KEY`, `show CFG SECTION KEY` (prints user/default presence and string value), `defaults app|user|basic [CX CY]` (prints every Linux default), `migrate_legacy LASTVERSION`
(LASTVERSION as integer `(major<<24)|(minor<<16)|patch`), `basic_migrate`, `resdef W H PRE19`, `pre31 LASTVERSION USER_INI_EXISTS COPY_OK`, `multiview_pre22 LAYOUT`, `reset`, `echo TEXT`.

## linuxcapture runner (spec S-049)

`gen/linuxcapture_runner.c` drives the real sources `plugins/linux-v4l2/{v4l2-helpers,v4l2-controls,v4l2-input}.c`, `plugins/linux-pulseaudio/pulse-input.c` and
`plugins/linux-capture/xshm-input.c` (each `#include`d by a `gen/linuxcapture_w_*.c` wrapper) against fakes in `gen/linuxcapture_stubs.c`:
a libv4l2 + kernel V4L2 device model, a PulseAudio wrapper/stream model, an xcb/X11 helper layer, and a minimal OBS core. `libobs/obs-data.c` and `obs-properties.c`
are the real ones. Needs the system headers `linux/videodev2.h`, `libv4l2.h`, `pulse/pulseaudio.h`, `xcb/*`, `libavcodec/avcodec.h` and libs `-lpulse -lxcb -lxcb-shm -lxcb-randr -lxcb-xinerama`.
ALSA, JACK and xcomposite are not linked. Every output line is the echoed command `> ...`, a `log LEVEL text` line (the text after `, errno: ` is masked `<E>`), an `event ...` line
(PulseAudio stream/audio events), a state print, or, after `trace on`, a fake system-call trace line starting with `  | `. `obs_module_text(K)` returns `K`.

Settings (shared by all sources): `set-int|set-bool|set-str KEY VALUE`, `get KEY int|bool|str`, `settings-clear`, `source-name NAME`, `clock NS` (fake `os_gettime_ns`), `echo TEXT`.
Properties: `props` (dumps the last property set: `prop NAME type= desc= visible= enabled=` + int range / list items `- "name" = value disabled=`; groups recursed), `modified NAME`,
`modified-in GROUP NAME` (run the property's modified-callback with the current settings; prints the callback result). Fault injection: `trace on|off`, `fail IOCTL_OR_STEP` / `unfail`
(`open`, V4L2 ioctl names without the `VIDIOC_` prefix such as `G_FMT`, `S_FMT`, `QUERYCAP`, `REQBUFS`, ...; Pulse: `server source stream connect ctx`; X: `attach`), `mmap-fail|mmap-ok`, `decoder-fail`.

Fake V4L2: `fake-dir NAME...` (entries of `/sys/class/video4linux`, `d:NAME` = directory, `-` = none), `fake-dir-missing`, `dev PATH` (select/create; later `dev-*` apply to it),
`dev-cap CARD BUS CAPS DEVCAPS`, `dev-input NAME CAPS`, `dev-fmt FOURCC DESC FLAGS`, `dev-std NAME ID`, `dev-dv W H CLOCK HFP HS HBP VFP VS VBP ILVFP ILVS ILVBP INTERLACED`,
`dev-size FOURCC W H`, `dev-size-stepwise FOURCC`, `dev-ival FOURCC W H NUM DEN`, `dev-ival-stepwise`, `dev-ctrl ID TYPE NAME MIN MAX STEP DEF FLAGS`, `dev-menu CTRLID INDEX NAME`,
`dev-cur INPUT W H FOURCC BPL NUM DEN STD` (current kernel state), `dev-reqbufs N`. FOURCC = 4 characters or a number. A failing first enumeration zeroes the `type` field (see S-049 D-2).
The capture thread is never started (`pthread_create` is a trace line `thread start`); `src-thread-run` runs the thread body once with its stop event already signalled.
V4L2 commands: `v4l2-pack A B`, `v4l2-unpack N`, `v4l2-obsfmt FOURCC`, `v4l2-tables`; helpers on the "current fd": `h-open PATH`, `h-set-input N`, `h-input-caps N`, `h-set-format RES|-1 FOURCC|-1`,
`h-set-framerate N`, `h-set-standard N`, `h-set-dv N`, `h-null-args`, `h-create-mmap`, `h-destroy-mmap`, `h-start`, `h-stop`, `h-reset`, `h-controls`; source: `src-defaults`, `src-create`,
`src-update`, `src-destroy`, `src-props`, `src-state`, `src-changed` (prints the restart decision), `src-name`, `src-field pixfmt|width|height|linesize|color_range V`, `src-prep-frame`, `src-thread-run`.

Fake PulseAudio: `pulse-server NAME VERSION DEFAULT_SOURCE DEFAULT_SINK`, `pulse-source NAME DESC MONITOR_OF_SINK|- FORMAT RATE CHANNELS` (FORMAT = a `pa_parse_sample_format` name),
`pulse-sink NAME DESC MONITOR_SOURCE_INDEX|- MONITOR_SOURCE_NAME`, `pulse-chunk data|hole BYTES` (next peek result). Commands: `pulse-fmt NAME`, `pulse-fmt-num N`, `pulse-ch N`, `pulse-chmap LAYOUT_NUMBER`,
`pulse-timecalc FRAMES RATE`, `pulse-info input|output`, `pulse-defaults`, `pulse-create input|output`, `pulse-update`, `pulse-destroy`, `pulse-props input|output`, `pulse-read` (deliver the queued chunk through the read callback),
`pulse-read-nostream`, `pulse-state`.

Fake X11: `x-screen X Y W H [NAME|-] [fail]` (appended; used by RandR as monitors, Xinerama as screens, plain X as screens with origin 0,0), `x-ext shm|randr|xinerama 0|1`, `x-connect-fail 0|1`.
xshm commands: `xshm-version 1|2` (selects `xshm_input` / `xshm_input_v2`), `xshm-info`, `xshm-defaults`, `xshm-create`, `xshm-update`, `xshm-destroy`, `xshm-state`, `xshm-props`, `xshm-geometry`,
`xshm-last-server`. Unknown commands abort the runner (exit 2).

## wincapture runner (spec S-051)

`gen/wincapture_runner.c` (+ `gen/wincapture_shim/`, a minimal fake `Windows.h`) compiles three pieces of **real** reference C on Linux: `libobs/util/windows/window-helpers.c` (against a fake window table), the `shared/obs-hook-config` headers, and `plugins/win-dshow/shared-memory-queue.c` + `tiny-nv12-scale.c` (against fake named file mappings), plus `plugins/win-capture/compat-helpers.c` (against a file in `$CONFORMANCE_TMPDIR`). Commands starting with `m_` are a **MODEL**: logic hand-ported from static C/C++ that cannot be compiled here (window/game/display capture method and settings, WASAPI layout/update/default-device rules, dshow selection, DirectShow encoder sizing); their vectors are source-derived, not produced by the reference binary. Tokens: `-` = NULL; strings may be quoted with C escapes.

Settings store (a fake `obs_data_t`): `s_reset`, `s_defaults KIND` (window_capture, game_capture, monitor_capture, display_capture, wasapi_input, wasapi_output, wasapi_process, dshow, dshow_encoder), `s_set KEY int|bool|str VALUE` (user value; defaults never override user values), `s_dump`, `s_get KEY`.
Fake windows: `win_reset`, `win_add ID TITLE CLASS EXEPATH PID [hidden|iconic|cloaked|tool|child|empty|parent=ID]...` (z-order = insertion order, front first; pid 4242 is the current process; exe path `!noaccess` / `!nopath` make OpenProcess / GetProcessImageFileName fail), `win_fallback 0|1` (FindWindowEx on the desktop returns nothing), `win_info ID`, `win_uwp ID`, `find|find_top include|exclude class|title|exe CLASS TITLE EXE`, `rating ID PRIO CLASS TITLE EXE`, `list include|exclude`, `build_strings STR`, `encode_decode TITLE CLASS EXE`; property list: `prop_clear`, `prop_add NAME VALUE`, `prop_check KEY IDX`.
Hook protocol: `hook_layout`, `hook_names`, `hook_mapping PID`, `hook_set FIELD VALUE`, `hook_hex FROM [COUNT]`. Virtual camera queue: `vq_create SLOT CX CY INTERVAL`, `vq_open SLOT`, `vq_close SLOT`, `vq_state SLOT`, `vq_info SLOT`, `vq_header SLOT`, `vq_write SLOT YLINESIZE YBYTE UVBYTE TS`, `vq_read SLOT DSTCX DSTCY`. Compatibility list: `compat_json JSON|-`, `compat_tr KEY TEXT`, `compat_check game|bitblt|wgc TITLE CLASS EXE`.
MODEL commands: `m_wgc_supported`, `m_wc_method`, `m_wc_update`, `m_wc_visibility`, `m_dup_method`, `m_hook_rate`, `m_convert_format`, `m_gc_config`, `m_audio_changed`, `m_ks_to_layout`, `m_layout_to_ks`, `m_wasapi_new|update|default`, `m_dshow_*`, `m_denc_*`, `m_shmem_layout`, `m_frame_interval`, `m_hook_ver_ok`. Unknown commands abort the runner (exit 2).

## hwencoders runner (spec S-050)

`gen/hwencoders_runner.c` drives wrapper translation units that `#include` the real hardware-encoder
plugin sources (obs-nvenc, obs-qsv11, obs-ffmpeg nvenc/vaapi) against hand-written vendor shims
(`gen/hwencoders_inc/ffnvcodec/`) and recording fakes. Every command echoes itself as `> cmd args`;
fakes print one line per vendor call; the plugins' log output appears as `log <level> <text>`.
Build needs libjansson, libavutil, libavcodec, libva, libpci (headers; libav/libva only for symbols
the vectors never reach). One process per script; process-wide caches (NVENC init) are sticky,
so version-gate scripts are one-per-scenario.

- Host: `host format|pref_format|colorspace|range|fps|size|tex_nv12|tex_p010|scaling|gpu_scaling|hdr_peak|timebase|has_roi|roi_increment V...`, `roi L T R B PRIO`, `roi_clear`.
- Settings: `reset`, `set int|str|bool KEY V`, `get int|str|bool KEY`, `has_user KEY`, `defaults h264|hevc|av1`, `dump 0|1|2` (verbosity of fake NVENC init dump).
- NVENC fake: `nvcap 10bit|444|dyn_bitrate|lookahead|temporal_aq N`, `nvstatus open|caps|preset|init|reconf|bitstream|encode|lock|unlock|seqparams|unmap|destroy NAME|N`, `nvpreset FIELD N`, `nverr TEXT`, `lockpic TYPE PTS SIZE`, `header HEX`.
- NVENC run: `create CODEC tex|soft`, `update`, `state`, `encode PTS`, `encodes FROM TO`, `extra`, `destroy`, `register`, `compat_register`, `load`, `invoke ID create|defaults|properties`.
- Helpers: `ini "TEXT"` (helper-process output), `dll`, `sym maxver|create 0|1`, `maxver HEX`, `pipefail`, `supported`, `fsupported` (forked, reports `crash: signal N`), `initnv`, `caps CODEC`, `vinfo`, `brokensplit`, `props CODEC`, `nvfail NAME [TEXT]`, `errname N`.
- ffmpeg nvenc: `ff_defaults`, `ff_create h264|hevc`, `ff_reconfigure`, `ff_destroy`, `ff_initfail FAIL ERR [ONCE]`, `ff_packet HEX DTS`, `ff_ctx BF TB`, `ff_encode`, `ff_state`, `ff_video_info PREF IN`, `ff_register`.
- VAAPI: `va_caps open|cbr|vbr|qvbr|cqp|bframe|h264|hevc|av1 0|1`, `va_fail STEP`, `va_create`, `va_update`, `va_destroy`, `va_defaults`, `va_encode HEX|- PTS DTS SEND RECV`, `va_video_info`, `va_rc NAME`, `va_rc_modified`, `va_device_modified`, `va_register`, `fcall CMD...` (forked, crash capture).
- QSV: `q_adapter IDX INTEL DGPU AV1 HEVC`, `q_set ovi_adapter|open_ok|enc_ret|bs_present|cpu_platform N`, `q_load`, `q_headers VPS SPS PPS`, `q_bs HEX FRAMETYPE TS DTS`, `q_create h264|av1|hevc tex|soft`, `q_encode PTS [tex|badtex]`, `q_extra`, `q_ts obs|mfx N`, `q_destroy`.

## maccapture runner (spec S-052)

`gen/maccapture_runner.c` is a pure **model** runner: the macOS plugins (ScreenCaptureKit, CoreAudio, AVFoundation, VideoToolbox) cannot be compiled on Linux, so every command re-implements one decision of the reference from reading its source; expected output is source-derived. Only `get_audio_channels()` from `libobs/media-io/audio-io.h` is real C. Arguments are space separated (`-` = empty string); macOS versions are `major*100+minor`; Apple enum/format constants are written by symbolic name.

Commands: `sck_defaults`, `sck_audio_defaults`, `sck_visibility TYPE`, `sck_audio_visibility TYPE`, `sck_content_flags display|window SHOW_HIDDEN`, `sck_update CUR_TYPE CUR_DISPLAY CUR_HIDE_CURSOR CUR_HIDE_OBS CUR_WINDOW CUR_APP NEW_TYPE NEW_DISPLAY NEW_SHOW_CURSOR NEW_HIDE_OBS NEW_WINDOW NEW_APP`, `sck_config TYPE MACOS FPS_NUM FPS_DEN SHOW_CURSOR HAVE_VIDEO_INFO W H`, `sck_window_frame TYPE CONTENT_W CONTENT_H POINTS_TO_PIXELS SCALE CUR_W CUR_H BUF_W BUF_H ERRORED`, `sck_color_space [PREFERRED...]`, `sck_dispatch video|audio AUDIO_ONLY MACOS SC_NULL`, `sck_audio_cfg SPEAKER_ENUM`, `sck_audio_buf BYTES CHANNELS BYTES_PER_FRAME RATE PTS_SECONDS`, `scw_reset|scw_add ID APP TITLE|scw_list SHOW_EMPTY`, `sca_add NAME BUNDLE|sca_list`, `sck_display_name NAME W H X Y`, `sck_display_migrate HAS_LEGACY HAS_NEW UUID_OK UUID_NONEMPTY`; legacy: `cw_reset`, `cw_add NUM PID OWNER NAME`, `cw_init OWNER NAME PID WINDOW`, `cw_find OWNER NAME`, `cw_list WINDOW OWNER NAME SHOW_EMPTY`, `dc_crop_mode`, `dc_visibility`, `dc_size`, `dc_rect`; CoreAudio: `ca_defaults`, `ca_format FLAGS BITS`, `ca_layout N`, `ca_sanitize STR`, `ca_listed INPUT NAME`, `ca_resolve UID INPUT FIRST_OUTPUT`, `ca_retry DEFAULT_CHANGED`, `ca_chmap UID V1..V8`, `ca_chmap_clamp CH AVAIL M...`, `ca_combo OUT AVAIL`, `ca_init_format DOWNMIX AVAIL SPEAKERS`, `ca_buffer N`; AVCapture: `av_defaults FAST`, `av_preset`, `av_aspect W H`, `av_fps_desc MIN:MAX...`, `av_fallback_fps FPS MIN:MAX:MINDURVAL:MINDURSCALE:MAXDURVAL:MAXDURSCALE...`, `av_subtype`, `av_fourcc FMT FULL`, `av_cs`, `av_range`, `av_fourcc_str`, `av_update`; VideoToolbox: `vt_profile CODEC PROFILE FORMAT`, `vt_cs`, `vt_hdr PEAK`, `vt_rc RC APPLE_SILICON MACOS LIMIT KBPS QUALITY MAXKBPS WINDOW`, `vt_visibility RC LIMIT`, `vt_pixfmt FMT FULL CODEC`, `vt_props CODEC HW AS MACOS`, `vt_defaults ...`, `vt_name`, `vt_params RC KEYINT FPS_NUM FPS_DEN QUALITY AQ_MODE`, `vt_update OLD NEW OLD_LIMIT NEW_LIMIT`, `vt_register`, `mach_ids`. Unknown commands abort with exit 2.

## filtersrest runner (spec S-054)

Area `filtersrest` (`gen/filtersrest_runner.c`, `gen/filtersrest_modreg.c`, `gen/filtersrest.sources`). Links the **real**
vendored RNNoise (`plugins/obs-filters/rnnoise/src`), the real system `libspeexdsp`, the real libswresample-backed
`audio-resampler-ffmpeg.c`, the real `noise-suppress-filter.c` and the real `obs-filters.c`; only the OBS host is faked
(sources, audio info). Run `./run.sh` from `test/conformance` (the `-I` path is relative). Float samples are generated
deterministically: generator `silence | sine F A | noise A SEED | harm F A | speechlike F A NA SEED` (see S-054 section 5).

Commands: `audio RATE CH` (host audio), `echo ...`, `try CMD...` (forked, `try-end ok|crash|hang`),
`rnn_info`, `rnn_create S`, `rnn_destroy S`, `rnn_gen S GEN...`, `rnn_run S N [inplace] [every=K]` (N frames of 480 int16-scale
samples; prints `vad`, `rms_in`, `rms_out`, `peak_out`), `rnn_cmp S1 S2 N` (identical output of two states),
`spx_create S FRAMES RATE`, `spx_ctl S LEVEL` (set, then get), `spx_gen S GEN...`, `spx_run S N FRAMES RATE [every=K]`,
`spx_destroy S`, `ns_create ID v1|v2 METHOD|- LEVEL [mono-parent]`, `ns_gen ID CH GEN...` (float units), `ns_process ID FRAMES TS`
(prints returned frames/timestamp/per-channel rms or `null`, and queued sizes), `ns_destroy ID`, `module` (runs `obs_module_load`
and lists the registrations in order). Values are printed rounded (`%.3f`, integer rms) and compared byte-exactly; a port
using another RNNoise/Speex implementation compares within the S-054 section 5 tolerances.

## gfximages runner (spec S-053)

Area `gfximages`: libobs image decoding and the built-in effects. `gen/gfximages_runner.c` links the **real** `libobs/graphics/image-file.c`,
`graphics-ffmpeg.c` (through `gen/gfximages_ffwrap.c`, a translation unit that `#include`s the C file and adds hooks into its static
functions), the vendored `libnsgif` (`gif.c`, `lzw.c`), `graphics.c`, the effect parser and the shader parser. The graphics device is a fake
(`gen/gfximages_stubs.c`): textures keep their pixels, shaders expose the globals of their text as parameters. Needs FFmpeg libraries
(`-lavformat -lavcodec -lswscale -lavutil`). The scripts are generated by `python3 gen/gfximages_gen.py` (fixtures: `gen/gfximages_fixtures.py`;
GIF with an LZW encoder, PNG with zlib, BMP, PPM; embedded as hex in the scripts). The scratch directory (`$CONFORMANCE_TMPDIR`) is the working directory
of the runner, so file names and log messages are relative; FFmpeg error text in log lines is replaced by `<ffmpeg-error>`.

- Files: `file NAME HEX` (write a fixture), `trunc NAME NEW NBYTES` (copy the first bytes).
- libnsgif directly: `gif_new NAME [rgba|bgra|argb|abgr]`, `gif_delay MIN DEFAULT`, `gif_scan [LEN]` (feed the first LEN bytes of the file, cumulative), `gif_complete`, `gif_info`
  (header + frame table), `gif_frame N` (decode, prints the canvas as RGBA hex), `gif_prepare [COUNT]`, `gif_reset`, `gif_palette global|local N`, `gif_destroy`. Results are the library's error strings.
- `gs_image_file_ex`: `img_init NAME straight|srgb|premul`, `img_init_null`, `img_try NAME MODE` (init in a forked child killed after 2 s; prints `hang`, `crash` or
  `terminated by the crash handler`), `img_state`, `img_pixels` (still pixels), `img_tex_init`, `img_tex` (fake texture contents and map count), `img_tick NS [COUNT]`, `img_update`, `img_free`.
- Still images: `texfile NAME MODE` (`gs_create_texture_file_data3`), `texfile_missing` (null/empty/missing names), `reformat PIX W H LINESIZE ORIENT MODE HEX` (a fabricated decoded frame
  with metadata `Orientation=ORIENT` through the real reformat code; PIX = rgba|bgra|bgr0|rgba64be|rgb24|gray8|yuv420p).
- Effects: `effect PATH` (repository relative, e.g. `libobs/data/default.effect`): parser result, parameters with defaults, techniques, passes with entry functions and bound parameters, sampler states.
- `echo WORDS...`. Unknown commands abort the runner (exit 2).

## utilrest runner (spec S-055)

`gen/utilrest_runner.c` (+ `utilrest_stubs.c`, `utilrest_pafake.c`, `utilrest_api.h`, registration `utilrest.sources`) links the real
`util/crc32.c`, `pipe.c`, `pipe-posix.c`, `source-profiler.c`, `platform-nix-dbus.c`, `platform-nix-portal.c` and
`audio-monitoring/pulse/*`. Requirements: libpulse, glib/gio development files and `dbus-daemon` (the D-Bus scripts start a private session bus).
Log lines print as `log <level> <text>` (non-printables as `\xHH`); text is compared. `pa ...` lines are the fake PulseAudio server
seeing stream calls; `svc ...` lines are the fake D-Bus service seeing calls. Slots (`<s>`, `<i>`) are small integers chosen by the script.

| Command | Meaning |
|---|---|
| `crc32 <seed-hex> <hex\|->`, `crc32s <seed-hex> "text"`, `crc32_null0 <seed-hex>` | `calc_crc32` |
| `popen <s> <type\|-> <cmd\|->`, `args <exe> [arg..]` (`%x` = `add_argf("%s-%d")`), `popen2 <s> <type\|->` | `os_process_pipe_create` / `create2` (uses the last `args`) |
| `pread <s> <n>`, `pread_err <s> <n>`, `pwrite <s> "data"`, `pwrite_big <s> <n>` | read / read_err / write (prints byte counts and quoted data) |
| `pclose <s>`, `pclose_signalled <s>`, `pclose_null`, `pnull_ops` | destroy (`ret=`; `ret_is_positive=` for signalled children), NULL handles |
| `sp_clock <ns>`, `sp_adv <ns>` | fake clock |
| `sp_enable <0\|1>`, `sp_gpu <0\|1>`, `sp_reset_video <num> <den>`, `sp_frame_begin`, `sp_frame_collect`, `sp_render_begin`, `sp_render_end` | profiler frame API |
| `sp_source <i> <async>`, `sp_tick <i> <ns>`, `sp_render <i> <cpu-ns> [timer-ticks]`, `sp_async_recv <i>`, `sp_async_ts <ns>` | per-source sampling (the clock advances by the given time) |
| `sp_gfx_cfg <ticks> <disjoint> <freq> <data_ok>`, `sp_gfx` | fake GPU timers / print create-destroy counters |
| `sp_remove <i>`, `sp_run_tasks`, `sp_result <i>`, `sp_get <i>`, `sp_fill_null` | removal queue, `fill_result` (prints `fill=0/1` and `untouched=` when false), `get_result` |
| `pa_server <name> <ver> <default\|->`, `pa_sink <name> <desc> <u8\|s16\|s32\|f32\|s24\|alaw> <rate> <ch>`, `pa_cfg <key> <n>` | configure the fake PulseAudio (`connect_fails`, `ops_fail`, `stream_new_fails`, `connect_ret`, `begin_size`, `begin_ret`) |
| `pa_available`, `pa_enum [stop_after]`, `pa_default_id`, `pa_match <a\|-> <b\|->`, `pa_init`, `pa_unref`, `pa_stats` | wrapper / enumeration; `pa_stats` prints mainloop / context counters |
| `mon_dev <id\|->`, `mon_src <i> <flags> <volume> <device_id-setting\|-> <activate_refs>`, `ao <rate> <speakers>`, `resampler_fail <0\|1>` | engine state |
| `mon_create <s> <src>`, `mon_play <s> <frames> <v0> <step>`, `mon_reset <s>`, `mon_destroy <s>`, `mon_destroy_null` | audio monitor (`mon_play` feeds planar float, L = v, R = -v) |
| `dbus_bus none\|private`, `dbus_service ss\|pm\|mate\|gnome\|portal ..`, `dbus_fail <member>`, `dbus_ok <member>`, `dbus_delay <ms>` | choose / configure the bus and the fake service |
| `dsi_create/inhibit/destroy <s> ..`, `pii_create/inhibit/destroy <s> ..`, `*_destroy_null` | `dbus_sleep_info_*`, `portal_inhibit_*` (`inhibit <s> "reason" <0\|1>`) |
| `dbus_wait <n>`, `dbus_response <code>`, `dbus_log` | pump the main loop until n new service calls (max 5 s) + 150 ms; emit the portal `Response`; print and clear the service's call log |

A failing `mon_reset` leaves the monitor in the reference's broken state; scripts do not destroy it (defect D-3 of S-055).

## pipewire runner (spec S-056)

`gen/pipewire_runner.c` (+ `pipewire_stubs.c`, `pipewire_api.h`, registration `pipewire.sources`) links the REAL
`plugins/linux-pipewire/{formats,portal,screencast-portal,camera-portal,pipewire,linux-pipewire}.c` plus libobs `obs-data.c` /
`obs-properties.c`, real GLib/GIO (GVariant, GCancellable, GUnixFDList) and real `libpipewire` property/SPA pod code. Faked by script:
the OBS core (`host.*`), graphics (`gs.*`, `gl.*`), the D-Bus portals (`dbus.*`: bus, proxies, cached properties, async calls,
Request `Response` signals) and the PipeWire client API (`pw.*`: thread loop, context, core, registry, node proxies, streams, loop events,
buffers). Needs glib/gio, libpipewire-0.3 (+spa), libdrm headers (paths in `pipewire.sources` are the Arch/Debian defaults); run `run.sh`
from `test/conformance`. Every fake prints one line per call; pods handed to PipeWire are printed as
`Object(type=..,id=..){key=Type:value; ..}`. Pointers (`%p` of streams/registry/events), the PipeWire header version, the jansson error text
and the (use-after-free) device id of "Removed device" are masked.

| Command | Meaning |
|---|---|
| `unique_name :1.N`, `bus ok\|fail`, `proxy ScreenCast\|Camera ok\|fail`, `prop IFACE NAME "<GVariant text>"` | D-Bus environment (cached proxy properties such as `AvailableSourceTypes`, `AvailableCursorModes`, `version`) |
| `screencast_load/unload`, `camera_load/unload`, `module_load/unload` | plugin entry points (registration is printed) |
| `set NAME KEY int\|bool\|dbl\|str VALUE`, `get NAME KEY ..`, `settings_json NAME`, `outflags NAME HEX`, `new NAME SOURCEID`, `update/save/show/hide/size/render/destroy NAME`, `name NAME` | source instance (settings, defaults are applied by `new`; `outflags` overrides the output flags after `new`) |
| `props NAME`, `dump NAME`, `click NAME PROP`, `modified NAME PROP [GROUP]` | properties: build+print, print, button, modified callback (optionally inside group `controls`) |
| `finish N ok "<variant>"`, `finish N fd`, `finish N error cancelled\|failed MSG` | complete the N-th (1-based) async D-Bus call |
| `response INDEX\|PATH "(code, {..})"` | deliver the Request `Response` signal to subscription INDEX (1-based) or to the active subscription with that path |
| `portal_ok NODE` | drive the newest screencast flow to the end (all steps granted, stream NODE) |
| `video W H NUM DEN`, `gs_dmabuf FLAGS DRMNAME..`, `gs_dmabuf_none`, `gs_modifiers DRMNAME MOD..`, `gs_sync 0\|1`, `dmabuf_import ok\|fail` | graphics environment |
| `pw_server_version V\|-`, `pw_connect ok\|fail`, `pw_thread ok\|fail`, `pw_bind ok\|fail`, `core_error ID SEQ RES MSG`, `core_done ID SEQ` | PipeWire environment / core events |
| `stream N`, `param_changed [null] [id= subtype= format= size=WxH fps=n/d modifier= cm= cr= media=]`, `state_changed OLD NEW [ERR]`, `process`, `run_events` | stream events on the selected stream; `run_events` runs signalled loop events (renegotiation) |
| `buf_data ID IDX memptr\|memptr-null\|dmabuf\|syncobj TAG OFFSET STRIDE SIZE FLAGS FILL`, `buf_meta ID header\|crop\|transform\|cursor\|sync ..`, `buf_push ID` | build a PipeWire buffer (fd identity = TAG) and queue it for dequeue |
| `registry_global ID TYPE VER k=v..`, `registry_global_remove ID`, `node_info ID PARAMID:FLAGSHEX..`, `node_param ID SEQ\|async:N PARAMID PODNAME`, `proxy_done ID SEQ`, `proxy_removed ID` | registry / node proxy events (camera) |
| `pod_enum_format NAME raw\|mjpg\|h264 FMT W H KIND:fr,fr,.. [sizerange]`, `pod_prop_info NAME ID DESC int\|bool\|float none\|range\|enum\|step LABELS V..`, `pod_dump NAME` | build EnumFormat / PropInfo pods (`KIND` = none, enum, range, step, int, omit) |
| `format NAME\|ID`, `format_null NAME`, `leaks`, `echo` | format table lookup; texture / lock-depth counters |

## glbackend runner (spec S-057)

`gen/glbackend_runner.c` (+ `glbackend_stubs.c`, registration `glbackend.sources`) links the REAL OpenGL backend
`libobs-opengl` (X11/EGL platform glue, helpers, textures, stage surfaces, buffers, shaders and the OBS-HLSL to GLSL
translator) and drives its `device_*` / `gs_*` exports directly on a REAL headless OpenGL context: the runner starts a
private `Xvfb` (`-displayfd`, no network) and forces Mesa software rendering (`LIBGL_ALWAYS_SOFTWARE`, `GALLIUM_DRIVER=llvmpipe`,
the Mesa EGL vendor file) so that the host GPU is never used. Needs: `Xvfb`, Mesa EGL/GL, libX11, libxcb, libdrm headers. Run from
`test/conformance`; vectors were produced with Mesa llvmpipe 26.2 / OpenGL 4.6 core (`99_driver_dependent` varies by driver). Only
ERROR and WARNING log lines are printed (`log error ...`); initialisation logs (driver strings) are dropped.
`GLBACKEND_VERBOSE=1` prints every log line to stderr (debugging aid). Names of objects are script identifiers;
`-` means NULL. Enum/flag arguments accept `NAME`, `GS_NAME`, numbers, or `A|B`. Hex data may be written `rep:N:HEX`.

| Command | Meaning |
|---|---|
| `create [ADAPTER]`, `destroy`, `enter`, `leave`, `info`, `adapters`, `size`, `avail`, `flush` | device lifecycle and getters |
| `tex NAME W H FMT LEVELS FLAGS [HEX[,HEX..]\|-]` | `device_texture_create` (comma separated hex = one buffer per level); prints `ok`/`fail` |
| `texinfo NAME`, `read NAME` | getters; `read` stages the texture and prints `row N <hex>` per row at the reported linesize |
| `texmap NAME HEX` | map a dynamic texture, copy tightly packed rows, unmap |
| `cube NAME SIZE FMT LEVELS FLAGS`, `cubeinfo`, `vol NAME W H D FMT LEVELS FLAGS`, `volinfo`, `zs NAME W H FMT`, `free NAME` | other objects |
| `stage W H FMT SRC`, `copy DST SRC`, `copyr DST DX DY SRC SX SY SW SH` | stage surface with arbitrary format; copies |
| `set_rt TEX\|- [ZS\|-]`, `set_cube_rt`, `get_rt`, `clear FLAGS R G B A [DEPTH STENCIL]` | render target and clear |
| `viewport X Y W H`, `get_viewport`, `scissor X Y W H\|off`, `cull MODE`, `get_cull`, `blend on\|off`, `blendfn S D`, `blendfn_sep ..`, `blendop OP`, `depth on\|off`, `depthfn F`, `stencil on\|off`, `stencil_write on\|off`, `stencilfn SIDE F`, `stencilop SIDE FAIL ZFAIL ZPASS`, `colormask R G B A`, `srgb on\|off`, `get_srgb` | state |
| `ortho L R T B N F`, `frustum ..`, `proj_push`, `proj_pop`, `view_translate X Y Z` | projection / view matrix (the matrix stack is stubbed) |
| `sampler NAME FILTER U V W ANISO BORDER`, `load_sampler NAME\|- UNIT`, `load_tex NAME\|- UNIT` | samplers / texture units |
| `vshader NAME` / `pshader NAME` followed by shader text lines and a line `end` | shader creation (prints `ok`/`fail`; error text only as `present`) |
| `load_vs`, `load_ps`, `cur_shaders`, `params SHADER` | shader binding; parameter listing (name, type, viewproj/world presence) |
| `set SHADER PARAM KIND VALUES..` | KIND = `bool int float vec2 vec3 vec4 mat4 mat3 tex sampler default raw` (`raw HEX` = `gs_shader_set_val`) |
| `vb NAME NUM FLAGS COLORS NTEX` + NUM lines `x y z [AABBGGRR] [u v]..` + `end`; `ib NAME TYPE IDX..`; `load_vb`, `load_ib` | buffers (colour hex is the little-endian u32, bytes R,G,B,A) |
| `draw MODE START NUM`, `echo TEXT` | draw; comment line copied to the output |

## obsvideo runner (spec S-058)

`gen/obsvideo_runner.c` + `gen/obsvideo_nix.c` (+ `obsvideo_api.h`, registration `obsvideo.sources`) #include the REAL
`libobs/obs-video.c`, `obs-video-gpu-encode.c`, `obs-nix.c`, `obs-nix-platform.c` and `obs-nix-x11.c`. Faked by script: the graphics device
(`gs_*`, one `ev ...` line per call when `trace 1`), effects/techniques, the video output (`video_output_*`), sources, encoders, views,
displays, the profiler, the clock (`os_gettime_ns`/`os_sleepto_ns`), the encode semaphore and thread creation (the encode thread body is
run synchronously with a token budget), and the X server (Xlib/xcb: setup, keyboard mapping, keymap query, XInput raw events, `XLookupString`).
The runner re-executes itself from a private directory (`$CONFORMANCE_TMPDIR/obsvideo-work-<pid>/bin/runner`) so that the executable-relative
data path is deterministic; the install prefix is the relative path `prefix`. `obs-nix-wayland.c` is not covered. Needs libX11/libxcb headers.

| Command | Meaning |
|---|---|
| `trace 0\|1`, `echo TEXT`, `core INTERVAL_NS`, `counters`, `sem`, `freed`, `tasks_run` | trace switch, print, (re)create the fake core with the frame interval, print loop counters / semaphore posts / freed mixes / executed tasks |
| `clock T`, `clock_step N`, `video_time T`, `sleep_result DEFAULT [SEQ..]` | fake clock (advances N per `os_gettime_ns`), `obs->video.video_time`, `os_sleepto_ns` results (default and optional per-call sequence; 1 = deadline reached, moves the clock there) |
| `mix_add NAME HASVIEW BASE_W BASE_H OUT_W OUT_H FORMAT GPUCONV SCALE SPACE` | add a mix (formats `I420 NV12 I444 I010 P010 P216 P416 BGRA RGBA BGRX YUY2 NONE`); creates textures, per-plane staging surfaces (byte pattern `(k*7+ch*13+1)&255`), output frame buffers |
| `mix_active NAME RAW GPU`, `mix_set NAME FIELD V`, `mix_view_none`, `mix_view_copy DST SRC`, `render_tex_size NAME W H`, `stop_after NAME N`, `mix_dump NAME` | consumer counters; fields `texture_rendered render_space base_w base_h scale cur_texture gpu_conversion convert_null surf_null`; output size; `video_output_stopped` turns true after N calls |
| `effect_passes NAME N`, `effect_missing NAME` | technique pass count (effects `default bicubic lanczos area bilinear_lowres conversion`), set an effect pointer to NULL |
| `surf_cfg MIX T C LINESIZE [fail]`, `surf_rows MIX T C ROWS`, `lock_cfg MIX OK LS0 LS1 LS2`, `out_dump MIX PLANE LEN`, `out_clear MIX` | staging surface line size / rows / map failure, line sizes handed out by `video_output_lock_frame` (and its result), dump / clear the delivered frame |
| `source_add NAME [removed]`, `source_removed NAME 0\|1`, `source_removes R TARGET`, `source_refs NAME`, `cb_add tick\|draw\|rendered NAME`, `display_add NAME`, `data_valid 0\|1`, `task_add NAME` | sources (ticked most recent first), callbacks, displays, queued graphics tasks |
| `ctx_new INTERVAL [LAST_TIME]`, `loop N`, `thread` | create the loop context, run N frames (prints `loop -> <bool> ...`), run `obs_graphics_thread` until the stop condition |
| `vframe_push MIX raw\|gpu TS COUNT`, `vframe_clear MIX raw\|gpu` | edit the vframe-info deques |
| `enc_add NAME MIX TB_NUM TB_DEN DIVISOR TEX2 SUCCESS RECEIVED`, `enc_set NAME FIELD V`, `enc_pair ENC OTHER`, `enc_group ENC GROUP N_ENC STARTED START_TS`, `ready_group ENC`, `group_ts ENC`, `enc_dump NAME` | GPU encoders (fields `dead first_received first_raw_ts start_ts cur_pts reconfigure divisor_counter pause success received`), pairing, groups, `add_ready_encoder_group`, state dumps |
| `gpu_init MIX [tex_fail]`, `pthread_create_result N`, `gpu_thread MIX BUDGET`, `gpu_stop MIX`, `gpu_free MIX`, `gpu_queue_push MIX NAME COUNT TS LOCKKEY RELEASED`, `gpu_queue_clear`, `gpu_avail_trim MIX N`, `gpu_queue_dump MIX [full]` | GPU encode pool and thread body |
| `nix_platform_get/set N`, `nix_display_set fake\|null`, `nix_module_ext`, `nix_file cwd\|exe\|prefix NAME`, `nix_unfile ..`, `nix_find NAME`, `nix_env NAME VALUE\|-`, `nix_system_info` | platform selector, data-file search order (paths masked as `<work>`), system information (machine specific lines reduced to labels) |
| `x11_server up\|down`, `x11_vendor VENDOR REL MAJOR MINOR`, `x11_screen N`, `x11_setup MINKC MAXKC SYMS`, `x11_map CODE SYM..` (names such as `XK_Return` or numbers), `x11_kb_fail`, `x11_keymap_fail`, `x11_keys_down CODE..`, `x11_lookup CODE TEXT\|-`, `x11_event press\|release\|other\|nongeneric DETAIL`, `x11_stats` | fake X server |
| `hk_init`, `hk_free`, `hk_keycodes KEY`, `hk_base KEY`, `hk_pressed KEY`, `hk_translation KEY TEXT\|-`, `hk_to_str KEY [prefill]`, `hk_from_vk SYM`, `hk_to_vk KEY`, `hk_combo MODS KEY` | hotkey backend (KEY = `OBS_KEY_*` name or `#N`) |

Output lines: `ev ...` (trace), `log <level> <text>`, and the dump lines above. Pointers, host values (CPU, memory, kernel, distribution) and the
private work directory never appear in the vectors.

## luascript runner (spec S-060)

`gen/luascript_runner.c` drives the REAL Lua backend (`shared/obs-scripting/obs-scripting-lua*.c`, LuaJIT) with
Lua code embedded in the scripts. libobs is faked in `gen/luascript_stubs.c`; the SWIG runtime header and the `obslua`
module are stand-ins (`gen/luascript_swig/`, see spec S-060 section 1). Needs `luajit` dev files, `jansson`.
Lua source is written verbatim between `lua_begin NAME` and `lua_end` (raw lines, no tokenizing) to a private directory.
Handles: `S0..S5` scripts, `P0..P3` property sets, `H0..H3` signal handlers, sources by name.

| Command | Meaning |
|---|---|
| `scripting load\|unload` | `obs_scripting_load/unload` (prints the number of libobs tick callbacks) |
| `create Sn FILE [s:k=v i:k=n b:k=0\|1 d:k=x]`, `create Sn raw:PATH`, `create Sn -` | `obs_script_create` (file in the private dir, raw path, NULL) |
| `destroy Sn`, `reload Sn`, `update Sn [kv..]`, `save Sn [spec..]`, `settings Sn spec..`, `callbacks Sn` | script API; `spec` = `s:key`, `i:key`, `b:key`, `d:key`, `u:key` (has user value); `callbacks` counts the script's live/removed records |
| `props Sn Pn`, `click Pn NAME`, `modify Pn NAME Sn` | `get_properties` (lists name:type:description), button click, modified callback |
| `time NS`, `tick SECONDS [NS]`, `render CX CY` | set frame time / call the registered libobs tick and render callbacks (registration order) |
| `barrier` | drain the deferred queue |
| `handler Hn "decl"`, `emit Hn SIGNAL [s:k=v i:k=n ..]` | real signal handler and emission |
| `hotkey ID 0\|1`, `fe_event N`, `fe_save 0\|1 [spec..]`, `fe_state`, `counts` | libobs/frontend side events and registration counts |
| `fake_source NAME [nfilters]`, `scene_items N`, `refs`, `swig_hide "TYPE *" 0\|1` | object fixtures, reference counts, hide a type from the SWIG stand-in |
| `src_types`, `src_defaults ID spec..`, `src_create ID NAME [kv..]`, `src_call NAME OP [..]`, `src_free ID` | drive Lua-registered source types as libobs would |
| `try CMD..` | run one command in a forked child, print `crash signal N` if it dies (reference defects) |
| `echo ...` | comment line in the output |

Output: `log <level> <text>` (chunk names inside Lua errors printed as `[string "<chunk>"]`, the private directory as `$TMP`; levels
other than error/warning/info/debug as the number), `fake ...` lines from the libobs fakes, and the result lines above.

## mpegtsout runner (spec S-059)

`gen/mpegtsout_runner.c` (+ `mpegtsout_stubs.c` fake libobs, `mpegtsout_net.c` fake libsrt / librist sender / loopback sockets /
MPEG-TS analyser, registration `mpegtsout.sources`) includes the REAL `plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c` with its SRT / RIST
headers; libavformat's MPEG-TS muxer is real. Needs libavformat/libavcodec/libavutil, librist (address parsing) and the libsrt headers
(the library itself is not linked). Every command is echoed as `> ...`.

| Command | Meaning |
|---|---|
| `world KEY VALUE..` | fake libobs knobs: `fps N [D]`, `size W H`, `vformat`, `colorspace 601\|709\|srgb\|pq\|hlg\|default`, `range`, `vcodec`, `acodec`, `tracks`, `abitrate I V`, `vbitrate`, `keyint`, `framesize`, `samplerate`, `speakers`, `vinfo/ainfo/vextra on\|off`, `aextra I on\|off`, `service on\|off`, `url/user/pass/streamid/passphrase` (`-` = NULL, `@PORT@` = loopback test port), `can_begin`, `init_ok`, `pause_offset`, `hdr_peak` |
| `setting KEY VALUE`, `log LEVEL` | output setting (`muxer_settings`); lowest printed log level (default warning) |
| `info`, `props`, `create`, `start`, `stop TS`, `stop_async TS`, `wait`, `destroy` | registration data / lifecycle (`start`, `stop` wait for the start/stop thread) |
| `pkt v\|a TRACK PTS DTS KEY SIZE [SYS_DTS_US]`, `pktnull`, `drain`, `wait_event N`, `settle` | feed packets (Annex B payload for video), wait until the write thread has consumed them / N stop signals were recorded / 50 ms |
| `state`, `events`, `cfg service\|video\|audio\|muxer\|all`, `ts`, `sends`, `capture_clear` | print output state / recorded stop codes and last error / derived configuration / structural MPEG-TS report of captured bytes / chunk statistics |
| `srt_open URL [STREAMID\|- [PASS\|-]]`, `srt_write N`, `srt_close`, `rist_open URL [SECRET\|- [USER\|- [PASS\|-]]]`, `rist_write N`, `rist_close`, `risterr N` | call the URL layer directly |
| `srt_fail NAME ERR [ERRNO [NTH]]`, `rist_fail NAME [RET [NTH]]`, `srt_epoll ready\|timeout\|error [ERR [SKIP]]`, `srt_state NAME\|auto`, `srt_payload N\|auto`, `srt_reject badsecret\|N`, `srt_stats TS RATE SENT RETR DROP`, `srt_reset_net` | fault / behaviour injection into the fake transport |
| `rist_stats`, `rist_stats_other`, `clock_skew S`, `write_fail N RET`, `hold on\|off`, `blind N`, `crash_start` | fire the stats callback, skew wall clock, fail the nth muxer write, hold the write thread, hide the stop event N times, run start in a child (crash capture) |
| `sock_listen tcp\|udp`, `sock_close` | real loopback receiver for udp:// / tcp:// URLs |
