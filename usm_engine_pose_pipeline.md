# USM engine pose pipeline — binary audit

This document anchors the model/skeleton/animation implementation to the retail `USM.exe` rather than to assumptions made from exported data.

## Exactness contract

- Reference image: `USM.exe`, IDA image base `0x00400000`.
- Reference database: `C:\Users\pwd12\OneDrive\Documents\Ultimate Spider-Man\USM.exe.i64`.
- Every function used as an implementation authority is identified below by virtual address, exact IDA function length, and SHA-256 of its original machine-code bytes.
- The C++ is an instruction-audited semantic transcription. It is not claimed to recompile to the same machine-code bytes: that would require the original compiler, flags, libraries, linker, and complete binary layout. “Byte exact” here means that serialized input boundaries are exact, every byte is retained, and every documented field/loop/mask/constant is checked against the disassembly rather than inferred from Hex-Rays output.
- `NalSkeletonData`, `NalComponentData`, and animation components retain their original byte blocks alongside parsed views. Alignment and reserved bytes are never silently discarded or assigned a guessed meaning.

If any hash below differs, the associated transcription must be treated as unaudited until its instructions are checked again.

## Corrected pipeline facts

### Time, interpolation, and matrices

- `sub_5FF1A0` reads current time from character animation offset `+0x54` and multiplies it by global `flt_96A698`; it does not read the scale field at `+0x38`.
- `sub_5F06B0` uses the animation float at `+0x38` as the playback time span. The unrelated float at `+0x30` is preserved as an unknown header value and is no longer displayed or consumed as duration.
- Quaternion snapshots are interpolated with shortest-path normalized slerp. Position and scalar snapshots use linear interpolation.
- `sub_5FC9C0` emits the engine's row-major quaternion matrix. `sub_5FE000(child, parent)` composes row-vector matrices as `child * parent`; the column-major application therefore calls `parent * local`.
- `sub_5F2FD0` is an ordinary X-axis rotation. The former hard-coded 90-degree bias was not present in the executable and has been removed.

### Core character components

- Torso/head (`sub_5F6610`) consumes five animated quaternions plus pelvis translation. Neck and auxiliary neck are siblings under spine2.
- Standard legs (`sub_5F6960`) consume eight `vec3` offsets, eight proximal-to-distal bone indices, and one shared root. This layout is distinct from the toe-first IK layout in `sub_5F6DA0`.
- Standard arms (`sub_5F7160`) consume eight quaternions/offsets, bones 36–43, four forearm-twist bones, and the neck parent. The retail normal path sees `byte_959561 == 0` and constructs both serial twist nodes with angle zero. The `sub_5F4960` extraction and exact `0.33f` factor run only while two special call sites temporarily set that byte.
- IK arms follow `sub_5F7760`; their hand positions, hand rotations, elbow spins, twist bones, and parent matrices remain separate from the standard-arm representation. `sub_5F16E0` calls `sub_5EEEE0`/`sub_5EF100` with the clavicle matrix as callback parameter 2 and hand target as parameter 3; both callbacks read parameter 2 for the bend plane.
- Fakeroot follows `sub_5F62E0`, including the fixed nine-track base layout, quaternion root, position/floor vector, and signal fields.

### ArbitraryPO

- The per-skeleton header is eight dwords: node count, metadata, skeleton-quaternion offset, skeleton-position offset, 48-byte node-record offset, and evaluation-order offset. Zero offsets are null.
- Node records store quaternion index at `+0x20`, position index at `+0x22`, output matrix at `+0x24`, parent matrix at `+0x26`, quaternion-animation flag at `+0x28`, position-animation flag at `+0x2A`, and a reserved dword at `+0x2C`.
- The animation/default pose header consumes quaternion count in word 0 and total channel count in word 1. Words 2–3 are reserved/stale storage in retail files; they are retained but never treated as transforms. Quaternions begin at `+0x10`, followed by positions.
- `sub_5F2270` treats the per-animation channel mask as a variable-length `uint32` bitset with `ceil((quatCount + positionCount) / 32)` words. It counts enabled quaternion channels first, then enabled total channels. Venom has 41 logical channels and therefore serializes two mask words; the second word is not codec data.
- `sub_5F3C00` integrates a compact stream of `3 * enabledChannelCount` tracks. The first `3 * enabledQuaternionCount` tracks are quaternion triples; the remainder are position triples. There is no 16- or 32-channel evaluator cap.
- Matrix evaluation uses the serialized evaluation-order array. Constant channels fall back to the skeleton/default constants rather than to identity/zero.

### Finger families

- Top2 allocation/counting follows `sub_5FDA40`/`sub_5FDA60`: mask bits 0–1 each own three tracks, bits 2–9 own two, and bits 10–29 own one. The pose loop itself consumes bits 0–19; distal joints are procedural.
- Individual Curl follows `sub_5FDB60`: bits 0–1 each own one quaternion plus one scalar, and bits 2–9 each own two scalars.
- Reduced Angular follows `sub_5FDD50`, `sub_600ED0`, `sub_6010E0`, and `sub_5F9150`. Its serialized bone table is ten chain-major triples while its offset/default-pose slots are base/middle/tip-major. The parser now performs that exact remap.
- Reduced default pose is exactly 176 bytes: two quaternions, eight base-Z scalars, eight base-Y scalars, ten middle scalars, and ten tip scalars.
- Full Rotational follows `sub_5FDE30`, `sub_601280`, `sub_601470`, and `sub_5F9430`: three quaternion tracks for every enabled one of 30 chain-major joints.
- Tentacles Compressed is not a bone-table component. It owns 15 scalar channels and a 15-float default pose. The channels are five `(base diameter, subtentacle activity, pull factor)` triplets ordered UpLeft, UpRight, LowLeft, LowRight, and Tongue; OpenUSM's `TentaclesPoseDesc` lookup methods confirm the mapping.
- Venom and Carnage store their four six-point tendril centerlines (plus a six-point tongue) as ArbitraryPO nodes outside the PCM/ENTITY skin palette. The preview therefore keeps controller-only logical matrices alive and evaluates them in serialized order. The renderer then applies the retail descriptor roles found in the character packs: the tongue is a narrow, flattened, pointed `shaded_tongue` profile, while Carnage's `shaded_tentacles` are thin symbiote blades rather than round tubes. A zero diameter hides the associated appendage; this is why `vententa_1` is invisible at its first/last frames but extends during the middle of the clip.
- Character PCM sections use the 0x54-byte `USPersonMaterial`, not PCUV baked vertex colour. The viewer now preserves both texture names, the RGBA material colour, the lighting switch, and the environment/ink flags. Its shader follows OpenUSM's `3_VS` equation (`c7 * saturate(dot(normal,c6)) + c8`) and `7_PS` sphere-map combine (`lit * ink.a + ink.rgb`). OpenUSM builds the sphere coordinates from `LocalToWorld * WorldToView`, so the preview likewise transforms the skinned normal into view space; Venom's purple/black ink pattern therefore moves as the model view is rotated.
- OpenUSM resolves an authored texture name as `.IFL` before `.DDS`. The preview now parses those text timelines, strips the authored frame extension, preserves repeated entries as frame holds, resolves the PC texture payloads, and advances them at the retail 30 Hz scene rate. It also honors `USPersonMaterial::m_blend_mode` at `+0x4C`, allowing the frames' alpha channel to mask the eye mesh instead of exposing its black backing polygon. This restores the animated `venom_eye` and `venom_s_eye` surfaces. Carnage's `carnage_03` eyes intentionally have no IFL; they remain a static diffuse texture combined with the view-dependent `us_char_sphrmap_ink_15b` material.

### Serialized block boundaries

- Default-pose offsets are component-indexed, not physically sorted. A block ends at the smallest offset greater than its own. Carnage proves this with offsets `(32, 144, 240, 336, 528, 1168, 576)`; using the next table entry merged Tentacles into ArbitraryPO/Fakeroot.
- Carnage's physical default blocks are exactly `112, 96, 96, 192, 48, 60, 592` bytes. Known alignment/reserved tails are retained in `raw_default_pose_block`.
- Carnage component blocks are: torso `128` bytes (`116` consumed + `12` zero alignment), legs IK `192` (`180 + 12`), arms IK `272` (`248 + 24`), Top2 `496` (`488 + 8`), Fakeroot `0`, Tentacles `0`, and ArbitraryPO `2512` (`2504 + 8`).
- PCANIM track-offset tables define the exact serialized entropy-stream extent. Runtime allocation-size helpers do not define the on-disk stream length; using them truncated long clips and caused residual velocity to accumulate beyond the supplied bytes.

### Resource association, compatible rigs, and mesh bone maps

- Pack-directory resource types are advisory. Animation discovery supplements the typed directory list with a bounded `0x10101` PCANIM signature check over every directory resource. This recovered 262 animation records that were present in the retail packs but absent from the former UI list.
- PCANIM skeleton references use the skeleton header's logical-name hash. Most character skeletons reuse the directory resource hash, but Eddie Brock's `0x10200` generic skeleton does not. Both exact hashes now alias the same parsed skeleton, so `eb_idl` resolves to `eddie_brock` instead of disappearing from the animation list.
- Rig compatibility is structural rather than pointer-identical: kind, component records, component bone/other-matrix indices, ArbitraryPO output/parent topology, evaluation order, and the final logical parent map must match. Per-clip channel-source indices and animated/constant flags may differ because the animation is evaluated with its own referenced skeleton. Boomerang and `gang_ss` satisfy this contract, exposing 62 valid clips for the Boomerang mesh.
- A negative serialized frame count is the engine's empty-animation sentinel, not a one-frame clip. Zero-frame records also occur inside cutscene/controller resources and have no playable samples. The parser retains the complete container bytes, while the UI/export layer omits both forms from playable clips. `gss_webblindloop` is the retail negative-sentinel example.
- Entity `rel_po_idx` mappings occupy a sparse logical index space and therefore are not required to form a dense permutation of the mesh-pose count. Nick Fury has 59 mesh poses in a 60-slot logical space with slot 57 absent; his left prop hand is logical 59 and right prop hand is logical 58. Preserving the hole prevents every later twist/leg palette entry from shifting by one.
- Live PCM loading constructs the skeleton and logical palette before resolving section bone indices. Resolving sections against an empty or previous model's palette was the remaining live-view-only cause of otherwise correct exported poses stretching or spinning.

### PCMORPH and viseme playback

- A retail `.PCMORPH` is a normal `PCM 0x601` image whose item type is 3. Its 20-byte root contains the name pointer, target-set count, target-set pointer, owner pointer, and an extra pointer. Each target set is 12 bytes: kind, mesh-section count, and section-record pointer.
- Every morph section record is exactly `0x88` bytes: vertex count at `+0x00`, channel mask at `+0x04`, then 32 relocated stream pointers at `+0x08`. Section ordinals match the owning PCM LOD exactly; names and materials are not used for association.
- The position stream consumed by `sub_4140D0` is a chain of `uint16 changedCount`, `uint16 skippedCount`, then `changedCount` packed float3 values. The destination cursor advances through the changed values and following skip; blocks continue while `skippedCount != 0`. A zero changed count is valid and represents an unchanged prefix.
- Target set zero has kind 2 and stores neutral absolute positions. Its position bits match the owning PCM vertices exactly. Later kind-1 sets store sparse deltas, which the renderer accumulates from the neutral PCM position with the supplied weight.
- Resource-key type 50 is the retail viseme stream. Its 24-byte header stores channel count, format value, frame count, sample rate, an as-yet-unconsumed float at `+0x10`, and the serialized data pointer. The payload is exactly `frameCount * channelCount` frame-major floats beginning at `+0x18`; there is no padding or trailing data.
- `sub_6C0EE0` selects `floor(sampleRate * time)` with no inter-frame interpolation. Every non-zero channel `i` addresses PCMORPH target set `i + 1`. `sub_6C6400` advances time and releases the stream when the selected frame reaches the declared frame count; the preview returns the weights to neutral at the same boundary.
- The generic `USMMorph` 12-byte property is a separate direct-control path: bytes 0-5 are target-set indices terminated by `0xFF`, and bytes 6-11 are weights scaled by the retail `1/255` constant in `sub_7385D0`. This is not substituted for a viseme stream.
- The standalone Venom and Carnage viewer packs contain neither PCMORPH nor viseme resources. Their ordinary skeletal clips must remain skeletal-only. Morph/viseme resources are present on cutscene assets such as `VENOM_EDDIE`, Eddie Brock, Sable, Nick Fury, Mary Jane, and Peter Parker; the browser exposes only data that the selected pack/model actually supplies.

## Function-byte ledger

Hashes cover `[function_start, function_end)` exactly as defined in the IDB.

### Shared math and core pose construction

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5F2FD0` | 176 | `7cb1f26f7827288f6b0410335844d87cce7007ec4c97c1df474235dfa9437b51` | X rotation |
| `sub_5F06B0` | 394 | `a03a96b0aad1da78460e7e409c5e93b182fe028400a1eacaf8ba5ec1717d291f` | frame selection, loop wrap, interpolation fraction |
| `sub_5F16E0` | 715 | `f2fb1255f0ea0704c4f50487d97907757c7557ddd50ad7fd5b9000099beb3216` | two-bone IK driver |
| `sub_5F4960` | 899 | `f3cc78951b5211c5123bf3f597e1d53dd1e16687167b5cc8aee5375afd25bbfe` | twist extraction |
| `sub_5EEEE0` | 530 | `e648ee8ed419d323ab982efbb2f5029c8d11408412eb8a2cedb35a0ef08a4b32` | left-arm IK plane callback |
| `sub_5EF100` | 562 | `3df5da61b9b47975fe5c8cae467eccd08c0aa831ecf4e4237246bed48fece6dd` | right-arm IK plane callback |
| `sub_5FC9C0` | 154 | `2215ef804215416896e9e86850df40fffd5b7a8edf6a419f3ecf30c131b57c29` | quaternion to matrix |
| `sub_5FE000` | 664 | `466c26adfcf764839617f912f00ccad93dcaf12cb5075e168f0fd4734c26f553` | matrix composition |
| `sub_5F6610` | 623 | `03d818ecf69b30b9c3e5ec7d03d142de4772b8a273e819eb35fc48ebfb6ebcb8` | torso/head matrices |
| `sub_5F6960` | 300 | `93d524b512b1379eedb745d13fbc3293300873586565eeaddbad30075cb2a369` | standard legs matrices |
| `sub_5F6DA0` | 735 | `77e1a15c45eabfdd3a7b47264850445b4bc5ab32e21a0c13bfb38ff81e8233f6` | IK legs matrices |
| `sub_5F7160` | 739 | `ac2252287ab738fa6218a651ab0b969f63d2b92a14c193a6d22b5c422eb8a8c4` | standard arms matrices |
| `sub_5F7760` | 1274 | `804da040740ed2a6783cdcb553b2dd10758d49a3823650a4156df6037a9b30b2` | IK arms matrices |
| `sub_5F62E0` | 218 | `fdf7ec0ffc9a1a5713f933af50c06ccf1ca80efb50662c67a34a18a681ecd84e` | fakeroot matrices |

### PCMORPH and viseme playback

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_778840` | 268 | `515172796528cf76d45ddfaf077f69c9fecccf7adbdbf73c364496f37e4704ed` | PCMORPH relocation and section-record layout |
| `sub_4140D0` | 642 | `c50dd8f1ee53cbbb2d1c78eaf075a5315606ec324c0c1692c5c625fb2532c8f8` | sparse position-run application |
| `sub_408350` | 153 | `b57afdc6acfede422cfde41f0803c88d39442b180e0111c5e9ca71bb3516f075` | `USPersonMorphable` section apply wrapper |
| `sub_778950` | 56 | `e7903b8cc3c77184d09ead6feb9d7eac9f9f5dcc6ffb10a6506c8cbca6cd47f4` | target-set section dispatch |
| `sub_7789B0` | 323 | `73df6b198bfd6d3928b0052335d30ebe2d4aeb5f1215f7b501f7952d6750e5dd` | weighted target application across mesh sections |
| `sub_6C0A60` | 352 | `f30cc7ccaf91093092be2844699efcd4fa655c7963e7cacf8a43f755f55a4a39` | type-50 viseme resource load/fixup |
| `sub_6C0EE0` | 378 | `533b2eb77fb2134e2f889eaa535804bd495b46883aeaa0acc3de5f8595a8aa1e` | exact frame selection and channel-to-target mapping |
| `sub_6C6400` | 373 | `6839889032b2d24b583ef93f37504fde9ea4edc7cbc1312064f47b32136cf237` | viseme time advance and end-of-stream behavior |
| `sub_7385D0` | 717 | `7ea55351fc8b56c8c48acc2189754ef62253f4a47508200657b76fff7d07d716` | direct six-target `USMMorph` property application |

### Torso, eight-quaternion components, and fakeroot codecs

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FF870` | 357 | `c6b43ac1b4e48e5a45a8b08a966267e9b2188b4b0dfba358c1b2b067c8c0c046` | torso integration |
| `sub_5FF700` | 363 | `2f9187816ffb24e864e65b92e46fa5ea6cad8129d3656df7426bb639f5a510cb` | torso snapshot |
| `sub_5F64D0` | 305 | `f946542f942edd7942edd940718daa8a1ea970ec00693ecd791c55464fbd9d44` | torso pose build |
| `sub_5FF5B0` | 324 | `45da9309b8d6b7a75741b838040604ee70e56be6178e9af9b17787ba932b4cf5` | torso pose interpolation |
| `sub_5FFC10` | 175 | `c40cc07e9809ea4023e7447aaf0a027566107182f65f8a02c69be74234891301` | eight-quaternion integration |
| `sub_5FFB70` | 156 | `65d37d6c3ec449bead14131e44f0f0ef43a9211ff5ec12e04edc5c58e967dbf1` | eight-quaternion snapshot |
| `sub_5F68E0` | 116 | `a1e3fc19d7a783fb485d4166d5799d96139965d7761a4e245876baae3faab396` | eight-quaternion pose build |
| `sub_5FFA20` | 332 | `3c3a1e25e73500d3b7a6006d057c34b71cfec287b08452f3a0bca9adff6f73ae` | eight-quaternion interpolation |
| `sub_5FE980` | 1885 | `91171a6f41f273f6bd69669896a34108099554891db19f8237a1cf56ce7cee07` | fakeroot pose evaluation |
| `sub_5FF1A0` | 265 | `3df8b5ad1b54bdf29b36c4c9322369cd1e02b68bb5446c1b63a8b8f803490f20` | fakeroot integration/time source |
| `sub_5FF0E0` | 182 | `2a874442b612047909402d4bd87cf7845f7cebd5fa17464e6c6b317bc9bc3da3` | fakeroot snapshot |

### ArbitraryPO

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5F2270` | 870 | `f37ec1777ac6e4b5e1715ad5e316ce3b95fed8818f67279753b125be94aa3357` | variable mask sizing and active-channel counts |
| `sub_5F3C00` | 313 | `50db58f2220e80302c2f229cee9a185a16db3eb79c5ff4b1b63e818677ddb026` | compact quaternion/position integration |
| `sub_5F3D40` | 225 | `b803c2f4e9e862f6b0aff8b8ad29650a23ebecef5a3e55e00697ba7ffa2ab0ff` | snapshot |
| `sub_5F6000` | 292 | `669f328f54f01e74e43b0b8fca0898952f6f6e9c6ce847c1e127cedca7d4155a` | masked interpolation |
| `sub_5F6130` | 216 | `2ee2aa39243e52d977389d5041e265465f05fa20cdd40bc823f95b12c92dff9c` | unmasked interpolation |
| `sub_5F5E60` | 416 | `44896934e8752809d1d35d7c32d1e4ceeae64d99ccc4047db5908f46c7000b60` | node/matrix build |
| `sub_5F98E0` | 316 | `765df14537cc54b3c4dc4cc4ff2237b850b07f84df464eb698a8dd9191de7d11` | pose evaluation |
| `sub_5F9A20` | 49 | `ddf5a4fb8b04f086e131457e58c06dd43dc02a21c9264afdddc8528c654d6fe4` | interpolation wrapper |

### Tentacles Compressed codec

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FC0A0` | 102 | `f05d080f20725d8e985f63a9257b4774b22cdc5f5e0405f01503e0cc0c8bb98a` | constructor |
| `sub_5FD8F0` | 115 | `2613cf07550a302b342e89526c5ca9e73ddb4342afd4035f623dfd3d2d9361d1` | allocation sizing |
| `sub_5FE560` | 60 | `915a83063b9ff98fb675c6c376cc6dd1a61b197beb7a00ffeb15c8407a21942e` | pose wrapper |
| `sub_5FE5A0` | 583 | `5f33d1282cbf907ce686a133bc6687337e7ea696a8b8cb542bbfc74d13e0f111` | pose evaluation |
| `sub_5FE7F0` | 167 | `cffbfcd6e4fc0a1eee9b6dced4d9d5a6b8677d1d00fa0e5193f5b805ece76f20` | scalar integration |
| `sub_5FC140` | 8 | `f21f6f699535ecd2dd1176be581320481c314d93fefaa86c509264ac63043473` | interpolation thunk |
| `sub_5EFFF0` | 150 | `9038d115f8e1bc149bff7f699679d924e3fde7ff12bcc2b9d43c0f3d892b0526` | scalar interpolation |

### Top2 Knuckle Curl

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FC1A0` | 102 | `9cb44fa88479781a0db1efd7b170260361c3acbc3737562d821f0065f60cf51a` | constructor |
| `sub_5FDA40` | 24 | `b28c4e69e628c1c8ed8e75f671da4456b7bb158b8b43f6e59af770c598e6b2a3` | allocation sizing |
| `sub_5FDA60` | 58 | `1208fa32030fdbd4e8f53544ac1994e75911f7773441b9332f6921c3b95815ac` | active-track count |
| `sub_6006F0` | 60 | `915a83063b9ff98fb675c6c376cc6dd1a61b197beb7a00ffeb15c8407a21942e` | pose wrapper |
| `sub_600730` | 333 | `badd4d536848382d80c9a4d08840419596a13f15b4f27bbe07d4a3bd21c0e662` | pose evaluation |
| `sub_600940` | 342 | `6e446cc1d6b40a65df6d8b73f1cb0b476bd1d92dfbc5156ad008080031f61281` | mixed-track integration |
| `sub_600880` | 190 | `fa931378b6b1615f64c30ffef20eab7af86cb733b57c19c21553284a5c0d3b43` | snapshot |
| `sub_5FE8A0` | 8 | `b728438b38166df3c5a070b54e1d9faa2c12e09c027a22a7db88abcf54c0c00e` | matrix-build thunk |
| `sub_5F8230` | 519 | `c5448a9e73ece386b5aa0dc2d2e72fa3e978b572f1772056a7f8147c09a012b1` | finger matrices |
| `sub_5FE8B0` | 8 | `dcdf8dc567dffe5c9e2a4c69fa9ba2a073626f3fc86975f8929b6f692817a86f` | interpolation thunk |
| `sub_5F7C60` | 593 | `de8bd808307309085bb8f6741643c55b8ce27298faac5f07e0f0e1c30f935c43` | pose interpolation |

### Individual Curl

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FC270` | 102 | `4a94826a44859d33652a994b263fb148565735866db506eda08701215d0f011e` | constructor |
| `sub_5FDB60` | 182 | `785168cf414b0d44216388d2b8f87739db0542f30404687bc2f1744281f9c10a` | allocation sizing |
| `sub_600AA0` | 60 | `915a83063b9ff98fb675c6c376cc6dd1a61b197beb7a00ffeb15c8407a21942e` | pose wrapper |
| `sub_600AE0` | 324 | `b9175ce848571da8104669fabe74a169641bfaa617bf6397f0e1abd85c5c7472` | pose evaluation |
| `sub_600D00` | 390 | `ff45eacd6a59cb31998f03c33dfab7f9398852daede2c5f80b13791fdb68bf58` | mixed-track integration |
| `sub_600C30` | 204 | `ba897cc8fc7212b51b76d29c63ddbdc32cf69ef91b362477e5593c0a56f8eb4a` | snapshot |
| `sub_5FE8C0` | 8 | `6f0b4862991156b0ae18c86f1cdeab1a978e5aad92a36c1fa15fb3647caad838` | matrix-build thunk |
| `sub_5F88C0` | 627 | `2e1b54650371d8d9a6cf54986a6ba638e2a774f3828ca70a8a4d826879c6a7ad` | finger matrices |
| `sub_5FE8D0` | 8 | `8669a66a38d39500b09b04a0380fa1bd38c771c0a4368f63f418fb4b3ff6bfea` | interpolation thunk |
| `sub_5F8440` | 439 | `07a3ca896be288f4b3cb876c09ad8a4ef3b0ed260853aabe8322121e40b78973` | pose interpolation |

### Reduced Angular

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FC2F0` | 102 | `fb1beac4b3175eaf9f02cf84afa289657c5f89e9d8a5163e3a7e8a432a6e266d` | constructor |
| `sub_5FDD50` | 24 | `4c8b2f7de8f09d4d08b5a28dffc9bbd3855801288b6af63e03bf8f65781b018a` | allocation sizing |
| `sub_600E90` | 60 | `915a83063b9ff98fb675c6c376cc6dd1a61b197beb7a00ffeb15c8407a21942e` | pose wrapper |
| `sub_600ED0` | 333 | `7b9307cafa5a9d9b47ad60e661ae8fbe67a1450a68a9c064ede24604a5729ef3` | pose evaluation |
| `sub_6010E0` | 342 | `1539b1b6559830b26a9321fa77a1ebe12ad2a1222e3f4681a1151d5f7b95aa4f` | mixed-track integration |
| `sub_601020` | 190 | `a20550c849565ef95b0db79c01045d5cfc10ebb2c6f3ff9f23ba3d4d24158a2a` | snapshot |
| `sub_5FE8E0` | 8 | `d3f9ee62971caae0f951d82826d1f362af188d1381c8861c3858987df5294440` | matrix-build thunk |
| `sub_5F9150` | 502 | `6cf65d72ff2c69d18c0887c1c47c691bd6d257a6b369f205fb3b48e0b373bde7` | finger matrices |
| `sub_5FE8F0` | 8 | `af02559afb11d1b73444a99ed1534d932500ef5c51b0000d867b0779ec391431` | interpolation thunk |
| `sub_5F8B40` | 630 | `33d071a821c5330b71a964c287f53667beaacc4d11a84b444edda2a982833811` | pose interpolation |

### Full Rotational

| Function | Bytes | SHA-256 | Audited role |
|---|---:|---|---|
| `sub_5FC3D0` | 102 | `c849dbbc81382c6f029c626782f3f6f2a0b454d826ce0bcb059fb040efe08f89` | constructor |
| `sub_5FDE30` | 140 | `1daa6ba1a4f8a704e08d09024ebe7e8f620fcfdde89303d33c64280d1d25059e` | allocation sizing |
| `sub_601240` | 60 | `915a83063b9ff98fb675c6c376cc6dd1a61b197beb7a00ffeb15c8407a21942e` | pose wrapper |
| `sub_601280` | 332 | `14dc3dd5ade404faee2a98f6dd215fad7ff6dfa4a81721e5924b8a3f8487f942` | pose evaluation |
| `sub_601470` | 175 | `386e7878f5247a65a075c807cf1853af7efaec70e9b13baeea238474fd345b01` | quaternion integration |
| `sub_6013D0` | 156 | `0bea06d23b37e6509194a40647e54675161e0a65b7716daeb1316727cef7f302` | snapshot |
| `sub_5FE900` | 8 | `5755346bd1252f69479963d99020e80d6fe225475a07e89271fb89ec73776e88` | matrix-build thunk |
| `sub_5F9430` | 405 | `eb786806b0afa5e02537f470b6b471e836bd42744db2cb0fc581452fa4f44297` | finger matrices |
| `sub_5FE910` | 8 | `49b9a69d764a08ae5d3ad0b07f4633536a9ddb72340e7c68ed0e127f878ae602` | interpolation thunk |
| `sub_5F9350` | 96 | `2eb36a422585c503819c729800d4d6fda8282dbce26e9f9419cac8dd46fb8965` | pose interpolation |

## Retail-data verification

The complete installed PC pack directory contains 618 `.PCPACK` files. A raw type-ID scan found Top2 Knuckle Curl in 194 packs and found no occurrences of Individual Curl, Reduced Angular, or Full Rotational. Those three variants are present in `USM.exe` and are instruction-audited above, but this retail dataset cannot provide a character-asset golden test for them.

The post-fix headless corpus run completed with zero failures: 618 packs, 30,501 resources, 733 skeletons, 11,259 playable animation records, 62,261 components, 2,207,439 decoded frames, 43,096,496 decoded values, and 4,123 PCM resources (3,846 renderable conversions plus 277 non-renderable helper/collision PCMs). It reported three retained-data warnings and no decode, bounds, non-finite, or conversion failures.

The morph/viseme corpus audit covers all 618 packs: 80 PCMORPH files, 2,318 target sets, 30,763 section records, 5,225 position streams, 791,171 changed vertices, and 55,753 set-zero vertices compared bit-for-bit with their PCM positions. It also decodes all 185 viseme streams, 18,164 frames, and 272,460 float weights. Both parsers report zero failures. The targeted playback regression proves frame 0 and frame 1 weights are copied bit-for-bit to sets 1-15 and that end-of-stream returns to neutral. A hidden OpenGL regression applies a real Eddie frame, observes changed CPU vertices, and reads the uploaded VBO positions back bit-identically with no GL error.

After omitting negative sentinels and zero-frame controller records, the current animation scan exposes 10,953 positive-frame clips and reports zero character-component decode failures. Thirty-five generic vehicle, door, page-camera, or similar gameplay-controller clips retain warnings because their skeletons intentionally have no renderable matrix payload (two also retain controller/event codec warnings); they are not counted as model-render failures.

Targeted regressions also pass: Sable's idle belt authored seam is `0.03210` radians versus a `0.01988`-radian median step; Venom's jaw seam is `0.01113` radians versus a `0.00397`-radian median and `0.00870`-radian maximum ordinary step. Before the variable-mask correction those seams were `2.3826` and `1.074` radians respectively. Sable's two forearm-twist segments retain constant lengths through all 40 idle frames, and the CPU-skinned idle pose is contiguous.

Nick Fury's exported 30-frame idle now remains bounded between approximately `[-0.425, -1.241, -0.323]` and `[0.411, 0.916, 0.192]`; the former coat/prop spike is absent. Three-point samples of all 62 Boomerang clips remain below a `2.55`-unit maximum model span, with no clip exceeding 3 units. Sequential canonical-executable regressions report Eddie Brock's `eb_idl`, 62 Boomerang clips, and Nick Fury's `nf_idl` on their intended skeletons.

Fresh headless exports after the final parser/codec changes produced:

| Asset | Animations | Channels | Keys | Rotation keys | Non-finite values | Non-monotonic time pairs | Quaternion norm range |
|---|---:|---:|---:|---:|---:|---:|---|
| Beetle | 70 | 7,686 | 222,627 | 111,375 | 0 | 0 | `0.9999999148–1.0000001649` |
| Carnage | 52 | 4,989 | 170,653 | 85,344 | 0 | 0 | `0.9999999081–1.0000001661` |

The Debug x64 build completes successfully. The only diagnostics are the existing `size_t -> int` warning in `Interface.cpp` and `double -> float` warning in `Model.cpp`.

## Reproducing the byte hashes

Run the following in the connected IDA database, replacing the list with any newly audited function addresses:

```python
import hashlib
import ida_bytes
import ida_funcs

for ea in addresses:
    function = ida_funcs.get_func(ea)
    data = ida_bytes.get_bytes(function.start_ea, function.end_ea - function.start_ea)
    print(f"{function.start_ea:08X} {len(data)} {hashlib.sha256(data).hexdigest()}")
```
