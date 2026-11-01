# S-020 Effect language: effect parser, shader parser, effect runtime

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `graphics/effect-parser.c/.h`, `graphics/shader-parser.c/.h`, `graphics/effect.c/.h`, and the effect entry points of `graphics/graphics.c` (`gs_effect_create`, `gs_effect_create_from_file`) |
| Target crate | `crates/obs-graphics` (module `effect`) |
| Depends on | S-001 (allocator), S-011 (lexer, preprocessor, parser helpers: referenced rule by rule, not repeated) |
| Conformance | `test/conformance/scripts/effect/` (41 scripts, runner `gen/effect_runner.c` + fake backend `gen/effect_stubs.c`), runner language in `test/conformance/README.md` ("effect runner") |

## 1. Scope

An *effect file* (`*.effect`, ~50 ship with OBS and plugins load their own) is an HLSL-like text with global
parameters, structs, sampler states, functions and *techniques* made of *passes*. This component:

1. **parses** the file (effect parser), keeping the declared parameters (type, annotations, default values) and,
   per pass, the *entry call* of the vertex and the pixel stage;
2. **assembles the text of one shader per pass and stage** from exactly the declarations that stage needs, and
   hands it to the graphics backend (`gs_vertexshader_create` / `gs_pixelshader_create`); backends compile or
   transpile that text (S-0xx, WGSL transpiler in Phase 3) - so the exact text, layout included, is an interface;
3. provides the **effect runtime** (`gs_effect_*`, `gs_technique_*`, `gs_param_*`): parameter lookup, value
   storage, defaults, the technique/pass iteration protocol that uploads values to the backend;
4. provides the stand-alone **shader parser** (`shader_parse`, `get_shader_param_type`, `get_sample_filter`,
   `get_address_mode`, `shader_sampler_convert`) that backends run over the assembled text.

Out of scope: the backends themselves, `gs_effect_*` helpers that only forward to graphics state
(`gs_draw_sprite`, ...), the lexer/preprocessor (S-011).

Callers: libobs `obs_init_graphics` and every source/filter/transition plugin (`gs_effect_create_from_file`,
`gs_effect_loop`, `gs_effect_set_*`), the frontend, and the OpenGL / D3D11 / Metal backends (shader text, `shader_parse`).
Behaviour on valid input is essential; behaviour on malformed input is characterised because effect files are
user-supplied by third-party plugins.

## 2. Terminology and conventions

RFC 2119 words. All text is bytes (UTF-8 passes through; only ASCII matters). "Token" is an S-011 cf token (after
preprocessing); *blank* tokens are SPACETAB and NEWLINE tokens (comments are SPACETAB). `cur` is the parser cursor
(S-011 section 5.5); every error is recorded at `cur` with the rules of S-011-R71 (position = start of the current
token in the lexer that owns it, file = that lexer's file name).

**Cursor helpers** (all defined in S-011 section 5.5; this spec uses these short names):

| name | S-011 | meaning |
|---|---|---|
| `NEXT()` | R72 `cf_next_token` | advance one token (not if `cur` is blank/NONE), skip blanks; true unless NONE |
| `NEXTV()` | R73 | `NEXT()`; on false add `Unexpected EOF` |
| `PEEKV(t)` | R73 | next non-blank token into `t` without moving; on false add `Unexpected EOF` |
| `IS(s)` | R74 | current token text equals `s` exactly |
| `GOTO(a,b)` | R75 | advance to the next token equal to `a` or `b` (skips balanced `{...}` blocks); false at end |
| `GOTOV(a,b)` | R75 | `GOTO` + `Unexpected EOF` on false |
| `PASSPAIR` | R77 | `cf_pass_pair('{','}')` |
| `SHOULD_BE(s,g1,g2)` / `NEXT_SHOULD_BE(...)` | R78 | SUCCESS / CONTINUE (error `Expected 's'`) / EOF |
| `IS_TYPE(ty,what,g)` | R79 | SUCCESS or `Expected 'what'` (+ skip to `g`) then CONTINUE/EOF |
| `GET_NAME(dst,what,g)` / `NEXT_NAME` | R80 | `IS_TYPE(NAME,...)` + copy |
| `expecting(s)`, `eof()`, `syntax()` | R71 | add `Expected 's'`, `Unexpected EOF`, `Syntax error` |

Return codes (used by the grammar procedures): `SUCCESS = 0`, `CONTINUE = -1`, `BREAK = -2`,
`UNEXPECTED_CONTINUE = -3`, `UNEXPECTED_BREAK = -4`, `EOF = -5`.

Numbers: "long" is the C `long` of the reference platform (8 bytes, little endian, on the committed vectors; 4 bytes on
Windows - Q-6). Floats are IEEE binary32. `%.9g` printing, no tolerance needed: all values in the vectors are exact
copies of parsed text. The runner prints the raw bytes (hex) of every stored value.

## 3. Interface

### 3.1 Types

```c
enum gs_shader_param_type { UNKNOWN=0, BOOL=1, FLOAT=2, INT=3, STRING=4, VEC2=5, VEC3=6, VEC4=7,
                            INT2=8, INT3=9, INT4=10, MATRIX4X4=11, TEXTURE=12 };
enum gs_shader_type       { GS_SHADER_VERTEX = 0, GS_SHADER_PIXEL = 1 };
enum gs_sample_filter  { POINT=0, LINEAR=1, ANISOTROPIC=2, MIN_MAG_POINT_MIP_LINEAR=3, MIN_POINT_MAG_LINEAR_MIP_POINT=4,
                         MIN_POINT_MAG_MIP_LINEAR=5, MIN_LINEAR_MAG_MIP_POINT=6, MIN_LINEAR_MAG_POINT_MIP_LINEAR=7,
                         MIN_MAG_LINEAR_MIP_POINT=8 };
enum gs_address_mode   { CLAMP=0, WRAP=1, MIRROR=2, BORDER=3, MIRRORONCE=4 };
struct gs_sampler_info { enum gs_sample_filter filter; enum gs_address_mode address_u, address_v, address_w;
                         int max_anisotropy; uint32_t border_color; };
struct gs_shader_texture { gs_texture_t *tex; bool srgb; };      /* 16 bytes on LP64: pointer, flag, 7 padding bytes */
struct gs_effect_param_info { const char *name; enum gs_shader_param_type type; };
```

Opaque handles: `gs_effect_t`, `gs_eparam_t` (an effect parameter or an annotation), `gs_technique_t`, `gs_epass_t`.
Internal layouts that matter: an effect parameter holds `name`, `type`, `changed`, `cur_val` (byte buffer, initially
empty), `default_val` (byte buffer), `next_sampler`, and its list of annotation parameters (same struct, section
ANNOTATION). A pass holds `name` (NULL for unnamed), the vertex and pixel `gs_shader_t` and, per stage, the list of
`(effect parameter, shader parameter)` pairs to upload. An effect holds the parameters, techniques, the current
technique / pass, and the cached `ViewProj` and `World` parameters (`scale` is unused).

### 3.2 Functions (exported, replaced by the crate)

Effect runtime (`effect.c`): `gs_effect_destroy`, `gs_effect_get_technique`, `gs_effect_get_current_technique`,
`gs_effect_loop`, `gs_technique_begin`, `gs_technique_end`, `gs_technique_begin_pass`, `gs_technique_begin_pass_by_name`,
`gs_technique_end_pass`, `gs_technique_get_pass_by_idx`, `gs_technique_get_pass_by_name`, `gs_effect_update_params`,
`gs_effect_get_num_params`, `gs_effect_get_param_by_idx`, `gs_effect_get_param_by_name`, `gs_param_get_num_annotations`,
`gs_param_get_annotation_by_idx`, `gs_param_get_annotation_by_name`, `gs_effect_get_viewproj_matrix`,
`gs_effect_get_world_matrix`, `gs_effect_get_param_info`, `gs_effect_set_bool/float/int/matrix4/vec2/vec3/vec4/color/texture/texture_srgb/val`,
`gs_effect_get_val`, `gs_effect_get_val_size`, `gs_effect_get_default_val`, `gs_effect_get_default_val_size`,
`gs_effect_set_default`, `gs_effect_set_next_sampler`.

Creation (`graphics.c`): `gs_effect_t *gs_effect_create(const char *text, const char *filename, char **error_string)`,
`gs_effect_create_from_file(path, error_string)`. Shader parser (`shader-parser.c`):
`shader_parse(struct shader_parser *, const char *text, const char *file)` with the result tables, `shader_parser_geterrors`,
`get_shader_param_type`, `get_sample_filter`, `get_address_mode`, `shader_sampler_convert`.

Backend boundary used by the effect code (the crate calls these through its backend trait):
`create_vertex_shader(text, location, &mut errors) -> Option<Shader>`, `create_pixel_shader(...)`, `shader_get_param_by_name`,
`shader_get_param_info`, `load_vertex_shader(Option)`, `load_pixel_shader(Option)`, `shader_set_val(param, bytes)`,
`shader_set_texture(param, Option<texture>)`, `shader_set_next_sampler(param, sampler)`, `shader_destroy`,
`preprocessor_name() -> Option<&str>`. All `gs_*` entry points need a current graphics context; without one they log at
DEBUG level and fail (return NULL/false) - not exercised by the vectors.

### 3.3 Thread-safety

The effect list of the graphics context is protected by a mutex (insert in `gs_effect_create`). Everything else
(parameters, loop state) is single-threaded: the caller owns the graphics context while using an effect.

## 4. Data formats

### 4.1 The language (informal grammar)

```
file       := ( ';' | struct | technique | sampler | '{' ... '}' (error) | declaration )*
declaration:= specifier* TYPE NAME ( '(' function-rest | param-rest )
specifier  := 'uniform' | 'const' | 'property'
param-rest := [ '[' NUM ']' ] [ '<' annotation* '>' ] [ '=' VALUE ] ';'
annotation := TYPE NAME [ '=' VALUE ] ';'
VALUE      := scalar | '{' scalar (',' scalar)* '}' | TYPE '(' scalar (',' scalar)* ')' | STRING     (by parameter type)
struct     := 'struct' NAME '{' ( TYPE NAME [ ':' SEMANTIC ] ';' )* '}' ';'
sampler    := 'sampler_state' NAME '{' ( STATE '=' token* ';' )* '}' ';'
function   := TYPE NAME '(' [ param (',' param)* ] ')' [ ':' SEMANTIC ] '{' body '}'
param      := [ 'in' | 'inout' | 'out' | 'uniform' ] TYPE NAME [ ':' SEMANTIC ]
technique  := 'technique' NAME '{' pass* '}'
pass       := 'pass' [ NAME ] '{' ( ('vertex_shader'|'vertex_program'|'pixel_shader'|'pixel_program') '=' ['compile'] token* ';' )* '}'
```
All words are case sensitive; types are not validated (any NAME). The parser is deliberately lenient and has
extensive error recovery (rules R30-R70). Comments and the whole preprocessor are S-011 (`#define`, `#ifdef`, `#include`, ...).

### 4.2 Stored default values

`default_val` bytes (hex, little endian; vector `02-params-defaults`):

| declaration | `default_val` |
|---|---|
| `float f = 1.5;` | `0000c03f` (4 bytes) |
| `float2 v = {1,2};` / `float2(1,2)` | `0000803f00000040` (8 bytes) |
| `float4x4 m = {1,0,0,0,...}` | 16 floats, 64 bytes, in source order |
| `int i = -7;` | `f9ffffffffffffff` (one 8-byte long) |
| `int2 j = {1,-2};` | `0100000000000000feffffffffffffff` (16 bytes) |
| `bool b = true;` | `0100000000000000` (one long) |
| `string s = "hello";` | `68656c6c6f00` (text + NUL) |
| `texture2d t = "a.png";` | text + NUL |
| no initialiser | empty |

The *runtime type* of a parameter is derived from its declared type name by `get_effect_param_type`: exact names
`float float2 float3 float4 int2 int3 int4 float4x4 bool int string` map to FLOAT VEC2 VEC3 VEC4 INT2 INT3 INT4 MATRIX4X4 BOOL INT
STRING; any name starting with `texture` maps to TEXTURE; **everything else (including `float3x3`, `float2x2`, `half`, `double`,
`float1`, `sampler`) is UNKNOWN**, yet such parameters are still parsed with defaults and kept.

### 4.3 Assembled shader text - worked example

Source (`30-assembly-basic`, label `basic.effect`):

```
uniform float4x4 ViewProj;  uniform texture2d image;
sampler_state samp { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VIn  { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VOut VS(VIn v) { VOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float4 PS(VOut v) : TARGET { return image.Sample(samp, v.uv); }
technique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }
```

The backend receives for `(Vertex shader, technique Draw, pass 0)` the 294-byte text (`\n` = LF, `\t` = TAB):

```
uniform float4x4 ViewProj;\n\n\nstruct VOut {\n\tfloat4 pos : POSITION;\n\tfloat2 uv : TEXCOORD0;\n};\n\n
struct VIn {\n\tfloat4 pos : POSITION;\n\tfloat2 uv : TEXCOORD0;\n};\n\nVOut VS(VIn v)\n{ VOut o; ... return o; }\n\n
VOut main(VIn v)\n{\n\treturn VS(v);\n}\n
```
(no line breaks inside the real text), with location string `basic.effect (Vertex shader, technique Draw, pass 0)`.
Note the struct order (return type `VOut` before parameter type `VIn`), the two blank lines after the parameter, the
function header regenerated on its own line, the body copied verbatim starting at `{`, and the wrapper `main`. The
pixel shader text (281 bytes) contains the parameter `image`, the sampler block, only `VOut` (not `VIn`), `PS`, `main : TARGET`.
Vector `30-assembly-basic` holds both exact texts; the FNV-1a hash of every shader text is recorded for the corpus.

### 4.4 Error message catalogue

Every message is recorded as `<file> (<row>, <col>): <text>` (S-011-R12/R71). ERROR unless marked W (warning). `X` stands for the quoted word.

| Text | Raised by |
|---|---|
| `Unexpected EOF` | every `NEXTV` / `PEEKV` / `GOTOV` that runs into the end; `list` default (R18) |
| `Expected 'X'` with X = `type`, `name`, `type name`, `variable name`, `mapping name`, `mapping specifier` (`mapping` in the shader parser), `state name`, `numeric value`, `texture path string`, `string`, `true or false`, `assignment value`, `= or ;`, `; or >`, `>`, `<`, `,`, `;`, `=`, `]`, `{`, `}`, `(`, `)` | R10 (type/name), R40, R50, R61, R14, R15-R20, R30 |
| `Syntax error` | struct member without a name, function parameter list without `(`/`,`, double modifier, unknown pass command, annotation item recovery |
| `Invalid row count`, `Invalid column count` | R18 |
| `Invalid type 'T' used for assignment` | R16 |
| `'kw' keyword already specified` (W) | R11 |
| `'kw' keyword cannot be used with a function` | R12 |
| `Unexpected code segment` | R02 (top-level `{`) |
| `compile keyword not necessary` (W) | R61 |
| `Error creating shader: TEXT` | R71 |
| Log lines (not in the error list): `Pass (N) <NAME> missing vertex shader!` / `... pixel shader!`, `Effect shader parameter not found` | R71 |

## 5. Behaviour (numbered rules)

### 5.1 Entry and result of an effect parse

- **S-020-R01** `ep_parse(text, file)`: if the backend's preprocessor name `N` is non-NULL, an object-like macro named `N` with
  an **empty** token list is predefined (so `#ifdef N` is true; using `N` as an identifier crashes the reference - D-2). Then
  `cf_parser_parse(text, file)` (S-011-R70); when it fails (empty text, unterminated block comment) the parse returns false
  **without any error item** (`gs_effect_create` then returns NULL and `*error_string` is NULL).
- **S-020-R02** Top-level loop, while `cur` is not NONE: if the token is `;` or its first byte is whitespace (blank
  token): `cur += 1` (raw step); `struct` -> R40; `technique` -> R60; `sampler_state` -> R50; `{` -> error `Unexpected code segment`
  at that token, then `PASSPAIR` (skips the whole balanced block; the declaration inside is lost); anything else -> declaration (R10).
  There are no other keywords: `STRUCT`, `Technique` are ordinary words and fall into the declaration rule.
- **S-020-R03** After the loop: `success = no ERROR-level item exists` (warnings never fail). If `success`, the compile step (section 5.5) runs
  and its result is the result. **A parse with errors never creates any shader.**
- **S-020-R04** `gs_effect_create(text, filename, error_string)`: NULL text -> NULL. Otherwise parse; on failure: `*error_string`
  (when the pointer argument is non-NULL) = `error_data_buildstring` of the error list (S-011-R12: one line
  `<file> (<row>, <col>): <message>\n` per item including warnings; NULL when the list is empty) and the half-built effect is destroyed (its
  shaders are destroyed, backend `destroy` calls in creation order); result NULL. On success the effect is returned, and **if `filename`
  is non-NULL** it is inserted at the head of the context's effect list ("cached"). The `error_string` of a successful creation is
  never written.
- **S-020-R05** `gs_effect_create_from_file(path, err)`: returns the cached effect whose stored file name equals `path` (first match from the
  most recently created) without parsing or any backend call; else reads the file (failure: log ERROR `Could not load effect file '<path>'`, NULL), creates it
  with `filename = path`, returns it.
- **S-020-R06** `gs_effect_destroy(e)`: NULL or cached effects are untouched (cached effects are destroyed when the graphics context is
  destroyed, with their shaders, front to back of the list = newest first); uncached effects are freed with their shaders.

### 5.2 Declarations and parameters

- **S-020-R10** *Declaration* (algorithm 6.2): consume specifiers (R11), then `GET_NAME(type, 'type', ';')`, `NEXT_NAME(name, 'name', ';')`, `NEXTV()`
  (any failure aborts the declaration, freeing type and name). If `cur` is `(`: report R12 and parse a function (R30). Otherwise parse a parameter (R13).
- **S-020-R11** Specifiers `property`, `const`, `uniform` may appear in any order and any number of times. Each occurrence is consumed with
  `NEXTV()` (EOF aborts the declaration); a keyword seen before sets no new state but adds the WARNING `'<kw>' keyword already specified`
  **positioned at the token after the repeated keyword**. The flags are `is_property`, `is_const`, `is_uniform`.
- **S-020-R12** For a function, each of `property`, `const`, `uniform` that was given adds the ERROR `'<kw>' keyword cannot be used with a function`
  (order property, const, uniform; position: the `(` token). The function is still parsed and stored.
- **S-020-R13** *Parameter*: a record with the type text, name, the three flags, `is_texture = type starts with "texture"` (7 letters, case-sensitive),
  array count 0, no annotations, empty default. Then, in this order and each conditional on the current token: `;` -> done; `[` -> array (R14);
  `<` -> annotations (R15); `=` -> default (R16); finally the token MUST be `;`. Any failure drops the declaration **silently unless the failing sub-step
  reported an error**; the tokens after the failure point are then parsed as new top-level declarations (cascades are normal, see `06-params-errors`).
  Success appends the parameter in declaration order. Duplicate names are accepted.
- **S-020-R14** *Array* `[N]`: `NEXTV()` (failure: `Unexpected EOF`, declaration dropped); the token must be a NUM token accepted by `valid_int_str` (S-011-R10, applied to the token text with its length) - otherwise the declaration is dropped
  **without a message**; `array_count = strtol(token, 10)`; then `NEXT_SHOULD_BE(']', ';', NULL)` - only an `EOF` result fails (a missing `]` is reported with `Expected ']'` but parsing continues after skipping to `;`);
  then `NEXTV()`. An array count of 0 is indistinguishable from "not an array" (Q-5).
- **S-020-R15** *Annotations* (algorithm 6.3): require `<` (else `Expected '<'` and failure), then items `TYPE NAME [= VALUE] ;` until `>`. The item grammar and its
  error mapping are exactly algorithm 6.3; an item is a parameter record whose `is_texture` flag is **false regardless of its type** (Q-4: texture
  defaults are rejected in annotations). After the closing `>` the cursor moves to the next token (`NEXTV()`; failure drops the declaration). Annotations are kept in
  order; names may repeat.
- **S-020-R16** *Default value* (algorithm 6.4). Selection by the parameter's type text: texture flag -> R19; type `int` -> scalar long (R17); type `float` -> scalar float (R17);
  starts with `int` -> int vector/matrix (R18); starts with `float` -> float vector/matrix (R18); starts with `string` -> R19; `bool` -> R20; else the ERROR
  `Invalid type '<type>' used for assignment` (position: the `=` token) and failure. After a successful value the cursor moves with `NEXTV()`; failure drops the declaration.
- **S-020-R17** *Scalar* `= NUM` / `= - NUM`: `NEXTV()`; a `-` token sets the sign and `NEXT()` moves on (EOF without message); the token must be NUM (`Expected 'numeric value'`, skip to `;`).
  float: `(float) os_strtod(text)` negated if signed; os_strtod = C `strtod` (C locale) of at most 63 bytes starting at the token's first byte and **running on into the following source text** (so `2E+4` is read as 20000 although the lexer
  token is `2E`; the declaration is dropped anyway because the next token is `+`, so the over-read is unobservable); it also accepts hexadecimal floats (`0x10` = 16, `0x1p3` = 8, Q-2). int: `strtol(token, base 10)`, signed negation, stored as a `long` (so `0x10` = 0, `007` = 7, `1.5` = 1: Q-1). A leading `+` is not accepted;
  `1e-3` is split by the lexer into `1e` `-` `3` and drops the declaration (Q-3).
- **S-020-R18** *Vectors and matrices* for types starting with `float` / `int` (suffix after the prefix: `S`): `S[0]` must be `1`-`4` else the ERROR `Invalid row count`
  (position: the `=` token; parsing continues); `n = S[0] - '0'` (computed anyway); if `S[1] == 'x'`: `S[2]` must be `1`-`4` else `Invalid column count`, `n *= S[2] - '0'`. Then `NEXT()` (EOF: `Unexpected EOF`, fail); the token
  is `{` (closing `}`), or equal to the **declared type text** (constructor form; closing `)`, then `NEXT_SHOULD_BE('(', ';', NULL)` must succeed); anything else: `GOTO(';')` (silent failure if none) and the ERROR `Expected '{'`
  (CONTINUE). Then for `i` in `0..n`: one scalar (R17), then `NEXT_SHOULD_BE(i+1<n ? ',' : closer, ';', NULL)`. `n <= 0` (possible after the errors above) parses no values. The number of components therefore equals
  rows x columns, independent of the actual type name (`float3x3` stores 9 floats but is runtime type UNKNOWN; `int` vectors store `long`s).
- **S-020-R19** *String and texture defaults*: `NEXTV()`, the token must be a STRING (`Expected 'texture path string'` / `Expected 'string'`, skip to `;`); `cf_literal_to_str` (S-011-R60) of the token text; if it
  returns a string, `default_val` = its bytes + NUL (if NULL: stays empty, parse succeeds). The stored text inherits S-011 Q-10 (escape sequences read past the closing quote: Q-4).
- **S-020-R20** *Bool*: `NEXTV()`; `true` stores long 1, `false` long 0; otherwise the ERROR `Expected 'true or false'` and **EOF** (no skip: a cascade follows).

### 5.3 Structs and samplers

- **S-020-R40** *Struct* (algorithm 6.5). Members are `TYPE NAME [: SEMANTIC] ;`. The `;` after `}` is mandatory (without it the struct is dropped with `Expected ';'`). Members that do not fit
  (arrays, initialisers, qualifiers, extra semantics) are skipped up to `;`/`}` and the **member is dropped without a message** except for the errors listed in 6.5 (`Syntax error` for a missing type/name,
  `Expected 'type name'`/`'variable name'`/`'mapping name'`). Empty structs and duplicate struct names are accepted. A struct is "mapped" iff it has a first member and that member has a semantic.
- **S-020-R50** *Sampler* (algorithm 6.6): `sampler_state NAME { STATE = tokens... ; ... } ;`. The value is the **concatenation of the text of every non-blank token** between `=` and `;`
  (`Filter = a b;` -> `ab`); a state whose value is empty is dropped; duplicates are kept in order. Both `}` and the final `;` are mandatory. Names may repeat.

### 5.4 Functions, techniques, passes

- **S-020-R30** *Function* (algorithm 6.7). Parameters: optional single modifier `in|inout|out|uniform` (stored as the var type; `in` = default), `TYPE NAME [: SEMANTIC]`.
  Optional return semantic `: SEMANTIC` (a non-NAME after `:` reports `Expected 'mapping specifier'` and continues at `{`). The body must start with `{`
  (`Expected '{'` otherwise) and is captured verbatim: the text of **every token** (blanks included) from `{` to the matching `}` inclusive, concatenated. Braces nest by counting `{` / `}` tokens (strings are single tokens).
  Functions are stored in declaration order; duplicate names allowed.
- **S-020-R31** *Dependencies*: while the function is parsed, three string lists are built (duplicates kept, order of appearance):
  `struct_deps` (the return type if it names an already declared struct; each parameter type that names an already declared struct, else a sampler name used as a parameter type -> `sampler_deps`;
  every body token that names a declared struct), `func_deps` (body tokens naming an already declared function), `sampler_deps`, `param_deps` (body tokens naming an already declared global parameter).
  For each body token (after the first `{`) the test order is struct, function, sampler, parameter, first match wins. Only names declared **earlier in the file** are found
  (declaring a function, struct or parameter after its use hides it from the dependency scan; `31-assembly-deps` e8). Lookups find the first declaration of a name. A parameter that is shadowed by a local variable or parameter
  of the same name still counts. Struct *members* of struct type are not dependencies.
- **S-020-R60** *Technique* (algorithm 6.8): `technique NAME { pass* }`. Items that do not start with `pass` are skipped **silently** up to the closing `}` (nested blocks are skipped as units), so a
  technique with junk keeps only the passes before it. A technique without `{` skips to `;` (`Expected ';'` when none, otherwise `Expected '{'` and the technique is dropped).
  Techniques may be empty and may repeat names.
- **S-020-R61** *Pass* (algorithm 6.8): `pass [NAME] {` - the single token after `pass` is the name unless it is `{`; **the token after the name is not verified to be `{`** (it is skipped). Commands are
  `vertex_shader|vertex_program = [compile] CALL ;` and `pixel_shader|pixel_program = ...`. An unknown command word adds `Syntax error` and skips to `;`/`}`; a missing `=`
  adds `Expected '='`; a `}` before the `;` of a call adds `Expected ';'`. `compile` adds the WARNING `compile keyword not necessary` (position: the token after `compile`). The call is the list of all non-blank tokens up to and including the `;`
  followed by an end marker; a second assignment to the same stage is appended after the marker and therefore **ignored** (first assignment wins).
  A pass without a command for a stage has an empty call list for that stage.

### 5.5 Compile: parameters, passes and shader text

- **S-020-R70** `ep_compile` builds the effect only when R03 allows. Parameters first, in order: `name`, `type = get_effect_param_type(type text)` (section 4.2), `default_val` = the parsed bytes (moved), annotations likewise
  (name, runtime type, default). The effect's `view_proj` / `world` pointers are set to the parameter named exactly `ViewProj` / `World` (the **last** such declaration wins; any type). Then techniques in order, each pass in order: pass `name` copy (NULL stays NULL),
  vertex shader, then pixel shader (R71). **Every pass of every technique is processed even after a failure**; the result is the conjunction.
- **S-020-R71** *One stage* (`compile_pass_shader`): location = `<file>` + `" (Vertex "` or `" (Pixel "` + `"shader, technique <tname>, pass <index>)"` where `<file>` is the file name given to the parse (empty when NULL),
  `<index>` the 0-based pass index (decimal, unsigned). The text is built by R72-R75. The backend creation function is called with the text (a NULL string when nothing was written: the graphics layer rejects it with a DEBUG log
  and returns NULL), the location, and an out error text. If the returned error text is non-NULL and non-empty the ERROR `Error creating shader: <text>` is added to the parser's error list **at the parser's current cursor**
  (after a full parse: the end-of-input token; Q-C) - this does **not** change the success flag. If the backend returned a shader, for each parameter name collected while writing (R72, uniform parameters only, in
  write order) the effect parameter (`gs_effect_get_param_by_name`, first match) and the shader parameter (`gs_shader_get_param_by_name`) are paired and stored for the stage; if a shader parameter is not found: log ERROR
  `Effect shader parameter not found` and the stage fails (the remaining names are not looked up). No shader: the stage fails. A failed stage logs ERROR `Pass (<index>) <<passname or empty>> missing vertex shader!` /
  `... missing pixel shader!`.
- **S-020-R72** *Shader text* (`makeshaderstring`, algorithm 6.9). For the stage call list `C`: nothing (empty text) if `C` has no tokens or its first non-blank token is NONE or the word `NULL`. The first token names the entry
  function. All tokens of `C` up to the end marker are scanned in order: **every token whose text equals the name of a declared global parameter** (first match) is written (R73) *before anything else*; every token text is appended to the call
  string (no separators). If no function has the first token's name the text consists only of the parameters written so far (possibly empty) and the "written" marks are **not** reset (Q-B). Otherwise the function is written (R74), then
  the wrapper (R75), then all "written" marks (parameters, structs, functions, samplers) are cleared.
- **S-020-R73** *Parameter text*: nothing if already written in this text; else `const ` if the parameter is const, otherwise `uniform ` (and the name is appended to the pass's lookup list) if uniform; then `TYPE NAME`, then `[N]` when the array count is non-zero (decimal), then `;\n`.
  `property` is never written; defaults and annotations are never written; a const parameter loses its value (`const float c;`).
- **S-020-R74** *Function text* (recursive): mark written; (a) every name of `param_deps` in list order -> parameter text (R73), and if the list is non-empty then `\n\n`; (b) every name of `sampler_deps` in list order: the sampler block (if not
  yet written) and **always** one `\n` (so each repeated mention adds a blank line, Q-G); (c) every name of `struct_deps`: if not written, the struct block followed by `\n`; (d) every name of `func_deps` whose function is not yet written: its text (recursively), then `\n\n`; (e) the
  header `RET NAME(` + parameters joined by `, ` (each: modifier word `inout `/`out `/`uniform ` unless `in`, `TYPE NAME`, ` : SEMANTIC` if present) + `)\n` + the captured body + `\n`.
  Struct block: `struct NAME {` then per member `\n\t` + member text (as for parameters, modifier none) + `;` then `\n};\n`. Sampler block: `sampler_state NAME {` then per state `\n\t` + `STATE = VALUE;\n`, then `\n};\n`.
  The function mapping (return semantic) is **not** part of the function text.
- **S-020-R75** *Wrapper*: `\n` + `RET main(` + the **mapped** parameters of the entry function, each as `TYPE NAME` plus ` : SEMANTIC` when the parameter itself has the semantic, **concatenated without any separator** (Q-A) + `)` + ` : SEMANTIC`
  (the function's return semantic, if any) + `\n{\n\treturn ` + call string + `\n}\n`. A parameter is mapped if it has its own semantic, or its type names a declared struct (first match) whose first member has a semantic.
  The modifier words are never written in `main`.

### 5.6 Effect runtime

- **S-020-R80** Lookups: `get_param_by_idx(e, i)` NULL for NULL effect or `i >= num`; `get_param_by_name` first exact match (NULL name: DEFECT D-6);
  `get_num_params`; annotations: `get_num_annotations`, `get_annotation_by_idx(p, i)` is NULL for NULL `p` or `i > num` (**`i == num` returns a pointer one past the end**, D-7), `by_name` first exact match;
  `get_technique(e, name)` first exact match (NULL name: D-3); `get_current_technique`; `get_pass_by_idx(t, i)` NULL for NULL `t` or `i > passes` (D-7: `i == passes` is a past-the-end pointer); `get_pass_by_name` first exact match
  (compares with `strcmp` against pass names, an unnamed pass before a match crashes: D-4); `get_viewproj_matrix`, `get_world_matrix`; `get_param_info(p, &info)` stores name and runtime type (NULL `p`: untouched).
- **S-020-R81** Value setters all funnel into one routine `setval(param, bytes)`: NULL param -> log ERROR `effect_setval_inline: invalid param`, nothing; NULL data -> log ERROR `effect_setval_inline: invalid data`, nothing. Otherwise
  `size_changed = (cur_val.size != n)`; if size changed the buffer is resized to `n`; if size changed **or** the bytes differ: copy and set `changed = true`. Wrappers: `set_bool` stores `(int)b` (4 bytes), `set_float` 4,
  `set_int` 4, `set_vec2` 8, `set_vec3` **12** (x,y,z), `set_vec4` 16, `set_matrix4` 64, `set_color(argb)` = the 16-byte vec4 produced by `vec4_from_bgra` (S-008: components `(R,G,B,A)/255` from `0xAARRGGBB`),
  `set_texture(p, tex)` / `set_texture_srgb` store a `gs_shader_texture {tex, srgb}` (16 bytes; padding bytes are unspecified - port: store the typed pair), `set_val(p, data, n)` any `n` (no type or size check).
- **S-020-R82** `get_val(p)`: NULL p -> log ERROR `gs_effect_get_val: invalid param`, NULL; empty `cur_val` -> NULL; else a fresh copy. `get_val_size` the size (0 for NULL). `get_default_val` / `get_default_val_size`: same with `default_val`
  (ERROR text `gs_effect_get_default_val: invalid param`). `set_default(p)` = `setval(p, default bytes)` (so empty defaults log `invalid data`; NULL `p` crashes: D-5).
- **S-020-R83** *Technique protocol.* `gs_technique_begin(t)`: NULL -> 0; makes `t` the effect's current technique and the effect the active effect of the graphics context; returns the pass count.
  `gs_technique_end(t)`: NULL no-op; loads NULL vertex and pixel shaders into the backend, clears the current technique and active effect, and for **every effect parameter**: `cur_val` emptied, `changed = false`,
  `next_sampler` cleared. (The current pass is not cleared.)
- **S-020-R84** `gs_technique_begin_pass(t, i)`: false if `t` is NULL or `i >= passes`; else the pass becomes current, its vertex and pixel shaders are loaded (backend load calls: vertex, pixel), and **all** parameters of both stages are uploaded (R86 with `changed_only = false`); true.
  `begin_pass_by_name(t, name)`: false for NULL `t`; the first pass whose name equals `name` is begun (via `begin_pass`) and true is returned; false when none matches.
- **S-020-R85** `gs_technique_end_pass(t)`: NULL `t` or no current pass: no-op. Otherwise for each of the pass's parameters of both stages (vertex then pixel): if the **shader** parameter's type is TEXTURE, `shader_set_texture(sparam, NULL)`; then no current pass.
  `gs_effect_update_params(e)`: NULL no-op; with a current pass: upload with `changed_only = true`; no current pass: nothing.
- **S-020-R86** *Upload* of a stage's pair list, in list order, for each pair `(ep, sp)`: if `ep.next_sampler` is set: `shader_set_next_sampler(sp, sampler)` (**every time**, even if unchanged and even when `changed_only`); then if `changed_only` and not `ep.changed`: next pair;
  if `ep.cur_val` is empty: if `ep.default_val` is non-empty copy it into `cur_val` (and continue) else skip the pair; finally `shader_set_val(sp, cur_val bytes)`. After both stages: `changed` is cleared on every pair's effect parameter (vertex list, then pixel list).
  Consequences: a default is uploaded once per pass begin and becomes the current value until the technique ends; a parameter that is shared by both stages is uploaded twice.
- **S-020-R87** `gs_effect_set_next_sampler(p, s)`: NULL `p` logs ERROR `gs_effect_set_next_sampler: invalid param`; only parameters of runtime type TEXTURE store the sampler (otherwise ignored).
- **S-020-R88** `gs_effect_loop(e, name)` (algorithm 6.10): NULL `e` -> false. When not looping: if **any** effect is active in the graphics context (`gs_get_effect() != NULL`) log WARNING `gs_effect_loop: An effect is already active`, false; unknown technique log WARNING
  `gs_effect_loop: Technique '<name>' not found.`, false; else `gs_technique_begin(tech)` and `looping = true`. When looping: `gs_technique_end_pass(current technique)` and `name` is ignored.
  Then `begin_pass(current technique, loop_pass)` with `loop_pass` incremented after use; if that fails: `gs_technique_end`, `looping = false`, `loop_pass = 0`, false. True otherwise.

### 5.7 Shader parser (backend side)

- **S-020-R90** `shader_parse(sp, text, file)`: `cf_parser_parse`; false if that fails. Loop like R02 but with only `struct`, `sampler_state`, `{` (same `Unexpected code segment` + skip) and declarations (**no `technique`**, no `property` specifier). Result:
  true iff there is no ERROR item. Results are tables of params (global variables, in order), structs, samplers and functions.
- **S-020-R91** *Global variables*: specifiers `const` / `uniform` only (`property` is parsed as a type name, leading to a different error); `shader_var.var_type` = UNIFORM if uniform, else CONST if const, else IN (uniform wins). Tail: `;`, or `[N]` (R14), or `= VALUE` (no annotations).
  VALUE: type `int` -> long scalar, type `float` -> float scalar, type starting with `float` -> **brace form only** (`{a, b, ...}`, same row/column rules and errors as R18, the constructor form and `NEXT()`-based detection do not exist; a missing `{` is `Expected '{'`);
  every other type (`bool`, `int2`, `string`, textures, ...) gives `Invalid type '<t>' used for assignment`. Then `;` required, else silent drop.
- **S-020-R92** *Struct* as R40 (members as `shader_var`). *Sampler*: `sampler_state NAME { STATE = VALUE ; ... } ;` where VALUE is **exactly one token** (`cf_next_token_copy`) and the next token must be `;` (`NEXT_SHOULD_BE(';', ';', NULL)`); unlike the effect parser nothing is concatenated,
  empty values are an error, and a sampler item that fails is skipped by the same recovery as R50. Both `}` and `;` mandatory.
- **S-020-R93** *Function* as R30 except: no dependency tracking, no body text; `start` = the `{` token, then `PASSPAIR`; on success `cf_next_token` and `end` = the token reached (the first non-blank token after the closing `}`), body = tokens `[start, end)`. An unterminated body drops the function
  **silently** (no error, no crash). A `:` mapping that is not a NAME drops the function with the message `Expected 'mapping'` (the effect parser says `Expected 'mapping specifier'` and recovers; this one does not). Specifier errors as R12 (without `property`).
- **S-020-R94** `shader_sampler_convert(ss, info)`: zero-initialise `info` (filter POINT, all addresses CLAMP, border 0), `max_anisotropy = 1`; then for each state in order (state names compared case-insensitively): `Filter` -> `get_sample_filter(value)`; `AddressU/V/W` -> `get_address_mode(value)`;
  `MaxAnisotropy` -> `(int) strtol(value, 10)`; `BorderColor` -> `strtol(value + 1, NULL, 16)` stored in a `uint32` (**the first character is skipped**, Q-F). Unknown states are ignored; later states override earlier ones.
- **S-020-R95** `get_shader_param_type(name)`: exact names `float float2 float3 float4 int2 int3 int4 float4x4 bool int string` -> FLOAT VEC2 VEC3 VEC4 INT2 INT3 INT4 MATRIX4X4 BOOL INT STRING; names beginning with `texture` -> TEXTURE; otherwise UNKNOWN (the effect parser uses an identical private copy).
- **S-020-R96** `get_sample_filter(text)`: `Anisotropy` (case-insensitive) -> ANISOTROPIC; `Point` (case-insensitive) or exactly `MIN_MAG_MIP_POINT` -> POINT; `Linear` (case-insensitive) or exactly `MIN_MAG_MIP_LINEAR` -> LINEAR; exactly (case-sensitive) `MIN_MAG_POINT_MIP_LINEAR`,
  `MIN_POINT_MAG_LINEAR_MIP_POINT`, `MIN_POINT_MAG_MIP_LINEAR`, `MIN_LINEAR_MAG_MIP_POINT`, `MIN_LINEAR_MAG_POINT_MIP_LINEAR`, `MIN_MAG_LINEAR_MIP_POINT` -> the same-named enum; anything else -> LINEAR.
- **S-020-R97** `get_address_mode(text)` (case-insensitive): `Wrap`|`Repeat` -> WRAP, `Clamp`|`None` -> CLAMP, `Mirror` -> MIRROR, `Border` -> BORDER, `MirrorOnce` -> MIRRORONCE, else CLAMP.

## 6. Algorithms

### 6.1 Notation

Procedures return codes (section 2). `E(s)` = add ERROR `s` at `cur`. "discard X" = drop the half-built record. Helper calls as in section 2.
`type`/`name` strings are copies of token text. `tokentext()` is the merged text of `cur`.

### 6.2 Declaration (R10-R13)

```
declaration():
    prop = const = uni = false
    loop:
        for kw, flag in [("property", prop), ("const", const), ("uniform", uni)]:
            if IS(kw):
                if not NEXTV(): return                      # message already added
                if flag: warning "'kw' keyword already specified"      # at the NEW cur
                flag = true; continue outer loop
        break
    if GET_NAME(type, "type", ";") != SUCCESS: return
    if NEXT_NAME(name, "name", ";") != SUCCESS: return
    if not NEXTV(): return
    if IS("("):
        for kw in [property, const, uniform] if given: E("'kw' keyword cannot be used with a function")
        function(type, name)
    else:
        parameter(type, name, flags)
```

### 6.3 Annotations (R15)

```
annotations(P):
    if not IS("<"): expecting("<"); return false
    loop:
        v = new annotation (type "", name "", flags false, is_texture false)
        r = annotation_item(v)
        if r is UNEXPECTED_CONTINUE: syntax(); r = CONTINUE
        if r is UNEXPECTED_BREAK:    syntax(); r = BREAK
        if r == CONTINUE: discard v; continue
        if r == BREAK:    discard v; break
        if r == EOF:      discard v; return false
        P.annotations.push(v)
    if not IS(">"): expecting(">"); return false          # cannot happen after BREAK, kept for completeness
    if not NEXTV(): return false
    return true

annotation_item(v):
    if not NEXTV(): return EOF
    if IS(";"): return CONTINUE
    if IS(">"): return BREAK
    c = IS_TYPE(NAME, "type name", ";");      if c != SUCCESS: return c
    v.type = tokentext()
    if not NEXTV(): return EOF
    if IS(";"): expecting("variable name"); return UNEXPECTED_CONTINUE
    if IS(">"): expecting("variable name"); return UNEXPECTED_BREAK
    c = IS_TYPE(NAME, "variable name", ";");  if c != SUCCESS: return c
    v.name = tokentext()
    if not NEXTV(): return EOF
    if IS(":") or IS(">"): expecting("= or ;"); return UNEXPECTED_BREAK
    if IS("="):
        if not default_value(v): expecting("assignment value"); return UNEXPECTED_BREAK
    if not IS(";"):
        if not GOTOV(";", ">"): expecting("; or >"); return EOF
        return CONTINUE
    return SUCCESS
```

### 6.4 Default value (R16-R20)

```
default_value(P):                      # cur is on "="
    if assignment(P) != SUCCESS: return false
    return NEXTV()
assignment(P):
    t = P.type
    if P.is_texture:                return literal(P, "texture path string")
    if t == "int":                  return scalar(P, false)
    if t == "float":                return scalar(P, true)
    if t starts with "int":         return list(P, false)
    if t starts with "float":       return list(P, true)
    if t starts with "string":      return literal(P, "string")
    if t == "bool":                 return boolean(P)
    E("Invalid type 't' used for assignment"); return CONTINUE
scalar(P, is_float):
    if not NEXTV(): return EOF
    neg = false
    if IS("-"): neg = true; if not NEXT(): return EOF
    c = IS_TYPE(NUM, "numeric value", ";"); if c != SUCCESS: return c
    append (float: 4-byte strtod prefix of the token text, negated if neg; int: 8-byte strtol, negated if neg) to P.default
    return SUCCESS
list(P, is_float):
    S = P.type[(5 if is_float else 3):]
    n = 0
    if S[0] not in '1'..'4': E("Invalid row count")
    n = S[0] - '0'                                  # integer arithmetic, may be negative/large
    if S[1] == 'x':
        if S[2] not in '1'..'4': E("Invalid column count")
        n = n * (S[2] - '0')
    if not NEXT(): eof(); return EOF
    if IS("{"):          closer = "}"
    elif IS(P.type):     closer = ")"; c = NEXT_SHOULD_BE("(", ";", none); if c != SUCCESS: return c
    else:
        if not GOTO(";", none): return EOF
        expecting("{"); return CONTINUE
    for i in 0 .. n-1:
        c = scalar(P, is_float); if c != SUCCESS: return c
        c = NEXT_SHOULD_BE(i+1 < n ? "," : closer, ";", none); if c != SUCCESS: return c
    return SUCCESS
literal(P, what):
    if not NEXTV(): return EOF
    c = IS_TYPE(STRING, what, ";"); if c != SUCCESS: return c
    s = cf_literal_to_str(token); if s: P.default = s + NUL
    return SUCCESS
boolean(P):
    if not NEXTV(): return EOF
    if IS("true"):  append long 1; return SUCCESS
    if IS("false"): append long 0; return SUCCESS
    expecting("true or false"); return EOF
```
The shader parser (R91) uses `scalar` and a `list` restricted to `float...` types with only the `{` form (no `NEXT()` before the check: it is `NEXT_SHOULD_BE("{", ";", none)`), with closer `}`.

### 6.5 Struct (R40)

```
struct():
    S = new
    if NEXT_NAME(S.name, "name", ";") != SUCCESS: discard; return
    if NEXT_SHOULD_BE("{", ";", none) != SUCCESS: discard; return
    loop:
        v = new member
        r = member(v)
        if r == UNEXPECTED_CONTINUE: syntax(); r = CONTINUE
        if r == UNEXPECTED_BREAK:    syntax(); r = BREAK
        if r == CONTINUE: discard v; continue
        if r == BREAK:    discard v; break
        if r == EOF:      discard v; discard S; return
        S.members.push(v)
    if NEXT_SHOULD_BE(";", none, none) != SUCCESS: discard S; return
    push S
member(v):
    if not NEXTV(): return EOF
    if IS(";"): return CONTINUE
    if IS("}"): return BREAK
    c = IS_TYPE(NAME, "type name", ";");        if c != SUCCESS: return c
    v.type = text
    if not NEXTV(): return EOF
    if IS(";"): return UNEXPECTED_CONTINUE
    if IS("}"): return UNEXPECTED_BREAK
    c = IS_TYPE(NAME, "variable name", ";");    if c != SUCCESS: return c
    v.name = text
    if not NEXTV(): return EOF
    if IS(":"):
        if not NEXTV(): return EOF
        if IS(";"): return UNEXPECTED_CONTINUE
        if IS("}"): return UNEXPECTED_BREAK
        c = IS_TYPE(NAME, "mapping name", ";"); if c != SUCCESS: return c
        v.mapping = text
        if not NEXTV(): return EOF
    if not IS(";"):
        if not GOTOV(";", "}"): return EOF
        return CONTINUE
    return SUCCESS
```

### 6.6 Sampler (R50)

```
sampler():
    S = new
    if NEXT_NAME(S.name, "name", ";") != SUCCESS: discard; return
    if NEXT_SHOULD_BE("{", ";", none) != SUCCESS: discard; return
    if not PEEKV(peek): discard; return
    while peek.text != "}":
        if sampler_item(S) == EOF: discard; return
        if not PEEKV(peek): discard; return
    if NEXT_SHOULD_BE("}", ";", none) != SUCCESS: discard; return
    if NEXT_SHOULD_BE(";", none, none) != SUCCESS: discard; return
    push S
sampler_item(S):
    r = NEXT_NAME(state, "state name", ";");           if r != SUCCESS: return r
    r = NEXT_SHOULD_BE("=", ";", none);                if r != SUCCESS: return r
    value = ""
    loop: if not NEXTV(): return EOF
          if first byte of cur text == ';': break
          value += text(cur)
    if value non-empty: S.push(state, value)
    return SUCCESS
```
(Shader parser variant, R92: `r = NEXT_NAME(...)`; `NEXT_SHOULD_BE("=", ";", none)`; `NEXT_TOKEN_COPY(value)` (NEXTV + copy); `NEXT_SHOULD_BE(";", ";", none)`; push both.)

### 6.7 Function (R30, R31)

```
function(type, name):
    F = new (ret_type = type, name = name)
    if type names a declared struct: F.struct_deps.push(type)
    if not params(F): discard; return
    if not NEXTV(): discard; return
    if IS(":"):
        c = NEXT_NAME(F.mapping, "mapping specifier", "{")
        if c == EOF: discard; return
        if c != CONTINUE: if not NEXTV(): discard; return     # CONTINUE leaves cur on "{"
    if not IS("{"): expecting("{"); discard; return
    if not body(F): discard; return        # D-1
    NEXT()                                  # step over the closing "}"
    push F
params(F):
    if not PEEKV(peek): return false
    if first byte of peek == ')': NEXT(); return true
    do:
        v = new
        if not IS("(") and not IS(","): syntax()
        c = param(F, v)
        if c != SUCCESS:
            discard v
            if c == CONTINUE: return true
            if c == EOF: return false
        F.params.push(v)
    while not IS(")")
    return true
param(F, v):
    if not NEXTV(): return EOF
    # at most ONE modifier is consumed: first match among in, inout, out, uniform (tested in that order)
    if IS("in"):      if not NEXTV(): return EOF; v.kind = IN
    elif IS("inout"): if not NEXTV(): return EOF; v.kind = INOUT
    elif IS("out"):   if not NEXTV(): return EOF; v.kind = OUT
    elif IS("uniform"): if not NEXTV(): return EOF; v.kind = UNIFORM
    c = GET_NAME(v.type, "type", ")");       if c != SUCCESS: return c
    c = NEXT_NAME(v.name, "name", ")");      if c != SUCCESS: return c
    if not NEXTV(): return EOF
    if IS(":"):
        c = NEXT_NAME(v.mapping, "mapping specifier", ")"); if c != SUCCESS: return c
        if not NEXTV(): return EOF
    if v.type names a declared struct: F.struct_deps.push(v.type)
    elif v.type names a declared sampler: F.sampler_deps.push(v.type)
    return SUCCESS
body(F):
    depth = 1
    F.contents += text(cur)                    # the "{"
    while depth > 0:
        old = cur; cur += 1                    # raw step (blanks are tokens too)
        if old.type == NONE: return false      # D-1: the reference has already stepped past NONE
        if cur.type is SPACETAB or NEWLINE: pass
        elif IS("{"): depth += 1
        elif IS("}"): depth -= 1
        elif cur names a declared struct:   F.struct_deps.push(name)
        elif cur names a declared function: F.func_deps.push(name)
        elif cur names a declared sampler:  F.sampler_deps.push(name)
        elif cur names a declared parameter:F.param_deps.push(name)
        F.contents += text(cur)
    return true
```
The tests "names a declared X" use the first record with exactly that token text among the records already stored; they run for *every* non-brace, non-blank token (including types, keywords, numbers - a number never names anything).

### 6.8 Technique and pass (R60, R61)

```
technique():
    T = new
    if NEXT_NAME(T.name, "name", ";") != SUCCESS: goto fail
    if not NEXTV(): return                                  # nothing else happens
    if not IS("{"):
        if not GOTO(";", none): expecting(";"); return      # no further step
        expecting("{"); goto fail
    if not NEXTV(): goto fail
    while not IS("}"):
        p = new pass
        r = pass(p)
        if r == UNEXPECTED_CONTINUE: discard p; if not GOTO("}", none): goto fail; continue
        if r == EOF: discard p; goto fail
        T.passes.push(p)
        if not NEXTV(): goto fail
    NEXT()
    push T; return
fail:
    NEXT(); discard T
pass(p):
    if not IS("pass"): return UNEXPECTED_CONTINUE
    if not NEXTV(): return EOF
    if not IS("{"):
        p.name = text(cur)
        if not NEXTV(): return EOF
    if not PEEKV(peek): return EOF
    while peek.text != "}":
        r = command(p)
        if r < 0 and r != CONTINUE: return r
        if not PEEKV(peek): return EOF
    NEXT()                                                # cur = "}"
    return SUCCESS
command(p):
    if not NEXTV(): return EOF
    if IS("vertex_shader") or IS("vertex_program"): call = p.vertex
    elif IS("pixel_shader") or IS("pixel_program"):  call = p.pixel
    else: syntax(); if not GOTOV(";", "}"): return EOF; return CONTINUE
    if NEXT_SHOULD_BE("=", ";", "}") != SUCCESS: return CONTINUE
    if not NEXTV(): return EOF
    if IS("compile"): warning "compile keyword not necessary"; if not NEXTV(): return EOF
    while not IS(";"):
        if IS("}"): expecting(";"); return CONTINUE
        call.push(cur)
        if not NEXTV(): return EOF
    call.push(cur); call.push(END)
    return SUCCESS
```
(`pass` returns `SUCCESS` also for the `CONTINUE` results of commands; a pass is stored even if its commands failed.)

### 6.9 Shader text (R72-R75)

```
shader_text(call, used):                          # returns text or "nothing"
    if call has no tokens: return nothing
    t = first token; skip leading tokens whose first byte is whitespace (none occur) ; if t is NONE or t.text == "NULL": return nothing
    out = ""; callstr = ""
    for tok in call (until END): 
        if exists param with name == tok.text (first): write_param(param)
        callstr += tok.text
    f = first function named call[0].text           # lookup uses the token that was found as "first non-blank"
    if f is none: return out                         # marks NOT reset
    write_func(f); write_main(f, callstr)
    clear all written marks
    return out
write_main(f, callstr):
    out += "\n" + f.ret + " main("
    for v in f.params:
        mapped = v.mapping != none; st = none
        if not mapped: st = first struct named v.type; if st: mapped = st.members non-empty and st.members[0].mapping != none
        if mapped:
            out += v.type + " " + v.name
            if st == none: out += " : " + v.mapping
    out += ")"
    if f.mapping: out += " : " + f.mapping
    out += "\n{\n\treturn " + callstr + "\n}\n"
```
`write_param`, `write_func` as R73/R74.

### 6.10 Effect loop (R88)

```
loop(e, name):
    if e == null: return false
    if not e.looping:
        if active_effect != null: warn "gs_effect_loop: An effect is already active"; return false
        t = technique(e, name); if t == null: warn "gs_effect_loop: Technique 'name' not found."; return false
        technique_begin(t); e.looping = true
    else:
        technique_end_pass(e.cur_technique)
    if not begin_pass(e.cur_technique, e.loop_pass): ; e.loop_pass += 1 happens before the test
        technique_end(e.cur_technique); e.looping = false; e.loop_pass = 0; return false
    return true
```
(`loop_pass` is incremented whether or not the pass exists, then reset to 0 on failure.)

## 7. Concurrency

Only the effect list mutex (R04, R06). The parser and runtime are single-threaded: concurrent use of one effect from several threads is undefined, as in the reference.

## 8. Reference defects and quirks

QUIRKs (SHOULD reproduce; vectors contain them):

- **Q-1** int defaults use base-10 `strtol`: `0x10` is 0, `007` is 7, `1.5` is 1 (`07-params-quirks`).
- **Q-2** float defaults use C `strtod`: hexadecimal floats (`0x10`, `0x1p3`) are accepted (`07`).
- **Q-3** `1e-3` / `2E+4` cannot be written: the lexer splits at the sign; the declaration is dropped and a follow-up `Expected 'type'` is reported (`07`).
- **Q-4** String / texture defaults inherit the S-011 Q-10 escape quirk; texture-typed annotations are rejected (`Invalid type 'texture2d' used for assignment`) because the annotation record's texture flag is computed from an empty type (`05`, `07`).
- **Q-5** `x[0]` is stored as a non-array (`07`, `04`).
- **Q-6** int/bool defaults are C `long` (8 bytes here; 4 on Windows) - byte length and hex of `default_val` are platform-dependent (`02`, `07`). The setters store **4 bytes** (`set_int`, `set_bool`), so the same parameter reaches the backend
  as a 4-byte value when it was set and as an 8-byte (LP64) value when it was defaulted (`61-runtime-values`, `62-runtime-loop`: `count`/`flag` uploads are 8 bytes). Backends must accept both sizes.
- **Q-A** the wrapper `main` concatenates multiple mapped parameters without separators (`main(V vfloat2 uv : TEXCOORD0)`), producing text the shader parser rejects (`80-quirks`).
- **Q-B** the "written" marks are cleared only after a stage whose entry function exists (`80-quirks`).
- **Q-C** `Error creating shader` items are positioned at the end of the input and carry the backend text including its trailing newline (`43-backend`, `80-quirks`); they never change the creation result.
- **Q-D** only uniform parameters are looked up in the created shader (`80-quirks`).
- **Q-E / Q-F** `BorderColor = #RRGGBB` fails through the shader parser; the first character of the value is skipped by `shader_sampler_convert` (`71`, `80-quirks`).
- **Q-G** every mention of a sampler adds a blank line to the shader text (`31-assembly-deps` e4b).
- **Q-H** a technique that contains something other than passes silently loses everything after it; a pass name token is never validated; a second assignment to the same stage is ignored (`40`, `41`, `34`).
- **Q-I** Errors in a declaration frequently produce *no* message (dropped struct members, `float x[2;`, `float x = 1 2`); parsing resumes mid-statement.
- **Q-J** an annotation or a parameter with an array/annotation/default in the "wrong" order is dropped; the order is fixed `[N]` `<...>` `= value` (`04`, `05`).
- **Q-K** Shader texture/sampler `info` of a sampler without `Filter` is POINT (zero-init), not LINEAR (`71`).

DEFECTs (required behaviour of a conforming implementation):

- **D-1** (out-of-bounds read, crash) A function body that is not closed before the end of the input (`float y() {`, `... { return 1;`): after hitting the NONE token the reference steps past it and the top-level loop reads garbage memory
  (`ignore ep d1a...` in `90-defects`; with the reference's heap behaviour it crashes in ~9 % of mutated corpus inputs). **Required:** stop at the NONE token, discard the function, do not read past the token array.
  Recommended: add `Unexpected EOF` (the benign reading of the reference adds nothing, so the effect would silently lose the function). The vectors do not pin either choice.
- **D-2** (NULL dereference) The backend's preprocessor name used as an identifier (`float _OPENGL;`) crashes the reference (the predefined macro has no token array, S-011 D-4 family). **Required:** expand to nothing (empty macro).
- **D-3** `gs_effect_get_technique(e, NULL)` (and so `gs_effect_loop(e, NULL)` when not looping) crashes on `strcmp`. **Required:** return NULL / false.
- **D-4** `gs_technique_get_pass_by_name`, `gs_technique_begin_pass_by_name` crash when an unnamed pass (NULL name) is compared before a match. **Required:** unnamed passes never match; no crash.
- **D-5** `gs_effect_set_default(NULL)` crashes. **Required:** log ERROR `effect_setval_inline: invalid param`, no-op.
- **D-6** `gs_effect_get_param_by_name(e, NULL)` and `gs_param_get_annotation_by_name(p, NULL)` crash when there is something to compare. **Required:** NULL.
- **D-7** `gs_technique_get_pass_by_idx(t, passes)` and `gs_param_get_annotation_by_idx(p, count)` return a pointer one past the end (the bound check is `>` instead of `>=`). **Required:** NULL. The vectors print `past-end` for the reference's non-NULL result and the
  README's comparison rule accepts `null` for it (`60-runtime-lookup`, `90-defects`).
- **D-8** Tokens that come from an `#include`d file keep a pointer to a dead stack object (S-011 D-4): errors positioned inside included files print garbage (file `(null)`, wrong row/column) in the reference.
  **Required:** report the included file's name and its own row/column. (`ignore` case in `90-defects`.)

- **D-9** (memory leaks on error paths; not observable) The reference leaks memory when a sampler item hits the end of input (`ep_parse_sampler_state_item`), when a pass names an unknown entry function (`ep_makeshaderstring` loses the call string,
  see Q-B) and when a technique parse returns early after its name was read. Found with the leak sanitizer in `11-samplers`, `34-assembly-call`, `41-techniques-errors`, `80-quirks`. **Required:** no leaks (automatic in Rust). The runs are otherwise clean under
  AddressSanitizer/UBSan except for the deliberate `90-defects` cases.

## 9. Implementation notes (non-normative)

- Model the effect as plain Rust structs; keep parameter order and give lookups `Option` handles (index-based ids avoid pointer identity issues; `annotation`/`param` share one type).
- Reproduce the *procedures* of section 6 literally (they are written to be transliterated): the error recovery is intricate and every vector depends on exactly which tokens were consumed.
  A table-driven token cursor with the S-011 helpers as methods is the natural structure.
- The shader text is built with plain string appends; compare byte-for-byte. Keep the three-lists dependency model rather than a graph walk: duplicates and order are observable.
- `default_val` as `Vec<u8>` plus the declared type is enough; the `long` width should be a build constant (8) with a note for Windows.
- The fake backend of the conformance runner (`gen/effect_stubs.c`) is deliberately simple; a port can reuse its design: a `Backend` trait, a shader object that remembers its text/location, and a shader-parser-based parameter table.
- WGSL transpiler authors (Phase 3): the text of section 4.3 is the transpiler input; rely on `shader_parse` results (R90-R93), not on the effect parser's internals.

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01, R02, R03, R04 | `12-toplevel`, `06-params-errors`, `43-backend`, `66-lifecycle` |
| R05, R06 | `66-lifecycle` (incl. a missing file) |
| R10-R13 | `01-params-types`, `03-params-specifiers`, `06-params-errors` |
| R14 | `04-params-array`, `07-params-quirks` |
| R15 | `05-params-annotations` |
| R16-R20 | `02-params-defaults`, `06-params-errors`, `07-params-quirks` |
| R40 | `10-structs` |
| R50 | `11-samplers` |
| R30, R31 | `20-functions`, `21-functions-errors`, `31-assembly-deps` |
| R60, R61 | `40-techniques`, `41-techniques-errors`, `34-assembly-call` |
| R70 | `01-params-types`, `60-runtime-lookup`, `70..75-corpus-*` |
| R71 | `30-assembly-basic`, `43-backend`, `80-quirks` |
| R72, R73 | `32-assembly-params`, `34-assembly-call`, `80-quirks` |
| R74 | `31-assembly-deps`, `35-assembly-text` |
| R75 | `33-assembly-main`, `80-quirks` |
| R80 | `60-runtime-lookup` (incl. duplicate ViewProj/World), `65-runtime-null` |
| R81, R82 | `61-runtime-values`, `65-runtime-null` |
| R83-R87 | `62-runtime-loop`, `63-runtime-passes`, `64-runtime-textures` |
| R88 | `62-runtime-loop` |
| R90-R93 | `70-sparse-params`, `71-sparse-structs-samplers`, `72-sparse-functions` |
| R94-R97 | `71-sparse-structs-samplers`, `73-converters` |
| Q-1 .. Q-6, Q-A .. Q-K | `07`, `31`, `34`, `40`, `41`, `43`, `71`, `80-quirks` |
| D-1 .. D-9 | `90-defects` (plus `44-preprocessor` for D-2's non-crashing counterpart) |
| corpus | `70-corpus-libobs-basic`, `71-corpus-libobs-scale`, `72-corpus-libobs-deinterlace`, `73-corpus-obs-filters`, `74-corpus-obs-transitions`, `75-corpus-other` |

Conformance classes (README, "Comparison rules"): `07-params-quirks`, `80-quirks` and the QUIRK-marked cases inside `31-assembly-deps` (e4b), `34-assembly-call`, `40-techniques`, `41-techniques-errors`, `43-backend`
and `71-sparse-structs-samplers` (BorderColor) are **QUIRK** (SHOULD match); `90-defects` is **DEFECT** (the `try-end crash` / `ignore` results are not to be reproduced; `past-end` lines accept `null`);
every other script is **CORE** and every line must match, log lines by level only.

## 11. Open questions

- The Windows `long` width (4 bytes) changes `default_val` for int and bool parameters; the committed vectors are LP64. The D3D11/Metal backends read `default_val` only through `gs_shader_set_val`, which passes `cur_val.size` bytes; backends that
  assume 4-byte ints would disagree on Linux - not investigated (no backend in this repository was read for it).
- `effect_param_parse_property` is declared in `effect.h` but has no definition in the reference; nothing calls it.
- Exact error positions of items added after parsing (`Error creating shader`) depend on the final cursor; for text ending without a newline the position is the column after the last character (observed), for text ending in newlines it is the start of the next line (inferred from the S-011 NONE-token rule, not separately tested).
- Thread-safety of `gs_effect_create` against concurrent `gs_effect_create_from_file` for the same path (both may parse); not tested.
