# S-026 `rtmp-services` (streaming service catalogue plugin)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `plugins/rtmp-services/`: `rtmp-common.c`, `rtmp-custom.c`, `rtmp-services-main.c`, `rtmp-format-ver.h`, `service-specific/{twitch,amazon-ivs,service-ingest,nimotv,showroom,dacast}.c` |
| Target crate | `crates/plugin-rtmp-services` |
| Depends on | S-014 (`obs_data`: settings objects, typed getters, `has_user_value`), S-015 (`obs_properties`: list/text/bool properties, `modified` callbacks, `apply_settings` order), S-024 (file-updater: package download, `package.json` version handling, cache directory logic, confirm callback contract) |
| Conformance | `test/conformance/scripts/rtmpservices/` (28 scripts), runner `gen/rtmpservices_runner.c` |

## 1. Scope

Two *service* types registered with the host:

* `rtmp_common` - "Streaming Services": a catalogue of hosted services read from `services.json`
  (server lists, recommended encoder limits, supported codecs, multitrack/"Enhanced Broadcasting"
  hints). Five names get special treatment (Twitch, Amazon IVS, Nimo TV, SHOWROOM, Dacast).
* `rtmp_custom` - "Custom Streaming Server": user typed server / key / optional credentials.

Out of scope: the host's service wrapper (`obs_service_*` default handling), the file-updater
(S-024), RTMP/SRT/RIST output implementations, the frontend's own reading of `services.json`
(it also reads `recommended.output`, which this plugin ignores) and `package.json` handling.

### 1.1 Host contract assumed by this spec

The plugin is driven through a vtable. The following host behaviour is assumed (it is implemented
by libobs, not by the plugin, and reproduced by the conformance runner):

* Before `get_max_fps`, `get_max_bitrate`, `get_supported_resolutions` the host zeroes the out
  values (`0` / `NULL`,`0`); an absent optional callback leaves them zero.
* `apply_encoder_settings` is forwarded only when at least one settings object is non-NULL.
* `can_try_to_connect` absent => true.
* `obs_service_properties` = `get_properties(data)` followed by `obs_properties_apply_settings`
  with the instance settings. `apply_settings` runs the `modified` callbacks **last property first**
  (so for `rtmp_common`: `show_all`, then `service`); see S-015.
* Connect-info ids: URL 0, stream id (key) 2, username 4, password 6, encrypt passphrase 8,
  bearer token 10.
* Strings returned by getters are valid until the next call on the same instance (the Rust API may
  return owned `String`/`Option<String>`; `None` models NULL and is distinct from `""`).
* A module has a *data directory* (read only, shipped; `obs_module_file`) and a *config directory*
  (user cache; `obs_module_config_path`). `obs_module_file(f)` yields NULL when the file does not
  exist (the empty name yields the directory itself); `obs_module_config_path(f)` is `config_dir + f`.
* `obs_module_text(key)` returns the translation (en-US below) or the key itself.

### 1.2 Locale strings (en-US, used verbatim in property labels / disclaimer)

| key | text |
|---|---|
| `StreamingServices` | Streaming Services |
| `CustomStreamingServer` | Custom Streaming Server |
| `Service` / `Server` | Service / Server |
| `Server.Auto` | Auto (Recommended) |
| `Server.AutoRTMPS` | Auto (RTMPS, Recommended) |
| `Server.AutoRTMP` | Auto (RTMP) |
| `StreamKey` | Stream key |
| `UseAuth` / `Username` / `Password` | Use authentication / Username / Password |
| `ShowAll` | Show all services |
| `MultitrackVideo.Disclaimer` | `%1 automatically optimizes your stream settings, which may include encoding multiple video qualities and/or canvas orientations. Selecting this option will send %2 information about your computer and software setup.` |
| `MultitrackVideo.LearnMoreLink` | ` <a href='%1'>Learn More</a>` (leading space) |

## 2. `services.json` format (format_version 5)

```
{ "format_version": 5,                       // integer, must equal 5
  "services": [ SERVICE, ... ] }

SERVICE = { "name": str,                      // required for use (see D-2)
            "common": bool,                   // listed without "show all" (default false)
            "alt_names": [str,...],           // old names; non-strings ignored
            "protocol": str,                  // "RTMP" | "RTMPS" | "SRT" | "RIST" | "HLS" | ...; optional
            "servers": [ {"name": str, "url": str}, ... ],
            "more_info_link": str, "stream_key_link": str,
            "multitrack_video_configuration_url": str,
            "multitrack_video_name": str, "multitrack_video_learn_more_link": str,
            "supports_additional_audio_track": bool,
            "supported video codecs": [str,...], "supported audio codecs": [str,...],
            "recommended": { "keyint": int, "profile": str, "bframes": int, "x264opts": str,
                             "max video bitrate": int, "max audio bitrate": int, "max fps": int,
                             "supported resolutions": ["WxH", ...],
                             "bitrate matrix": [ {"res": "WxH", "fps": int, "max bitrate": int}, ... ],
                             "output": ... /* ignored here */ } }
```
Typed access is strict: `get_string` yields a value only for a JSON string, `get_int` only for a
JSON *integer* (a float such as `5.0` counts as absent => 0), `get_bool` only for `true`/`false`
(absent => false). Unknown keys are ignored. The production file (`$schema`, 70+ services) uses
exactly the keys above (`$schema` at the top level); script `27-smoke-real-services` prints the key
sets found.

Worked example (script data `services-ok.json`, service "Alpha"): `common:true`, two servers
`Primary rtmp://a.alpha.example/live` and `Secure rtmps://b.alpha.example:443/live` (plus a
nameless one that is never listed), `recommended.keyint 3`, ...

## 3. Rules - locating and validating the file

* **S-026-R01** *Candidates.* Every operation that needs the catalogue reads, in order,
  `<config dir>/services.json` (skipped when there is no config path) then
  `<data dir>/services.json` (skipped when the module file is not found). The first candidate that
  passes R02-R04 is used; a candidate that fails is skipped silently except for the logs below.
  If none passes the catalogue is *absent*. The file is **re-read on every operation** (update,
  properties, apply, max bitrate, codec list on first use); nothing is cached across operations.
* **S-026-R02** *Reading.* Bytes are read as UTF-8 text with a leading BOM `EF BB BF` removed;
  a missing or **empty** file is unreadable (skip, no log). The text is parsed as strict JSON in
  which **duplicate object keys at any depth are an error** (log WARNING
  `[open_json_file] Error reading JSON file (<line>): <parser text>`, skip).
* **S-026-R03** *Format version.* `format_version` must be a JSON integer equal to 5. Otherwise
  (missing, string, float, other integer) log DEBUG `Wrong format version (<n>), expected 5` with
  `n` = 0 for non-integers, and skip.
* **S-026-R04** *Services list.* The `services` member must exist (any JSON type); else log
  WARNING `No services list` and skip. A present-but-empty array is a **valid** catalogue (no
  fallback to the next candidate). A non-array value is accepted as catalogue but every consumer
  treats it as an empty list (R22 additionally logs WARNING `JSON file root is not an array`).

## 4. Rules - `rtmp_common` instance

State per instance: `service`, `protocol`, `server`, `key` (strings, never NULL after update),
`supported_resolutions` (list of `(cx,cy)`), `max_fps`, `supports_additional_audio_track`,
cached `video_codecs` / `audio_codecs` string lists (lazily filled).

* **S-026-R10** *Update (also create).* Copy `service`, `protocol`, `server`, `key` from settings
  (missing => `""`). Reset resolutions (none), `max_fps = 0`, `supports_additional_audio_track =
  false`, and **drop both codec caches** (they are refilled lazily). Then, if the catalogue is
  present, apply R11-R13; if absent nothing else happens (so `protocol` may remain `""`, Q-6).
* **S-026-R11** *Lookup + rename.* Look up the stored service name with R20. If it was found via an
  alternative name, replace the instance's `service` with the canonical name. The **settings
  object is not modified** here (Q-2).
* **S-026-R12** *Protocol default.* If the instance protocol is `""`, set it to the result of
  R30 evaluated for the found service (`serv` may be NULL: result `"RTMP"`). R30 may also write
  `protocol` into settings (see there).
* **S-026-R13** *Found service.* (a) `copy_info_to_settings` (R40); (b) if `recommended` is an
  object: supported resolutions (R94) and `max fps` = integer `max fps` or 0; (c)
  `supports_additional_audio_track` = bool value; (d) `ensure_valid_url` (R50).
* **S-026-R14** *Getters.* `get_protocol` returns the instance protocol (`"RTMP"` only if NULL,
  which cannot happen after an update). `get_username` / `get_password` return the Dacast values
  (R82) for Dacast and NULL otherwise. `get_connect_info(type)`: URL -> R80, stream id -> R81,
  username -> username, password -> password, encrypt passphrase -> **password if protocol is
  exactly `"SRT"`, key if exactly `"RIST"`, else NULL**; bearer token and every other id -> NULL.
  `can_try_to_connect`: let `key` = the R81 result (the *fetched* key for SHOWROOM/Dacast); Dacast => `key`
  non-NULL and non-empty (no url test); otherwise `url` (R80 result) non-empty **and** `key` non-empty
  (`key` evaluated first, then `url`; both may trigger network access).
* **S-026-R15** *Instance without file.* All catalogue-derived getters return their zero values:
  resolutions `[]`, max fps 0, max bitrate `0/0`, codecs NULL, apply does nothing.

### 4.1 Service lookup (R20-R22)

* **S-026-R20** *`find_service(name)`.* Iterate the catalogue in file order. Skip entries that
  are not *available* (R21). For each remaining entry: if `name` equals the entry's `name`
  (byte-exact, case-sensitive) return it; else if `name` equals any **string** element of its
  `alt_names` array return it and report the entry's `name` as the *new name*. The first hit in file
  order wins; an earlier entry's alt name beats a later entry's real name (Q-3). NULL result if none.
* **S-026-R21** *Availability.* An entry is available if it has a string `protocol` and that
  protocol is registered in the host (`obs_is_output_protocol_registered`, exact case-sensitive
  compare); otherwise it is available iff **at least one** server URL begins with `rtmp://` and
  RTMP is registered, or begins with `rtmps://` and RTMPS is registered (the plain-URL test needs
  `servers` to be an array of objects whose `url` is a string; otherwise D-1). `http(s)://`, `srt://`
  etc. URLs without a `protocol` key make the entry unavailable. A non-array `servers` or an empty
  array => unavailable. Unavailable entries are invisible to lookup, listing, and every derived getter.
* **S-026-R22** *Service list contents* (`add_services`). Non-array root => WARNING and stop.
  For each available entry in order, `add_service`: not an object => WARNING (`service is not an
  object`), skip; no string `name` => WARNING (`service has no name`), skip; `common` false and
  `show_all` false and name != current service name => skip; `servers` missing or not an array =>
  WARNING (`service '<name>' has no servers`), skip; else append item (label = value = name).
  Afterwards, if `find_service(current)` (by R20, current name may be an alt name) finds nothing and
  the current name is non-empty, **insert** an item (label = value = current) at index 0 and mark it
  **disabled** (Q-4). Note: because a current service given by *alternative* name is found but its
  entry is not listed under that name, the list lacks it (Q-5).

### 4.2 Protocol derivation (R30)

* **S-026-R30** `get_protocol(service, settings)`: if the entry has a string `protocol`, return
  it (nothing written). Else if `servers` is not an array return `"RTMP"`. Else look at the
  **first** server's `url`: if it begins with `rtmps://` set `settings["protocol"] = "RTMPS"` and
  return `"RTMPS"`; otherwise return `"RTMP"` (no write). A first server without string url is D-1.
  `update_protocol` (used by R40) is the same decision but **always** writes `settings["protocol"]`
  (`protocol` key value, or `"RTMPS"`, or `"RTMP"`; if `servers` is not an array nothing is written).

### 4.3 Settings enrichment (R40)

* **S-026-R40** `copy_info_to_settings(entry, settings)`, with `name = settings["service"]`
  (the *current settings value*, which may still be an alt name, R11):
  1. copy string members `more_info_link`, `stream_key_link`, `multitrack_video_configuration_url`,
     `multitrack_video_name` into settings **only if present as strings** (absent => the old settings
     value stays, Q-7);
  2. if settings has no *user value* `multitrack_video_name`, set it to `"Multitrack Video"`;
  3. `disclaimer` = `MultitrackVideo.Disclaimer` with `%1` replaced by `settings["multitrack_video_name"]`
     and `%2` by `name`; if the entry has string `multitrack_video_learn_more_link` then
     `MultitrackVideo.LearnMoreLink` with `%1` replaced by that url is appended; store as
     `settings["multitrack_video_disclaimer"]` (string; always written);
  4. `update_protocol` (R30).
  Replacement is "replace every occurrence" applied `%1` first then `%2` (a `%2` inside the name or
  `%1` inside the learn-more link is not re-expanded because the link is appended afterwards, but a
  `%2` inside `multitrack_video_name` **is** replaced).
* **S-026-R50** *`ensure_valid_url`.* Applies only if the service **name contains "Facebook"**
  (ASCII case-insensitive substring) and the instance `server` is non-NULL and `servers` is an array:
  scan the entries' string `url`s (entries without a string url are skipped); if the instance
  `server` equals one of them case-insensitively nothing happens; otherwise set instance `server`
  and `settings["server"]` to the **first** url seen (if any). An empty `server` string is
  "not found" and is replaced as well. Other services keep unknown servers untouched.

### 4.4 Properties (R60-R72)

* **S-026-R60** `get_properties` builds, in this order: list `service` (label "Service", combo list
  type = 2 `OBS_COMBO_TYPE_LIST`, string format = 3), bool `show_all` ("Show all services"), list
  `server` ("Server", list, string), text `key` ("Stream key", password type = 1). The properties
  object *owns the catalogue loaded at that moment* (`param`); with no catalogue the param is NULL.
  Property modified callbacks are set on `service` (R61) and `show_all` (R62). The item lists start empty;
  they are filled only by `apply_settings`/`modify` (the host's `obs_service_properties` applies the
  settings, which first fires `show_all`, then `service`).
* **S-026-R61** *`service` modified* (`service_selected`): `name = settings["service"]`; if NULL/empty
  return **false**. `entry = find_service(param, name)` (param NULL => none). If none: insert
  `(name,name)` at index 0 of the service list, disabled; insert `(server, server)` at index 0 of the
  **server list**, disabled, where `server = settings["server"]`; return true (lists are *not*
  cleared first: stale items stay, Q-4). If found: rename `name` to the canonical name and write it to
  `settings["service"]`; rebuild the server list (R70); then R40 on the settings; return true.
* **S-026-R62** *`show_all` modified:* if the properties have no param return **false** (the service list
  is not built; it stays as it was); else clear the service list and rebuild it by R22 using
  `show_all = settings["show_all"]` (bool) and `current = settings["service"]`; return true.
  (It does not refill the server list.)
* **S-026-R70** *Server list fill* for service name `name` (canonical): clear the list; if `servers`
  is not an array log WARNING (`Servers for service '<name>' not a valid object`) and stop (empty list).
  Then:
  * `name == "Twitch"`: add `(Auto "Auto (Recommended)", "auto")`; if the number of known Twitch
    ingests is **> 1**, add `(ingest.name, ingest.url)` for each and stop; otherwise continue with the
    generic loop (so the list is "Auto" + JSON servers).
  * `name == "Nimo TV"`: add `("Auto (Recommended)", "auto")` and continue with the generic loop.
  * `name == "Amazon IVS"`: if RTMPS is registered add `("Auto (RTMPS, Recommended)", "auto-rtmps")`;
    add `("Auto (RTMP)", "auto-rtmp")`; if known ingests **> 1**: if RTMPS is registered add one item
    per ingest `("<name> (RTMPS)", ingest.rtmps_url)` (value may be NULL, Q-9), then one per ingest
    `("<name> (RTMP)", ingest.url)`, and stop; otherwise continue with the generic loop.
  * generic loop: for each server entry with a **string `name` and string `url`**, skipping `rtmps://`
    URLs when RTMPS is not registered, append `(name, url)`.
  The "known ingest" lists are the module-wide lists of R150.
* **S-026-R71** `server` and `key` carry no callbacks; their `visible`/`enabled` are true.
* **S-026-R72** `rtmp_common` has no defaults callback.

### 4.5 URL and key (R80-R83)

* **S-026-R80** *URL* (`get_url`), evaluated in this order, first match returns:
  1. service == `"Twitch"` and server == `"auto"`: request an ingest refresh with 3 s wait (R152) and
     return the URL of ingest #0 (NULL if there is none, D-5).
  2. service == `"Amazon IVS"` and server **starts with** `"auto"`: refresh (3 s); return ingest #0's
     `url` if the server is **exactly** `"auto-rtmp"`, else its `rtmps_url` (so `"auto"`, `"auto-rtmps"`,
     `"automatic"` all give the RTMPS url). Case-sensitive. The built-in default ingest has
     `url = rtmps://ingest.global-contribute.live-video.net:443/app/` and **no** `rtmps_url` (Q-8).
  3. service == `"Nimo TV"` and server == `"auto"`: `nimotv_get_ingest(key)` (R153).
  4. service == `"SHOWROOM"` and server and key non-NULL: `showroom_get_ingest(server, key).url` (R154).
  5. service == `"Dacast"` and server and key non-NULL: `dacast_ingests_load_data(server, key)` then
     `dacast_ingest(key).url` (R155).
  6. otherwise the instance `server`.
  Service names are compared exactly after the R11 rename.
* **S-026-R81** *Key* (`get_key`): SHOWROOM with server and key -> `showroom_get_ingest(...).key`
  (same cache as the URL, so no extra request within 10 s); Dacast with key (non-NULL, **even empty**,
  Q-10) -> `dacast_ingest(key).streamkey`; else the instance key.
* **S-026-R82** `get_username`/`get_password`: Dacast with key -> `dacast_ingest(key).username` /
  `.password`; else NULL. They never download.
* **S-026-R83** Dacast `get_url` downloads **on every call** (R155); `get_key/user/password` only read.

### 4.6 Applying recommended encoder settings (R90-R93)

* **S-026-R90** `apply_encoder_settings(video, audio)` (either may be NULL): if the catalogue is
  absent do nothing. `entry = find_service(instance.service)`; none => WARNING
  `[initialize_output] Could not find service '<name>'` when the name is non-empty, then return;
  no `recommended` member (any type but absent) => return. Otherwise apply R91 to `video` and R93 to
  `audio` when non-NULL.
* **S-026-R91** *Video:* (1) if `keyint` is an integer set `keyint_sec`; (2) **always** set string
  `rate_control = "CBR"` (even without `keyint`); (3) if `recommended.profile` is a string and the
  settings' existing item `profile` has type string, set `profile` (an absent or non-string item is not
  replaced - the encoder default is not materialised); (4) compute `max_bitrate` = R92 matrix result if
  `bitrate matrix` is an array and it yields non-zero, else `max video bitrate` if an integer, else 0;
  if `max_bitrate != 0` and settings `bitrate` (int, absent => 0) > `max_bitrate`, set both `bitrate`
  and `buffer_size` to `max_bitrate`; (5) if `bframes` is an integer set `bf`; (6) if `x264opts` is a
  string set `x264opts` = existing `x264opts` (or `""`) + (`" "` if the existing is non-empty) +
  the recommended string (**appended**, never de-duplicated).
* **S-026-R92** *Bitrate matrix* (also used by R110): needs host video info (`obs_get_video_info`
  fails => result 0). `cur_fps = fps_num / fps_den` (double division; `fps_den` is non-zero in the host).
  Iterate rows in order; skip rows that are not objects, lack a string `res`, or whose `res` does not
  `sscanf("%dx%d")` to two numbers (trailing text ignored, Q-11); a row matches iff `cx == output_width`
  and `cy == output_height` and `cur_fps <= (double)(int fps) + 1e-7` (`fps` absent => 0); the **first**
  matching row's integer `max bitrate` (absent/non-int => 0) is the result; no match => 0. A matched row with
  `max bitrate` 0 yields 0 and so falls back to `max video bitrate`.
* **S-026-R93** *Audio:* if `max audio bitrate` is an integer and settings `bitrate` > it, set `bitrate`.

### 4.7 Limits and codecs (R94-R96, R110)

* **S-026-R94** *Resolutions* (on update): `recommended."supported resolutions"` array; each **string**
  element parsed with `sscanf("%dx%d")` (both must convert; trailing text accepted, Q-11); entries with
  `cx <= 0` or `cy <= 0` skipped; duplicates kept; order kept. If none valid the list is empty.
  `get_supported_resolutions` returns a copy (count 0 => NULL).
* **S-026-R95** *Max fps:* integer `recommended."max fps"` else 0; `get_max_fps` returns it.
* **S-026-R110** *Max bitrate* (`get_max_bitrate(video*, audio*)`, host zeroes both first): no
  catalogue, no entry or no `recommended` member => leave both untouched. Audio: if pointer non-NULL
  and `max audio bitrate` is an integer set it (else untouched). Video: if pointer non-NULL:
  `matrix` (R92, only if array) else `max video bitrate` (integer, else 0); **write** the result (0 if none).
* **S-026-R96** *Codec lists* `get_supported_video_codecs` / `..._audio_codecs`: return the cached list
  if the instance has one. Else read the catalogue (absent => NULL), find the service (R20 on the
  instance service), take the array `supported video codecs` / `supported audio codecs` (not an
  array => NULL). Each element is formatted into a buffer of **15 characters max** (longer names are truncated to the first
  15 bytes), the elements are joined with `;` and then split again on `;` **dropping empty
  segments**, so an element containing `;` becomes several codecs, an empty element disappears, and an
  empty/all-empty array yields NULL. The result is cached for the instance until the next update
  (D-6 for non-string elements). The returned list is NULL-terminated.

### 4.8 `rtmp_custom`

* **S-026-R120** *State/update:* `server`, `key`, `username`, `password` strings (missing => `""`),
  `use_auth` bool.
* **S-026-R121** *Getters:* url = server, key = key, username/password = value only if `use_auth`
  else NULL. `get_connect_info`: same mapping as R14 with these getters (passphrase: protocol `"SRT"`
  -> password (subject to `use_auth`, so NULL when auth is off), `"RIST"` -> key).
* **S-026-R122** *Protocol* by **case-sensitive prefix** of `server`: `rtmps://` -> `"RTMPS"`,
  `srt://` -> `"SRT"`, `rist://` -> `"RIST"`, anything else (including `RTMPS://`, `http://`, `""`)
  -> `"RTMP"`.
* **S-026-R123** *can_try_to_connect:* server non-empty (key not considered).
* **S-026-R124** *apply_encoder_settings:* with `protocol = R122`: if the protocol is not RTMP/RTMPS and
  `video` is non-NULL set bool `repeat_headers = true`; if the protocol is SRT or RIST and `audio` is
  non-NULL set bool `set_to_ADTS = true`. Nothing else.
* **S-026-R125** *Properties:* text `server` ("URL", literal not localized, default text type 0), text
  `key` ("Stream key", password), bool `use_auth` ("Use authentication"), text `username` ("Username",
  default), text `password` ("Password", password). `username` and `password` are created visible; the
  modified callback of `use_auth` sets both `visible = settings["use_auth"]` and returns true. It is
  only run by `apply_settings`/`modify`, so before that both are visible.
* **S-026-R126** `rtmp_custom` has no resolution/fps/bitrate/codec callbacks (host defaults apply).

## 5. Rules - module and ingest providers

* **S-026-R140** *Module load.* In order: initialise the Twitch, Dacast and Amazon IVS tables;
  `module_name = "rtmp-services plugin (libobs " + libobs version string + ")"`; add two procedures to the
  global proc handler: `void twitch_ingests_refresh(int seconds)` and
  `void amazon_ivs_ingests_refresh(int seconds)`. If built with updates (the reference build for the
  conformance vectors has them enabled; the update base URL is
  `https://obsproject.com/obs2_update/rtmp-services` by default) and the config path is non-NULL create the
  package updater `update_info_create(log_prefix "[rtmp-services plugin] ", user_agent module_name,
  url "<base>/v5", local_dir = data dir, cache_dir = config dir, confirm = R141)`; then load the cached
  Twitch and Amazon IVS ingests (R151) (both executed even without a config path). Register `rtmp_common` and
  `rtmp_custom` (in this order). Always returns true.
* **S-026-R141** *Confirm callback* (called by the updater for every downloaded file): for a file whose
  name equals `services.json` **ASCII case-insensitively**, parse its text as JSON with the host's `obs_data` JSON reader (**duplicate keys are rejected here too**, logging an ERROR
  and returning false), read `format_version` as an integer (a float is truncated: `5.9` -> 5,
  `"5"` -> 0) and accept (true) iff it equals 5; unparsable text or non-object root -> false. Every other
  file name -> true.
* **S-026-R142** *Unload:* destroy the package updater, free the Twitch ingests and the updater, SHOWROOM
  cache, Dacast table, Amazon IVS ingests, module name. (The reference does not support load -> unload ->
  load in one process: stale pointers, Q-12; implementations need not.)
* **S-026-R143** *Refresh procs:* argument `seconds` (int): `<= 0` -> 3, `> 10` -> 10, then call the
  Twitch / Amazon IVS refresh (R152).

### 5.1 Ingest tables: Twitch and Amazon IVS (`service-ingest.c`)

Each of the two providers keeps a module-global *ingest table* `{ingests: [Ingest{name,url,rtmps_url}],
loaded, refreshing, refreshed (flags), updater, mutex}` protected by a mutex; lookups return a *copy* of
the three pointers by index (out of range => all NULL).

* **S-026-R150** *Built-in default:* Twitch: `{"Default", "rtmp://ingest.global-contribute.live-video.net/app", no rtmps}`;
  Amazon IVS: `{"Default", "rtmps://ingest.global-contribute.live-video.net:443/app/", no rtmps}`.
* **S-026-R151** *Cache load (module load):* push the default; if `<config dir>/twitch_ingests.json`
  (resp. `amazon_ivs_ingests.json`) exists, read it and run R156 (without writing the cache); success sets `loaded`.
  Unreadable/invalid cache leaves the default.
* **S-026-R152** *Refresh(seconds):* if `refreshed` is set return immediately. If `refreshing` is not
  yet set, set it and create a single-file updater for `https://ingest.twitch.tv/ingests` (Twitch, log
  prefix `[twitch ingest update] `) resp. `https://ingest.contribute.live-video.net/ingests` (Amazon IVS,
  log prefix `[amazon ivs ingest update] `) with user agent = module_name and callback R157. If `loaded`
  is **not** set, poll up to `seconds * 100` times with a 10 ms sleep, stopping as soon as `refreshed` is set.
  So without an answer each call blocks `seconds` seconds (Q-13: not only the first call). Once `refreshed` was
  set the call is a no-op for the rest of the process.
* **S-026-R156** *Load ingests* (`load_ingests(json, write_file)`): parse JSON (plain, duplicates allowed);
  failure or missing `ingests` member => fail. `count = array length` (non-array => 0). If `count <= 1` and the
  table is non-empty => fail (table untouched, so a one-element list never replaces anything once Default exists).
  Else **free the table**, then for each element: skip if `name` or `url_template` is missing (any type);
  skip if the (string) `name` contains `deprecated` or `depracated` (substring); otherwise append
  `{name, url = url_template with every "/{stream_key}" removed, rtmps_url = url_template_secure with the
  same removal (NULL if absent or non-string)}`. Non-string `name` => crash (D-3); non-string
  `url_template` => NULL url. If the table is now empty => fail (**the table stays empty**, D-5). On success, when
  `write_file`: write the raw downloaded JSON to `<config>/<provider>_ingests.new.json` then replace
  `<config>/<provider>_ingests.json` with it (safe replace, no backup).
* **S-026-R157** *Updater callback:* lock the table, run R156 with `write_file = true`; on success set
  `refreshed` and `loaded`; always return true.

### 5.2 Nimo TV (`nimotv.c`)

* **S-026-R153** `get_ingest(key)`: if a previous result string exists (non-NULL) and fewer than **2 s** (fractional
  difference `< 2`, whole-second clock) passed since the last *successful* fetch, log INFO and return it
  (the cache ignores the key, Q-14). Else HTTP GET `https://globalcdnweb.nimo.tv/api/ingests/nimo?id=<key>` where
  `<key>` is percent-encoded (every byte except ASCII alphanumerics and `-._~` becomes `%XX`, uppercase hex),
  timeout 3 s, TLS peer and host verification on. Failure (transfer error, status != 200, empty body) logs
  WARNING and returns NULL **keeping** the previous cached string and time. Success: replace the cached string
  by `ingests[0].url` of the JSON body (NULL if unparsable, no `ingests`, empty array, or non-string url),
  record the time, log INFO, return it (possibly NULL).

### 5.3 SHOWROOM (`showroom.c`)

* **S-026-R154** `showroom_get_ingest(server, access_key)`: a table keyed by `access_key` holds
  `{url, key, last_time_s}`. If an entry exists and `< 10` whole seconds passed since `last_time_s`
  return it. Otherwise GET `server + access_key` (timeout 30 s, TLS verification on). Failure (error,
  status != 200, empty body) => WARNING and return the shared **invalid ingest `{url "", key ""}`** (an old expired entry is
  kept in the table but not returned). Success: parse JSON (duplicates **rejected**; failure => invalid
  ingest); `url = streaming_url_rtmp`, `key = streaming_key` (strings; absent/non-string => **NULL**),
  create or replace the entry, set `last_time_s`, return it. `server` or `access_key` empty
  => the request URL is empty and the fetch fails (invalid ingest).

### 5.4 Dacast (`dacast.c`)

* **S-026-R155** `dacast_ingests_load_data(server, key)`: clear `loaded`; destroy any previous updater;
  create a single-file updater (log prefix `[dacast ingest load data] `) for URL `server + key`
  (NULL if both empty) with callback `ingest_update(param = key)`; if `loaded` is still unset poll up to 300 times
  with 10 ms sleeps (3 s). Callback: lock, `load_ingests(json, key)`: find or **create** the table entry for `key`
  (an entry now exists even if the answer is bad, with all fields NULL after a failure), free its previous
  strings, parse JSON; require object `stream` with all of `server`, `username`, `password`, `streamkey`
  present (any type), copy their string values (NULL for non-strings); on success `loaded` is set.
  Always returns true.
  `dacast_ingest(key)` returns the table entry for `key` (exact match) or the static **invalid ingest**
  `{url "rtmp://dacast", username "", password "", streamkey "fake_key"}` when no entry exists.

## 6. Concurrency

The ingest tables and the Dacast table are protected by one mutex each; the callbacks run on the updater
thread and the refresh waits poll flags with atomic loads. NIMO/SHOWROOM caches are unlocked globals (single
caller assumed). A Rust implementation should use `Mutex`/atomics; the conformance runner is single threaded
(updater answers are delivered on the caller's thread inside the sleep loop).

## 7. Reference quirks (SHOULD match) and defects (MUST differ)

| ID | Kind | Behaviour |
|---|---|---|
| Q-1 | QUIRK | `apply_settings` fires the `show_all` callback before the `service` callback (reverse property order). |
| Q-2 | QUIRK | After a rename through an alt name, the *instance* has the canonical name but `settings["service"]` keeps the alt name until the `service` callback runs; `copy_info_to_settings` in update uses the alt name in the disclaimer. |
| Q-3 | QUIRK | R20 matches alt names of earlier entries before names of later ones. |
| Q-4 | QUIRK | An unknown current service is inserted at the top of the service list (disabled) **and** the `service` callback inserts it again, producing two identical disabled items; the server list gets a disabled stored-server item at index 0 and is not cleared. |
| Q-5 | QUIRK | A current service given by alt name is not listed under any name (non-common). |
| Q-6 | QUIRK | Without catalogue, `protocol` stays `""` (not `"RTMP"`), and `get_protocol` returns `""`. |
| Q-7 | QUIRK | `multitrack_video_name` etc. are not cleared when the new service lacks them: the previous service's name leaks into the next disclaimer (the "Multitrack Video" default is only used if there is no user value). |
| Q-8 | QUIRK | Amazon IVS default ingest holds the rtmps URL in `url`; `auto-rtmps` gives NULL and `auto-rtmp` the rtmps address until a download replaces the list. |
| Q-9 | QUIRK | Amazon IVS list items for ingests without `url_template_secure` have a NULL value. |
| Q-10 | QUIRK | Dacast with an empty key looks up the invalid ingest: key `"fake_key"` and `can_try_to_connect` true. |
| Q-11 | QUIRK | `"WxH"` strings are parsed with scanf: `"1280x720x9"` is 1280x720; leading white space and `+` signs are accepted by `%d`. |
| Q-12 | QUIRK | Unload leaves stale updater pointers; reload is unsupported. |
| Q-13 | QUIRK | Every Twitch/Amazon refresh call blocks `seconds` seconds while no list was ever received. |
| Q-14 | QUIRK | The Nimo 2 s cache is not keyed by the stream key and failures keep the older value. |
| D-1 | DEFECT | A service without `protocol` key whose `servers` contains an element that is not an object with a **string `url`** (missing url, non-string url, non-object element) dereferences NULL in the availability test: **every** operation (create, update, properties, ...) crashes. Required: such a server element is treated as not matching `rtmp://`/`rtmps://`. Scripts `24-defects-crash` D-1..D-1d. |
| D-2 | DEFECT | A catalogue entry that is *available* but has no string `name` crashes the lookup of any name that does not match an earlier entry. Required: such entries are never matched by name. Scripts D-2..D-2d. |
| D-3 | DEFECT | Twitch/Amazon ingest JSON with a non-string `name` crashes in the substring test. Required: skip the element. |
| D-5 | DEFECT | An ingest download whose elements are all skipped (all deprecated / invalid) with `count >= 2` leaves the ingest table **empty** (the old list is freed first); `auto` then resolves to NULL. Required: keep the previous table when the new one would be empty. Script `25-defects-semantic`. |
| D-6 | DEFECT | Non-string codec entries are printed with `%s` of NULL (glibc prints `(null)`; UB). Required: skip them. Script `26-quirk-codec-entries` discards the value. |

(D-4 is intentionally unused.)

## 8. Implementation notes

* Model the catalogue as `serde_json::Value` loaded with a duplicate-key-rejecting parser (e.g. parse
  into an ordered map yourself or use a visitor that errors on duplicate keys). Keep strict typed
  accessors (`as_str`, `as_i64` only for JSON integers, `as_bool`).
* Make the host an injected trait (`ModuleFs`, `ProtocolRegistry`, `VideoInfo`, `Clock`, `Http`,
  `FileUpdater`, `Locale`) so the vectors can drive deterministic fakes.
* All URL/ingest "network" access in the vectors is faked; keep the 10 ms poll granularity observable
  through an injectable `sleep_ms`.
* The reference `time()` has one-second resolution (Nimo cache); SHOWROOM/Dacast use whole seconds of the
  monotonic clock. The vectors use a clock that moves only by `advance` and by sleeps.
* `package.json` and file version selection belong to the updater (S-024); this plugin only supplies the
  confirm callback (R141) and the URL/dirs.
* Windows: none of this module is platform specific; `SERVICES_URL` is a build-time constant.

## 9. Test harness (summary; details in `test/conformance/README.md`)

`gen/rtmpservices_runner.c` links the real plugin sources, the real `obs-data.c` and `obs-properties.c`, and
fakes (in `gen/rtmpservices_stubs.c`) the file system, module paths, locale, protocol registry, video info,
clock, file-updater and libcurl. Vectors record property lists (`props` blocks), settings dumps (keys sorted),
getter results and every fake interaction (`fake ...` lines). Comparison rules for a clean-room runner:

* `log <level> ...` lines: compare the level only.
* `guard-begin ... guard-end crash` (scripts `24`): a reference crash must be reproduced as `guard-end ok`;
  lines between are ignored in that case.
* Script `26` discards the value of non-string codec entries.
* Script `27` prints only structural facts about the production file.

## 10. Coverage

| Rules | Scripts |
|---|---|
| R01-R04 | `23-services-file` |
| R10-R15 | `02-selection`, `03-protocol-derive`, `04-getters`, `22-facebook-url` |
| R20-R22 | `01-list`, `02-selection`, `05-server-lists` |
| R30, R40 | `02-selection`, `03-protocol-derive` |
| R50 | `22-facebook-url` |
| R60-R62, R70 | `01-list`, `02-selection`, `05-server-lists`, `13-twitch`, `17-amazon-ivs`, `19-nimotv` |
| R80-R83 | `04-getters`, `13`-`15`, `17`-`21`, `28-empty-key-specials` |
| R90-R93 | `06-recommended` |
| R94-R96, R110 | `07-limits`, `26-quirk-codec-entries` |
| R120-R126 | `08-custom` |
| R140-R143 | `09-module`, `10-confirm`, `11-proc-clamp`, `12-module-no-config` |
| R150-R152, R156-R157 | `13-twitch`, `14-twitch-download`, `15-twitch-cache-single`, `16-twitch-cache-two`, `17-amazon-ivs`, `18-amazon-ivs-download`, `25-defects-semantic` |
| R153 | `19-nimotv` |
| R154 | `20-showroom`, `28-empty-key-specials` |
| R155 | `21-dacast`, `28-empty-key-specials` |
| Q/D | `24-defects-crash`, `25-defects-semantic`, `26-quirk-codec-entries`, and the quirk labels inside `01`-`22` |
| Production file | `27-smoke-real-services` |

## 11. Open questions

1. Should the Rust port keep reading the file on every operation (reference) or cache with invalidation on
   updater events? Behaviour is identical for the vectors; caching changes live-update semantics.
2. `ENABLE_SERVICE_UPDATES=OFF` builds skip the updater and cache loading entirely (not covered by vectors).
3. The frontend also reads `services.json` (`recommended.output`, multitrack config); its rules belong to
   the frontend spec.
4. Thread interleavings of the real updater (answer arriving between poll iterations) are modelled only by
   the `updater-route` delay.
