# Phase 1 review (P1-32)

Review file for Phase 1 (leaf libraries). WP-45 (P1-31) created this file
with the vendored-dep scan and deferral record; WP-46 (P1-32) completes the
benchmark, goldens/ABI and parent-tick sections.

## Vendored deps (P1-31)

### Usage scan (P1-31.1)

Pinned tree: `33.0.1-1-g806dd0c` (tag `33.0.1` + D3 commit `806dd0c`, which
adds the `OBS_ENABLE_RUST` option and touches neither dep). Command, run
from the pinned obs-studio tree root:

```sh
grep -rn "blake2\|json11" --include=CMakeLists.txt --include='*.c*' --include='*.cpp' .
```

(The subtask's `*.c*` glob is quoted here to keep it literal; it also matches
`.cmake` files, which is what catches the CMake glue in the output.)

Output, sorted (2026-11-28):

```text
./deps/blake2/CMakeLists.txt:10:set_target_properties(blake2 PROPERTIES FOLDER deps)
./deps/blake2/CMakeLists.txt:13:  add_library(blake2_static OBJECT)
./deps/blake2/CMakeLists.txt:14:  add_library(OBS::blake2_static ALIAS blake2_static)
./deps/blake2/CMakeLists.txt:16:  target_sources(blake2_static PRIVATE src/blake2-impl.h src/blake2b-ref.c PUBLIC src/blake2.h)
./deps/blake2/CMakeLists.txt:18:  target_include_directories(blake2_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
./deps/blake2/CMakeLists.txt:19:  set_target_properties(blake2_static PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
./deps/blake2/CMakeLists.txt:21:  set_target_properties(blake2 PROPERTIES FOLDER deps/blake2)
./deps/blake2/CMakeLists.txt:22:  set_target_properties(blake2_static PROPERTIES FOLDER deps/blake2)
./deps/blake2/CMakeLists.txt:3:add_library(blake2 OBJECT)
./deps/blake2/CMakeLists.txt:4:add_library(OBS::blake2 ALIAS blake2)
./deps/blake2/CMakeLists.txt:6:target_sources(blake2 PRIVATE src/blake2-impl.h src/blake2b-ref.c PUBLIC src/blake2.h)
./deps/blake2/CMakeLists.txt:8:target_include_directories(blake2 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
./deps/blake2/src/blake2b-ref.c:100:  blake2b_param P[1];
./deps/blake2/src/blake2b-ref.c:116:  return blake2b_init_param( S, P );
./deps/blake2/src/blake2b-ref.c:120:int blake2b_init_key( blake2b_state *S, size_t outlen, const void *key, size_t keylen )
./deps/blake2/src/blake2b-ref.c:122:  blake2b_param P[1];
./deps/blake2/src/blake2b-ref.c:13:   https://blake2.net.
./deps/blake2/src/blake2b-ref.c:141:  if( blake2b_init_param( S, P ) < 0 ) return -1;
./deps/blake2/src/blake2b-ref.c:147:    blake2b_update( S, block, BLAKE2B_BLOCKBYTES );
./deps/blake2/src/blake2b-ref.c:155:    a = a + b + m[blake2b_sigma[r][2*i+0]]; \
./deps/blake2/src/blake2b-ref.c:159:    a = a + b + m[blake2b_sigma[r][2*i+1]]; \
./deps/blake2/src/blake2b-ref.c:177:static void blake2b_compress( blake2b_state *S, const uint8_t block[BLAKE2B_BLOCKBYTES] )
./deps/blake2/src/blake2b-ref.c:191:  v[ 8] = blake2b_IV[0];
./deps/blake2/src/blake2b-ref.c:192:  v[ 9] = blake2b_IV[1];
./deps/blake2/src/blake2b-ref.c:193:  v[10] = blake2b_IV[2];
./deps/blake2/src/blake2b-ref.c:194:  v[11] = blake2b_IV[3];
./deps/blake2/src/blake2b-ref.c:195:  v[12] = blake2b_IV[4] ^ S->t[0];
./deps/blake2/src/blake2b-ref.c:196:  v[13] = blake2b_IV[5] ^ S->t[1];
./deps/blake2/src/blake2b-ref.c:197:  v[14] = blake2b_IV[6] ^ S->f[0];
./deps/blake2/src/blake2b-ref.c:198:  v[15] = blake2b_IV[7] ^ S->f[1];
./deps/blake2/src/blake2b-ref.c:20:#include "blake2.h"
./deps/blake2/src/blake2b-ref.c:21:#include "blake2-impl.h"
./deps/blake2/src/blake2b-ref.c:221:int blake2b_update( blake2b_state *S, const void *pin, size_t inlen )
./deps/blake2/src/blake2b-ref.c:232:      blake2b_increment_counter( S, BLAKE2B_BLOCKBYTES );
./deps/blake2/src/blake2b-ref.c:233:      blake2b_compress( S, S->buf ); /* Compress */
./deps/blake2/src/blake2b-ref.c:236:        blake2b_increment_counter(S, BLAKE2B_BLOCKBYTES);
./deps/blake2/src/blake2b-ref.c:237:        blake2b_compress( S, in );
./deps/blake2/src/blake2b-ref.c:23:static const uint64_t blake2b_IV[8] =
./deps/blake2/src/blake2b-ref.c:248:int blake2b_final( blake2b_state *S, void *out, size_t outlen )
./deps/blake2/src/blake2b-ref.c:256:  if( blake2b_is_lastblock( S ) )
./deps/blake2/src/blake2b-ref.c:259:  blake2b_increment_counter( S, S->buflen );
./deps/blake2/src/blake2b-ref.c:260:  blake2b_set_lastblock( S );
./deps/blake2/src/blake2b-ref.c:262:  blake2b_compress( S, S->buf );
./deps/blake2/src/blake2b-ref.c:273:int blake2b( void *out, size_t outlen, const void *in, size_t inlen, const void *key, size_t keylen )
./deps/blake2/src/blake2b-ref.c:275:  blake2b_state S[1];
./deps/blake2/src/blake2b-ref.c:290:    if( blake2b_init_key( S, outlen, key, keylen ) < 0 ) return -1;
./deps/blake2/src/blake2b-ref.c:294:    if( blake2b_init( S, outlen ) < 0 ) return -1;
./deps/blake2/src/blake2b-ref.c:297:  blake2b_update( S, ( const uint8_t * )in, inlen );
./deps/blake2/src/blake2b-ref.c:298:  blake2b_final( S, out, outlen );
./deps/blake2/src/blake2b-ref.c:302:int blake2( void *out, size_t outlen, const void *in, size_t inlen, const void *key, size_t keylen ) {
./deps/blake2/src/blake2b-ref.c:303:  return blake2b(out, outlen, in, inlen, key, keylen);
./deps/blake2/src/blake2b-ref.c:309:  return blake2b( out, BLAKE2B_OUTBYTES, in, inlen, NULL, 0 );
./deps/blake2/src/blake2b-ref.c:315:#include "blake2-kat.h"
./deps/blake2/src/blake2b-ref.c:31:static const uint8_t blake2b_sigma[12][16] =
./deps/blake2/src/blake2b-ref.c:332:    blake2b( hash, BLAKE2B_OUTBYTES, buf, i, key, BLAKE2B_KEYBYTES );
./deps/blake2/src/blake2b-ref.c:334:    if( 0 != memcmp( hash, blake2b_keyed_kat[i], BLAKE2B_OUTBYTES ) )
./deps/blake2/src/blake2b-ref.c:344:      blake2b_state S;
./deps/blake2/src/blake2b-ref.c:349:      if( (err = blake2b_init_key(&S, BLAKE2B_OUTBYTES, key, BLAKE2B_KEYBYTES)) < 0 ) {
./deps/blake2/src/blake2b-ref.c:354:        if ( (err = blake2b_update(&S, p, step)) < 0 ) {
./deps/blake2/src/blake2b-ref.c:360:      if ( (err = blake2b_update(&S, p, mlen)) < 0) {
./deps/blake2/src/blake2b-ref.c:363:      if ( (err = blake2b_final(&S, hash, BLAKE2B_OUTBYTES)) < 0) {
./deps/blake2/src/blake2b-ref.c:367:      if (0 != memcmp(hash, blake2b_keyed_kat[i], BLAKE2B_OUTBYTES)) {
./deps/blake2/src/blake2b-ref.c:48:static void blake2b_set_lastnode( blake2b_state *S )
./deps/blake2/src/blake2b-ref.c:54:static int blake2b_is_lastblock( const blake2b_state *S )
./deps/blake2/src/blake2b-ref.c:59:static void blake2b_set_lastblock( blake2b_state *S )
./deps/blake2/src/blake2b-ref.c:61:  if( S->last_node ) blake2b_set_lastnode( S );
./deps/blake2/src/blake2b-ref.c:66:static void blake2b_increment_counter( blake2b_state *S, const uint64_t inc )
./deps/blake2/src/blake2b-ref.c:72:static void blake2b_init0( blake2b_state *S )
./deps/blake2/src/blake2b-ref.c:75:  memset( S, 0, sizeof( blake2b_state ) );
./deps/blake2/src/blake2b-ref.c:77:  for( i = 0; i < 8; ++i ) S->h[i] = blake2b_IV[i];
./deps/blake2/src/blake2b-ref.c:81:int blake2b_init_param( blake2b_state *S, const blake2b_param *P )
./deps/blake2/src/blake2b-ref.c:86:  blake2b_init0( S );
./deps/blake2/src/blake2b-ref.c:98:int blake2b_init( blake2b_state *S, size_t outlen )
./deps/json11/CMakeLists.txt:14:  target_compile_options(json11 PUBLIC -Wno-unqualified-std-cast-call)
./deps/json11/CMakeLists.txt:17:set_target_properties(json11 PROPERTIES FOLDER deps)
./deps/json11/CMakeLists.txt:3:add_library(json11 OBJECT)
./deps/json11/CMakeLists.txt:4:add_library(OBS::json11 ALIAS json11)
./deps/json11/CMakeLists.txt:6:target_sources(json11 PRIVATE json11.cpp PUBLIC json11.hpp)
./deps/json11/CMakeLists.txt:8:target_include_directories(json11 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
./deps/json11/json11.cpp:171:    void dump(string &out) const override { json11::dump(m_value, out); }
./deps/json11/json11.cpp:22:#include "json11.hpp"
./deps/json11/json11.cpp:30:namespace json11 {
./deps/json11/json11.cpp:747:// Documented in json11.hpp
./deps/json11/json11.cpp:791:} // namespace json11
./frontend/cmake/feature-macos-update.cmake:26:  PRIVATE "$<LINK_LIBRARY:FRAMEWORK,Security.framework>" nlohmann_json::nlohmann_json OBS::blake2
./frontend/cmake/feature-macos-update.cmake:5:if(NOT TARGET OBS::blake2)
./frontend/cmake/feature-macos-update.cmake:6:  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/blake2" "${CMAKE_BINARY_DIR}/deps/blake2")
./frontend/cmake/feature-whatsnew.cmake:12:    if(NOT TARGET OBS::blake2)
./frontend/cmake/feature-whatsnew.cmake:13:      add_subdirectory("${CMAKE_SOURCE_DIR}/deps/blake2" "${CMAKE_BINARY_DIR}/deps/blake2")
./frontend/cmake/feature-whatsnew.cmake:16:    target_link_libraries(obs-studio PRIVATE MbedTLS::mbedtls nlohmann_json::nlohmann_json OBS::blake2)
./frontend/CMakeLists.txt:15:if(NOT TARGET OBS::json11)
./frontend/CMakeLists.txt:16:  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/json11" json11)
./frontend/CMakeLists.txt:43:    OBS::json11
./frontend/cmake/os-windows.cmake:1:if(NOT TARGET OBS::blake2)
./frontend/cmake/os-windows.cmake:2:  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/blake2" "${CMAKE_BINARY_DIR}/deps/blake2")
./frontend/cmake/os-windows.cmake:51:    OBS::blake2
./frontend/dialogs/OBSAbout.cpp:12:using namespace json11;
./frontend/dialogs/OBSAbout.cpp:8:#include <json11.hpp>
./frontend/dialogs/OBSYoutubeActions.cpp:271:	json11::Json broadcasts;
./frontend/dialogs/OBSYoutubeActions.cpp:289:					json11::Json stream;
./frontend/dialogs/OBSYoutubeActions.cpp:437:	json11::Json json;
./frontend/importer/OBSImporter.cpp:192:		json11::Json res;
./frontend/importer/OBSImporter.cpp:195:		if (res != json11::Json()) {
./frontend/importer/OBSImporter.cpp:196:			json11::Json::object out = res.object_items();
./frontend/importer/OBSImporter.cpp:201:				json11::Json::object newOut = out;
./frontend/importer/OBSImporter.cpp:219:			std::string out_str = json11::Json(out).dump();
./frontend/importers/classic.cpp:23:using namespace json11;
./frontend/importers/importers.cpp:22:using namespace json11;
./frontend/importers/sl.cpp:21:using namespace json11;
./frontend/importers/studio.cpp:24:using namespace json11;
./frontend/importers/xsplit.cpp:24:using namespace json11;
./frontend/importers/xsplit.cpp:445:int XSplitImporter::ImportScenes(const string &path, string &name, json11::Json &res)
./frontend/oauth/OAuth.cpp:13:using namespace json11;
./frontend/oauth/OAuth.cpp:9:#include <json11.hpp>
./frontend/oauth/RestreamAuth.cpp:12:#include <json11.hpp>
./frontend/oauth/RestreamAuth.cpp:16:using namespace json11;
./frontend/oauth/TwitchAuth.cpp:17:using namespace json11;
./frontend/updater/CMakeLists.txt:31:  PRIVATE OBS::blake2_static nlohmann_json::nlohmann_json zstd::libzstd_static comctl32 shell32 version winhttp wintrust
./frontend/updater/hash.cpp:49:	blake2b_state blake2;
./frontend/updater/hash.cpp:50:	if (blake2b_init(&blake2, kBlake2HashLength) != 0) {
./frontend/updater/hash.cpp:71:		if (blake2b_update(&blake2, hashBuffer.data(), read) != 0) {
./frontend/updater/hash.cpp:76:	if (blake2b_final(&blake2, hash.data(), hash.size()) != 0) {
./frontend/updater/updater.cpp:507:			blake2b(dataHash.data(), dataHash.size(), buf.data(), buf.size(), nullptr, 0);
./frontend/updater/updater.cpp:885:	blake2b(hash.data(), hash.size(), junk, sizeof(junk), nullptr, 0);
./frontend/updater/updater.cpp:967:	blake2b(hash.data(), hash.size(), junk, sizeof(junk), nullptr, 0);
./frontend/utility/WhatsNewInfoThread.cpp:103:		if (blake2b_update(&blake2, &buf, read) != 0) {
./frontend/utility/WhatsNewInfoThread.cpp:10:#include <blake2.h>
./frontend/utility/WhatsNewInfoThread.cpp:111:	if (blake2b_final(&blake2, hash, BLAKE2_HASH_LENGTH) != 0) {
./frontend/utility/WhatsNewInfoThread.cpp:88:	blake2b_state blake2;
./frontend/utility/WhatsNewInfoThread.cpp:89:	if (blake2b_init(&blake2, BLAKE2_HASH_LENGTH) != 0) {
./frontend/utility/YoutubeApiWrappers.cpp:15:using namespace json11;
./frontend/utility/YoutubeApiWrappers.cpp:445:bool YoutubeApiWrappers::ResetBroadcast(const QString &broadcast_id, json11::Json &json_out)
./frontend/utility/YoutubeApiWrappers.cpp:494:bool YoutubeApiWrappers::FindBroadcast(const QString &id, json11::Json &json_out)
./frontend/utility/YoutubeApiWrappers.cpp:515:bool YoutubeApiWrappers::FindStream(const QString &id, json11::Json &json_out)
./frontend/widgets/OBSBasic_Browser.cpp:24:#include <json11.hpp>
./frontend/widgets/OBSBasic_Browser.cpp:29:using namespace json11;
./frontend/widgets/OBSBasic_YouTube.cpp:71:	json11::Json json;
```

### Result

- **`deps/blake2` users — all frontend:** `frontend/updater/hash.cpp` (81
  lines — the primary user named in the subtask), `frontend/updater/updater.cpp`
  (file-integrity + self-test `blake2b()` calls),
  `frontend/utility/WhatsNewInfoThread.cpp` (WhatsNew URL hash), plus the CMake
  glue: `frontend/updater/CMakeLists.txt` (`OBS::blake2_static`),
  `frontend/cmake/os-windows.cmake`, `frontend/cmake/feature-macos-update.cmake`,
  `frontend/cmake/feature-whatsnew.cmake` (`OBS::blake2`).
- **`deps/json11` users — all frontend:** OAuth —
  `frontend/oauth/{OAuth,RestreamAuth,TwitchAuth}.cpp`; importer —
  `frontend/importers/{classic,importers,sl,studio,xsplit}.cpp` +
  `frontend/importer/OBSImporter.cpp`; additionally
  `frontend/dialogs/OBSAbout.cpp`, `frontend/dialogs/OBSYoutubeActions.cpp`,
  `frontend/utility/YoutubeApiWrappers.cpp`,
  `frontend/widgets/OBSBasic_Browser.cpp`, `frontend/widgets/OBSBasic_YouTube.cpp`;
  CMake: `frontend/CMakeLists.txt` (`add_subdirectory(deps/json11)` +
  `OBS::json11` link).
- **Nothing in `libobs/`** (and none in `shared/`, `plugins/`, `libobs-*`):
  the Done-when holds.
- The subtask text names `frontend/updater/hash.cpp` (blake2) and the frontend
  OAuth/importer code (json11); the scan confirms both and adds the further
  frontend users listed above. All consumers are still frontend, so the
  Done-when ("nothing in `libobs/`") is met.

### Deletion deferral (P1-31.2 → P7-04)

**Deferred: `deps/blake2` and `deps/json11` are NOT deleted in Phase 1.**
Both consumer sets are still C code:

- `deps/blake2` ← **frontend/updater** (P6-32): `updater/hash.cpp` +
  `updater/updater.cpp`; and the WhatsNew feature:
  `frontend/utility/WhatsNewInfoThread.cpp` with the
  `feature-whatsnew.cmake` / `feature-macos-update.cmake` / `os-windows.cmake`
  CMake glue.
- `deps/json11` ← **frontend OAuth/importer**: `frontend/oauth/`
  (Twitch/Restream auth) + `frontend/importers/` +
  `frontend/importer/OBSImporter.cpp`; plus the further frontend consumers
  above (About/YouTube/Browser).

**P7-04 ("Delete vendored deps") deletes both** once the Phase 6 ports remove
these include/link references; P7-04's Done-when ("no CMake/CI references
remain") covers the CMake glue listed in the scan output.

## Benchmarks (P1-32.1)

_TBD — WP-46: benchmarks vs the baseline table (`docs/rust-rewrite/benchmarks.md`)._

## Goldens/ABI on 3 OSes (P1-32.2)

_TBD — WP-46._

## Parent ticks (P1-32.3)

_TBD — WP-46: tick the parent tasks in `docs/rust-rewrite/phase-1-leaf-libraries.md` and update estimates._
