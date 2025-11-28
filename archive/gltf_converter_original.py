#!/usr/bin/env python3
"""
pcpack_to_gltf.py

Independent script to unpack .pcpack archives and generate glTF files with UVs, materials, textures,
and skinning information, preserving submeshes as separate glTF Meshes and Nodes,
with improved per-submesh texture assignment and fallbacks.

This was the original version that is no longer used. It's included in the archive for me to refer back to in case I need it at any point, particularly surrounding textures.
"""
import sys
import struct
from pathlib import Path
import argparse
import json
import numpy as np
from pygltflib import GLTF2, Buffer, BufferView, Accessor, Mesh, Primitive, Attributes, Scene, Node, Asset, Material, PbrMetallicRoughness, TextureInfo, Texture, Image, Sampler, Skin # Added Skin
from PIL import Image as PILImage

COMPONENT_TYPE_MAP = {
 np.float32: 5126,  # FLOAT
 np.uint32: 5125,   # UNSIGNED_INT
 np.uint16: 5123,   # UNSIGNED_SHORT
 np.int16: 5122,    # SHORT
 np.uint8: 5121,    # UNSIGNED_BYTE
 np.int8: 5120,     # BYTE
}

ACCESSOR_TYPE_MAP = {
 1: "SCALAR",
 2: "VEC2",
 3: "VEC3",
 4: "VEC4",
 16: "MAT4"
}

class Reader:
 def __init__(self, data):
     self.data = data
     self.off = 0
 def read(self, fmt):
     size = struct.calcsize(fmt)
     val = struct.unpack_from(fmt, self.data, self.off)
     self.off += size
     return val
 def read_uint(self):
     return self.read("<I")[0]
 def read_ushort(self):
     return self.read("<H")[0]
 def read_bytes(self, n):
     val = self.data[self.off:self.off + n]
     self.off += n
     return val
 def seek(self, offset):
     self.off = offset
 def tell(self):
     return self.off

def load_string_map(path):
 mapping = {}
 with open(path, 'r', encoding='utf-8', errors='ignore') as f:
     for line in f:
         parts = line.strip().split()
         if len(parts) >= 2 and parts[0].startswith('0x'):
             mapping[int(parts[0], 16)] = parts[1]
 return mapping

def extract_pcpack(data, string_map, out_dir_for_extraction):
 reader = Reader(data)
 for _ in range(6): reader.read_uint()
 reader.read_uint()
 data_off = reader.read_uint()
 marker = b'\xe3\xe3\xe3\xe3'
 pos = 0
 for _ in range(2):
     idx = data.find(marker, pos)
     if idx < 0:
         raise RuntimeError("Invalid .pcpack: marker not found")
     pos = idx + 4
     reader.seek(pos)
 assets = {}
 ext_map = {b'PCM ': '.pcm', b'DDS ': '.dds'}
 while True:
     hsh = reader.read_uint()
     type_val = reader.read_uint()
     if type_val == 0 or type_val >= 0x1000:
         break
     ofs = reader.read_uint()
     size = reader.read_uint()
     blob = data[ofs + data_off: ofs + data_off + size]
     ext = ext_map.get(blob[:4], '.bin')

     name_key_for_asset_group = string_map.get(hsh, f"{hsh:08X}")
     filename_on_disk_base = string_map.get(hsh, f"{hsh:08X}") # DDS files might be named by hash if not in map
     path = out_dir_for_extraction / f"{filename_on_disk_base}{ext}"

     path.parent.mkdir(parents=True, exist_ok=True)
     path.write_bytes(blob)

     assets.setdefault(name_key_for_asset_group, {}).setdefault(ext, []).append(path)
 return assets

def parse_pcm_file(pcm_path):
 data = pcm_path.read_bytes()
 reader = Reader(data)
 reader.seek(8)
 num_entries, info_ofs = reader.read("<II")
 reader.seek(info_ofs)

 parsed_submeshes = []
 model_bones_data = [] # To store bone information for the model

 uv_size_bytes = struct.calcsize('<2f')
 normal_size_bytes = struct.calcsize('<3f')

 found_model = False
 pcm_main_entries = []
 current_reader_pos = reader.tell()
 for _ in range(num_entries):
     pcm_main_entries.append(reader.read("<HHII"))
 reader.seek(current_reader_pos)

 for entry_type, entry_tag, entry_mdl_ofs, entry_size in pcm_main_entries:
     if entry_tag == 512:
         model_offset = entry_mdl_ofs
         found_model = True
         reader.seek(model_offset)
         name_ofs_model_header = reader.read_uint(); reader.read_uint()
         sub_count = reader.read_uint(); sub_info_block_ofs = reader.read_uint()

         # Read bone information
         num_bn = reader.read_uint()
         ofs_bn = reader.read_uint()
         reader.read_uint() # unk
         reader.read_uint() # ofs_unk
         reader.read_bytes(16) # 4f

         if num_bn > 0 and ofs_bn > 0 and ofs_bn < len(data):
             save_pos = reader.tell()
             reader.seek(ofs_bn)
             for i in range(num_bn):
                 # Bone matrix is 64 bytes (16 floats)
                 # Noesis script implies matrix is row-major. We store it as flat list.
                 raw_matrix_flat = list(reader.read("<16f"))
                 model_bones_data.append({
                     'matrix_raw': raw_matrix_flat, # This is bone's LOCAL transform, row-major flat list
                     'name': f"bone_{i}",
                     'parent': i - 1 if i > 0 else -1 # Simple hierarchy, to be refined
                 })
             reader.seek(save_pos)

         reader.seek(sub_info_block_ofs)
         sub_info_offsets = []
         for _ in range(sub_count):
             reader.read_uint()
             sub_info_offsets.append(reader.read_uint())

         for so_idx, submesh_offset in enumerate(sub_info_offsets):
             reader.seek(submesh_offset)
             name_ofs_sm = reader.read_uint()
             reader.read_uint(); reader.read_uint(); reader.read_uint()
             reader.read_bytes(16) # 4f
             texture_or_material_hash = reader.read_uint()
             reader.read_uint();

             itype = reader.read_uint()
             icnt = reader.read_uint(); iofs = reader.read_uint()
             reader.read_uint();
             vcnt = reader.read_uint(); vofs = reader.read_uint()
             unks_vertex_layout = reader.read("<" + "I"*8)
             stride = unks_vertex_layout[2]

             # Submesh bone map (indices into model_bones_data)
             submesh_bone_map_count = unks_vertex_layout[3]
             submesh_bone_map_offset = unks_vertex_layout[4]
             submesh_bone_map = []
             if submesh_bone_map_count > 0 and submesh_bone_map_offset > 0 and submesh_bone_map_offset < len(data):
                 save_pos_sm = reader.tell()
                 reader.seek(submesh_bone_map_offset)
                 submesh_bone_map = list(reader.read(f"<{submesh_bone_map_count}H"))
                 reader.seek(save_pos_sm)

             current_submesh_parser_pos = reader.tell()
             sm_name_str = f"{pcm_path.stem}_sub_{so_idx}"
             if name_ofs_sm > 0 and name_ofs_sm < len(data) - 4:
                 try:
                     reader.seek(name_ofs_sm + 4)
                     raw_sm_name_bytes = reader.read_bytes(28)
                     decoded_sm_name = raw_sm_name_bytes.split(b'\x00', 1)[0].decode('ascii', 'ignore')
                     if decoded_sm_name: sm_name_str = decoded_sm_name
                 except Exception: pass
             reader.seek(current_submesh_parser_pos)

             if vcnt == 0 or icnt == 0:
                 print(f"Skipping submesh '{sm_name_str}' in {pcm_path.name} (zero vert/index).")
                 continue

             reader.seek(vofs)
             vbuf = reader.read_bytes(vcnt * stride)
             verts_list, uvs_list, normals_list = [], [], []
             joints_data_list, weights_data_list = [], [] # For skinning

             for i in range(vcnt):
                 vert_off_in_vbuf = i * stride
                 px,py,pz = struct.unpack_from('<3f',vbuf,vert_off_in_vbuf); verts_list.extend([px,py,pz])
                 u,v,nx,ny,nz = 0.0,0.0,0.0,0.0,1.0

                 vertex_bone_indices = [0, 0, 0, 0]
                 vertex_bone_weights = [0.0, 0.0, 0.0, 0.0]

                 if stride==24:
                     if 12+uv_size_bytes<=stride:
                         try: u,v=struct.unpack_from('<2f',vbuf,vert_off_in_vbuf+12)
                         except: pass
                 elif stride==64: # Assumed to contain skinning data
                     # Normals
                     if 12+normal_size_bytes<=stride:
                         try: nx,ny,nz=struct.unpack_from('<3f',vbuf,vert_off_in_vbuf+12)
                         except: pass
                     # UVs
                     if 24+uv_size_bytes<=stride:
                         try: u,v=struct.unpack_from('<2f',vbuf,vert_off_in_vbuf+24)
                         except: pass

                     # Bone Indices (as floats, indices into submesh_bone_map)
                     if 32 + struct.calcsize('<4f') <= stride:
                         try:
                             raw_indices_f = struct.unpack_from('<4f', vbuf, vert_off_in_vbuf + 32)
                             temp_indices = []
                             for idx_f in raw_indices_f:
                                 idx_i = int(round(idx_f))
                                 if submesh_bone_map and 0 <= idx_i < len(submesh_bone_map):
                                     temp_indices.append(submesh_bone_map[idx_i]) # Map to global bone index
                                 elif not submesh_bone_map and 0 <= idx_i < len(model_bones_data): # Fallback if no map but indices are valid for global
                                     temp_indices.append(idx_i)
                                 else:
                                     temp_indices.append(0) # Default to bone 0 if mapping fails
                             vertex_bone_indices = temp_indices[:4] # Ensure only 4
                             while len(vertex_bone_indices) < 4: vertex_bone_indices.append(0) # Pad if less than 4
                         except Exception as e:
                             # print(f"Error reading bone indices for {sm_name_str} vert {i}: {e}")
                             pass # Keep default [0,0,0,0]

                     # Bone Weights
                     if 48 + struct.calcsize('<4f') <= stride:
                         try:
                             vertex_bone_weights = list(struct.unpack_from('<4f', vbuf, vert_off_in_vbuf + 48))
                             s = sum(vertex_bone_weights)
                             if s > 1e-5:
                                 vertex_bone_weights = [w / s for w in vertex_bone_weights]
                             else: # Handle zero sum - e.g., assign all to first influencing bone if any index > 0
                                 is_influenced = any(idx > 0 for idx_val_list in vertex_bone_indices for idx in ([idx_val_list] if isinstance(idx_val_list, int) else idx_val_list) if idx > 0)

                                 if is_influenced and sum(vertex_bone_weights) < 1e-5 : # if sum is zero but has influencing bones
                                    vertex_bone_weights = [1.0, 0.0, 0.0, 0.0] # Simplistic: assign to first
                                 elif not is_influenced : # No valid bones influencing, make sure weights are zero
                                    vertex_bone_weights = [0.0,0.0,0.0,0.0]


                         except Exception as e:
                             # print(f"Error reading bone weights for {sm_name_str} vert {i}: {e}")
                             pass # Keep default [0,0,0,0]

                 uvs_list.extend([u,v]); normals_list.extend([nx,ny,nz])
                 joints_data_list.extend(vertex_bone_indices)
                 weights_data_list.extend(vertex_bone_weights)

             reader.seek(iofs)
             ibuf = reader.read_bytes(icnt * 2)
             raw_indices = [struct.unpack_from('<H',ibuf,j*2)[0] for j in range(icnt)]
             indices_list = []
             if itype==5: # Triangle Strip
                 for j in range(len(raw_indices)-2):
                     a,b,c = raw_indices[j],raw_indices[j+1],raw_indices[j+2]
                     if max(a,b,c)>=vcnt or (a==b or a==c or b==c): continue
                     indices_list.extend([b,a,c] if j%2 else [a,b,c])
             else: # Triangles
                 for j in range(0,len(raw_indices)-(len(raw_indices)%3),3):
                     a,b,c = raw_indices[j],raw_indices[j+1],raw_indices[j+2]
                     if max(a,b,c)>=vcnt or (a==b or a==c or b==c): continue
                     indices_list.extend([a,b,c])

             if not indices_list:
                 print(f"Submesh '{sm_name_str}' in {pcm_path.name} (no valid triangles).")
                 continue

             parsed_submeshes.append({
                 'name':sm_name_str,
                 'positions':np.array(verts_list,dtype=np.float32),
                 'uvs':np.array(uvs_list,dtype=np.float32),
                 'normals':np.array(normals_list,dtype=np.float32),
                 'indices':np.array(indices_list,dtype=np.uint16),
                 'num_verts':vcnt,
                 'texture_hash':texture_or_material_hash,
                 'joints': np.array(joints_data_list, dtype=np.uint16).reshape(vcnt, 4) if joints_data_list else np.zeros((vcnt,4), dtype=np.uint16),
                 'weights': np.array(weights_data_list, dtype=np.float32).reshape(vcnt, 4) if weights_data_list else np.zeros((vcnt,4), dtype=np.float32)
             })
         if found_model: break # Process only the first model structure

 if not found_model and num_entries > 0 :
     print(f"Warning: No primary model entry (tag 512) processed in {pcm_path.name}")
 return parsed_submeshes, model_bones_data


def _create_gltf_material_from_texture(gltf, dds_path, output_dir, material_name_base, materials_cache, sampler_idx_ref):
 png_filename = dds_path.stem + ".png"
 if png_filename in materials_cache:
     return materials_cache[png_filename]
 try:
     png_abs_path = output_dir / png_filename
     img_pil = PILImage.open(dds_path)
     img_pil.save(png_abs_path, "PNG")

     if not gltf.samplers:
         gltf.samplers.append(Sampler(magFilter=9729, minFilter=9987)) # LINEAR, LINEAR_MIPMAP_LINEAR
         sampler_idx_ref[0] = 0

     image_idx = len(gltf.images)
     gltf.images.append(Image(uri=png_filename))

     texture_idx = len(gltf.textures)
     gltf.textures.append(Texture(sampler=sampler_idx_ref[0], source=image_idx))

     material_idx = len(gltf.materials)
     gltf.materials.append(Material(
         pbrMetallicRoughness=PbrMetallicRoughness(baseColorTexture=TextureInfo(index=texture_idx)),
         name=material_name_base, doubleSided=True
     ))
     materials_cache[png_filename] = material_idx
     return material_idx
 except FileNotFoundError:
     print(f"Texture file not found during material creation: {dds_path}")
 except Exception as e:
     print(f"Error creating material for texture {dds_path}: {e}")
 return None


# (Existing imports and functions like Reader, load_string_map, extract_pcpack,
#  parse_pcm_file, _create_gltf_material_from_texture remain the same as previous version)
# ...

def write_gltf_assets(submesh_data_list, model_bones_data, gltf_asset_name_str,
                   main_asset_texture_dds_path,
                   string_map,
                   output_dir_path):
 gltf = GLTF2()
 gltf.asset = Asset(version="2.0", generator="pcpack_to_gltf_py_v6_world_bones") # Updated generator name

 binary_blob = b""
 materials_cache = {}
 sampler_idx_ref = [-1]
 gltf.skins = []
 skin_idx_for_model = -1

 # Material setup (same as before)
 main_asset_gltf_material_idx = None
 if main_asset_texture_dds_path and main_asset_texture_dds_path.exists():
     main_asset_gltf_material_idx = _create_gltf_material_from_texture(
         gltf, main_asset_texture_dds_path, output_dir_path,
         main_asset_texture_dds_path.stem + "_main", materials_cache, sampler_idx_ref
     )
 ultimate_fallback_material_idx = None
 def get_ultimate_fallback_material():
     nonlocal ultimate_fallback_material_idx
     if ultimate_fallback_material_idx is None:
         ultimate_fallback_material_idx = len(gltf.materials)
         gltf.materials.append(Material(name="Ultimate_Default_Fallback", doubleSided=True,
                                        pbrMetallicRoughness=PbrMetallicRoughness(baseColorFactor=[0.6, 0.6, 0.6, 1.0])))
     return ultimate_fallback_material_idx

 current_buffer_offset = 0

 # --- Skin and Bone Setup (REVISED LOGIC) ---
 # This list will store the glTF node indices for the bones, in their original order.
 ordered_bone_gltf_node_indices_for_skin_joints = []
 armature_root_node_idx = -1
 # This dictionary maps the original bone index (0, 1, 2...) to its created glTF node index.
 original_bone_idx_to_gltf_node_idx = {}

 if model_bones_data:
     num_total_bones = len(model_bones_data)
     all_ibms_flat_list_col_major = []

     # This will store the calculated world-space transforms (column-major numpy arrays) for each bone.
     # We assume model_bones_data[i]['matrix_raw'] IS the world transform.
     bone_world_transforms_col_major_np = [np.identity(4)] * num_total_bones

     # Create a root "Armature" node for the skeleton.
     # All root bones of the skeleton will be children of this Armature node.
     armature_node = Node(name="Armature_" + gltf_asset_name_str)
     armature_root_node_idx = len(gltf.nodes)
     gltf.nodes.append(armature_node)

     # Pass 1: Read/Convert world transforms and create glTF Node objects (initially unparented and without local matrix set)
     for i, bone_def in enumerate(model_bones_data):
         # Assume bone_def['matrix_raw'] is the WORLD transform (row-major flat list from file)
         world_tm_row_major_flat = bone_def['matrix_raw']
         world_tm_row_major_np = np.array(world_tm_row_major_flat).reshape(4, 4)

         # Convert to column-major for internal use and glTF.
         world_tm_col_major_np = world_tm_row_major_np.T
         bone_world_transforms_col_major_np[i] = world_tm_col_major_np

         # Create the glTF node for this bone. Its local matrix and parenting will be set in Pass 2.
         bone_gltf_node = Node(name=bone_def.get('name', f"bone_{i}"))
         current_bone_gltf_node_idx = len(gltf.nodes)
         gltf.nodes.append(bone_gltf_node)

         # Store mapping and the ordered list for Skin.joints
         original_bone_idx_to_gltf_node_idx[i] = current_bone_gltf_node_idx
         ordered_bone_gltf_node_indices_for_skin_joints.append(current_bone_gltf_node_idx)

     # Pass 2: Calculate local transforms for glTF nodes, establish hierarchy, and calculate Inverse Bind Matrices (IBMs)
     for i, bone_def in enumerate(model_bones_data):
         current_bone_gltf_idx = original_bone_idx_to_gltf_node_idx[i]
         current_bone_world_tm_col_major = bone_world_transforms_col_major_np[i]

         parent_original_idx = bone_def.get('parent', -1) # Still using the i-1 parenting assumption for hierarchy

         parent_world_for_local_calc = np.identity(4) # By default, parent's world is identity (for roots under Armature)
         parent_gltf_node_for_hierarchy = armature_root_node_idx # Default parent node is the Armature

         if parent_original_idx != -1 and parent_original_idx in original_bone_idx_to_gltf_node_idx:
             # Ensure the parent_original_idx (from i-1 logic) is valid
             parent_gltf_node_for_hierarchy = original_bone_idx_to_gltf_node_idx[parent_original_idx]
             parent_world_for_local_calc = bone_world_transforms_col_major_np[parent_original_idx]

         # Calculate local transform: Local = inv(ParentWorld) * ChildWorld
         try:
             inv_parent_world_tm = np.linalg.inv(parent_world_for_local_calc)
             local_tm_col_major_np = np.dot(inv_parent_world_tm, current_bone_world_tm_col_major)
         except np.linalg.LinAlgError:
             print(f"Warning: Singular parent world matrix for bone {i} (orig_idx) when calculating local TM. Using world TM as local.")
             local_tm_col_major_np = current_bone_world_tm_col_major # Fallback: use world as local (will be incorrect if parented)

         # Set the local matrix for the glTF node
         gltf.nodes[current_bone_gltf_idx].matrix = local_tm_col_major_np.flatten(order='F').tolist()

         # Set hierarchy: make current_bone_gltf_idx a child of parent_gltf_node_for_hierarchy
         if gltf.nodes[parent_gltf_node_for_hierarchy].children is None:
             gltf.nodes[parent_gltf_node_for_hierarchy].children = []
         if current_bone_gltf_idx not in gltf.nodes[parent_gltf_node_for_hierarchy].children:
              gltf.nodes[parent_gltf_node_for_hierarchy].children.append(current_bone_gltf_idx)

         # Calculate IBM from this bone's world transform
         try:
             ibm_col_major_np = np.linalg.inv(current_bone_world_tm_col_major)
         except np.linalg.LinAlgError:
             print(f"Warning: Singular world matrix for bone {i} (orig_idx). Using identity for IBM.")
             ibm_col_major_np = np.identity(4) # Fallback to identity IBM
         all_ibms_flat_list_col_major.extend(ibm_col_major_np.flatten(order='F').tolist())

     # After processing all bones, if we have IBMs and joint node indices, create the skin
     if all_ibms_flat_list_col_major and ordered_bone_gltf_node_indices_for_skin_joints:
         ibm_bytes = np.array(all_ibms_flat_list_col_major, dtype=np.float32).tobytes()

         # Ensure buffer view for IBMs is correctly defined *before* it's referenced by accessor
         # And that its data is appended to binary_blob *after* mesh data, or manage offsets carefully.
         # For simplicity, IBM data can be appended now if its offset is tracked.
         bv_ibm_idx = len(gltf.bufferViews)
         gltf.bufferViews.append(BufferView(buffer=0, byteOffset=current_buffer_offset, byteLength=len(ibm_bytes))) # No target for IBMs

         acc_ibm_idx = len(gltf.accessors)
         gltf.accessors.append(Accessor(
             bufferView=bv_ibm_idx,
             componentType=COMPONENT_TYPE_MAP[np.float32],
             count=num_total_bones, # Number of bones
             type="MAT4"
         ))
         binary_blob += ibm_bytes # Append IBM data to the main binary blob
         current_buffer_offset += len(ibm_bytes)


         gltf.skins.append(Skin(
             inverseBindMatrices=acc_ibm_idx,
             joints=ordered_bone_gltf_node_indices_for_skin_joints, # Use the ordered list of glTF bone node indices
             skeleton=armature_root_node_idx # Root node of the skeleton hierarchy
         ))
         skin_idx_for_model = len(gltf.skins) - 1 # Get the index of the newly added skin
 # --- End Skin and Bone Setup ---

 scene_node_indices = []
 if armature_root_node_idx != -1: # Add the Armature (skeleton root) to the scene
     scene_node_indices.append(armature_root_node_idx)

 # Mesh and primitive processing (largely same as before)
 for i, sm_data in enumerate(submesh_data_list):
     # ... (material lookup logic - same as before) ...
     submesh_name = sm_data.get('name', f"submesh_{i}")
     texture_hash_from_pcm = sm_data.get('texture_hash')
     current_primitive_material_idx = None

     if texture_hash_from_pcm is not None and texture_hash_from_pcm != 0:
         resolved_tex_basename = string_map.get(texture_hash_from_pcm)
         specific_dds_path = None
         if resolved_tex_basename:
             path_try = output_dir_path / (resolved_tex_basename + ".dds")
             if path_try.exists(): specific_dds_path = path_try
         if not specific_dds_path:
             hash_filename_base = f"{texture_hash_from_pcm:08X}"
             path_try_hash = output_dir_path / (hash_filename_base + ".dds")
             if path_try_hash.exists():
                 specific_dds_path = path_try_hash
                 if not resolved_tex_basename: resolved_tex_basename = hash_filename_base
         if specific_dds_path:
             mat_name_for_specific = resolved_tex_basename if resolved_tex_basename else f"tex_hash_{texture_hash_from_pcm:08X}"
             current_primitive_material_idx = _create_gltf_material_from_texture(
                 gltf, specific_dds_path, output_dir_path,
                 mat_name_for_specific, materials_cache, sampler_idx_ref
             )
             if current_primitive_material_idx is None:
                 print(f"Failed to process specific texture for submesh '{submesh_name}' (hash 0x{texture_hash_from_pcm:08X}).")

     if current_primitive_material_idx is None and main_asset_gltf_material_idx is not None:
         current_primitive_material_idx = main_asset_gltf_material_idx
     if current_primitive_material_idx is None:
         current_primitive_material_idx = get_ultimate_fallback_material()


     pos_data=sm_data['positions'].tobytes(); uv_data=sm_data['uvs'].tobytes(); norm_data=sm_data['normals'].tobytes(); idx_data=sm_data['indices'].tobytes()
     num_verts=sm_data['num_verts']; num_indices=len(sm_data['indices'])
     if num_verts==0 or num_indices==0: print(f"Skipping {submesh_name} (empty geom)"); continue

     attributes_dict = {}

     # Positions
     bv_pos_idx=len(gltf.bufferViews);gltf.bufferViews.append(BufferView(buffer=0,byteOffset=current_buffer_offset,byteLength=len(pos_data),target=34962))
     min_pos_vals = np.min(sm_data['positions'].reshape(-1,3),axis=0).tolist() if sm_data['positions'].size > 0 else [0,0,0]
     max_pos_vals = np.max(sm_data['positions'].reshape(-1,3),axis=0).tolist() if sm_data['positions'].size > 0 else [0,0,0]
     acc_pos_idx=len(gltf.accessors);gltf.accessors.append(Accessor(bufferView=bv_pos_idx,componentType=COMPONENT_TYPE_MAP[np.float32],count=num_verts,type="VEC3",min=min_pos_vals,max=max_pos_vals))
     binary_blob+=pos_data; current_buffer_offset+=len(pos_data)
     attributes_dict['POSITION'] = acc_pos_idx

     # Normals
     bv_norm_idx=len(gltf.bufferViews);gltf.bufferViews.append(BufferView(buffer=0,byteOffset=current_buffer_offset,byteLength=len(norm_data),target=34962))
     acc_norm_idx=len(gltf.accessors);gltf.accessors.append(Accessor(bufferView=bv_norm_idx,componentType=COMPONENT_TYPE_MAP[np.float32],count=num_verts,type="VEC3"))
     binary_blob+=norm_data; current_buffer_offset+=len(norm_data)
     attributes_dict['NORMAL'] = acc_norm_idx

     # UVs
     bv_uv_idx=len(gltf.bufferViews);gltf.bufferViews.append(BufferView(buffer=0,byteOffset=current_buffer_offset,byteLength=len(uv_data),target=34962))
     acc_uv_idx=len(gltf.accessors);gltf.accessors.append(Accessor(bufferView=bv_uv_idx,componentType=COMPONENT_TYPE_MAP[np.float32],count=num_verts,type="VEC2"))
     binary_blob+=uv_data; current_buffer_offset+=len(uv_data)
     attributes_dict['TEXCOORD_0'] = acc_uv_idx

     # Skinning Attributes
     if skin_idx_for_model != -1 and 'joints' in sm_data and 'weights' in sm_data:
         joints_np = sm_data['joints']
         weights_np = sm_data['weights']

         if joints_np.size > 0 and weights_np.size > 0 and joints_np.shape[0] == num_verts and weights_np.shape[0] == num_verts:
             joints_val_data = joints_np.tobytes()
             bv_joints_idx = len(gltf.bufferViews)
             gltf.bufferViews.append(BufferView(buffer=0, byteOffset=current_buffer_offset, byteLength=len(joints_val_data), target=34962))
             acc_joints_idx = len(gltf.accessors)
             gltf.accessors.append(Accessor(bufferView=bv_joints_idx, componentType=COMPONENT_TYPE_MAP[np.uint16], count=num_verts, type="VEC4"))
             binary_blob += joints_val_data; current_buffer_offset += len(joints_val_data)
             attributes_dict['JOINTS_0'] = acc_joints_idx

             weights_val_data = weights_np.tobytes()
             bv_weights_idx = len(gltf.bufferViews)
             gltf.bufferViews.append(BufferView(buffer=0, byteOffset=current_buffer_offset, byteLength=len(weights_val_data), target=34962))
             acc_weights_idx = len(gltf.accessors)
             gltf.accessors.append(Accessor(bufferView=bv_weights_idx, componentType=COMPONENT_TYPE_MAP[np.float32], count=num_verts, type="VEC4"))
             binary_blob += weights_val_data; current_buffer_offset += len(weights_val_data)
             attributes_dict['WEIGHTS_0'] = acc_weights_idx
         else:
             if joints_np.shape[0] != num_verts or weights_np.shape[0] != num_verts:
                 print(f"Warning: Mismatch in vertex count for skinning data in submesh '{submesh_name}'. Expected {num_verts}, got joints: {joints_np.shape[0]}, weights: {weights_np.shape[0]}. Skipping skinning attributes for this submesh.")

     # Indices
     bv_idx_idx=len(gltf.bufferViews);gltf.bufferViews.append(BufferView(buffer=0,byteOffset=current_buffer_offset,byteLength=len(idx_data),target=34963))
     acc_idx_idx=len(gltf.accessors);gltf.accessors.append(Accessor(bufferView=bv_idx_idx,componentType=COMPONENT_TYPE_MAP[np.uint16],count=num_indices,type="SCALAR"))
     binary_blob+=idx_data; current_buffer_offset+=len(idx_data)

     primitive=Primitive(attributes=Attributes(**attributes_dict),indices=acc_idx_idx,material=current_primitive_material_idx,mode=4)
     mesh_idx=len(gltf.meshes);gltf.meshes.append(Mesh(primitives=[primitive],name=submesh_name))

     node_for_mesh = Node(mesh=mesh_idx,name=submesh_name+"_node")
     if skin_idx_for_model != -1 and 'JOINTS_0' in attributes_dict:
          node_for_mesh.skin = skin_idx_for_model

     mesh_node_gltf_idx=len(gltf.nodes)
     gltf.nodes.append(node_for_mesh)
     scene_node_indices.append(mesh_node_gltf_idx) # Add mesh node to scene

 if not gltf.nodes : print(f"No nodes generated for {gltf_asset_name_str}. GLTF will be empty or invalid."); return
 if not scene_node_indices and armature_root_node_idx == -1:
     print(f"No valid submeshes or armature for {gltf_asset_name_str}")
     # If there are no scene nodes but there are other definitions (like materials, accessors from a failed skin setup)
     # an empty scene might still be written, or we can return here.
     # For robustness, ensure a scene is only added if there are nodes for it.
     if not gltf.scenes: # Only add a default scene if none exists and we have nodes
         if gltf.nodes: # If there are nodes (e.g. only an armature was created but no meshes)
              gltf.scenes.append(Scene(nodes=[armature_root_node_idx] if armature_root_node_idx != -1 else [], name=gltf_asset_name_str+"_scene"))
              gltf.scene=0
         else: # No nodes at all
              return # Nothing to write
     elif not gltf.scenes[0].nodes: # Scene exists but is empty
          if armature_root_node_idx != -1:
               gltf.scenes[0].nodes.append(armature_root_node_idx)


 if gltf.scenes: # Ensure scene exists if we are proceeding
     if not gltf.scenes[0].nodes and scene_node_indices: # If default scene is empty but we collected mesh nodes
          gltf.scenes[0].nodes = scene_node_indices
     elif gltf.scenes[0].nodes and scene_node_indices: # If default scene has armature, append mesh nodes
         # Avoid duplicating armature if it's already in scene_node_indices from the start
         for mesh_node_idx_val in scene_node_indices:
             if mesh_node_idx_val not in gltf.scenes[0].nodes:
                  gltf.scenes[0].nodes.append(mesh_node_idx_val)

 if not gltf.scenes and (scene_node_indices or armature_root_node_idx != -1) : # If no scene was created yet but we have nodes
     nodes_for_scene = []
     if armature_root_node_idx != -1: nodes_for_scene.append(armature_root_node_idx)
     nodes_for_scene.extend([n for n in scene_node_indices if n != armature_root_node_idx])
     gltf.scenes.append(Scene(nodes=nodes_for_scene, name=gltf_asset_name_str+"_scene"))
     gltf.scene = 0


 bin_fn=gltf_asset_name_str+".bin"
 if len(binary_blob) > 0 :
      gltf.buffers.append(Buffer(uri=bin_fn,byteLength=len(binary_blob)))

 gltf_fp=output_dir_path/(gltf_asset_name_str+".gltf")
 try:
     gltf.save_json(str(gltf_fp))
 except Exception as e:
     print(f"Error saving gltf {gltf_fp}: {e}")
     return

 if len(binary_blob) > 0 :
     with open(output_dir_path/bin_fn,'wb') as f:f.write(binary_blob)
     print(f"Generated {gltf_fp.name} and {bin_fn}")
 elif gltf.buffers :
     print(f"Generated {gltf_fp.name}, but .bin file was not created as binary data was empty (though buffer entry exists).")
 else:
     print(f"Generated {gltf_fp.name} (no binary data).")


def main():
 parser = argparse.ArgumentParser(description='Extract .pcpack and generate glTF with tiered texture assignment and skinning')
 parser.add_argument('pcpack', help='Input .pcpack file')
 args = parser.parse_args()

 in_path = Path(args.pcpack).resolve()
 out_dir = in_path.parent / f"{in_path.stem}_output_gltf_skinned"
 out_dir.mkdir(exist_ok=True)

 script_dir = Path(__file__).resolve().parent
 strmap_path = script_dir / 'string_hash_dictionary.txt'
 if not strmap_path.exists():
     strmap_path_alt = in_path.parent / 'string_hash_dictionary.txt'
     if strmap_path_alt.exists(): strmap_path = strmap_path_alt
     else: print(f"string_hash_dictionary.txt not found."); sys.exit(1)

 strmap = load_string_map(strmap_path)
 data = in_path.read_bytes()
 assets = extract_pcpack(data, strmap, out_dir)

 for asset_name_str, files_dict in assets.items():
     # Consider .pcm plus files saved without magic (.bin/.dat) and try them as PCM.
     pcms_paths = []
     for _ext in ('.pcm', '.bin', '.dat'):
         pcms_paths.extend(files_dict.get(_ext, []))
     if not pcms_paths: continue

     dds_files_paths = files_dict.get('.dds', [])
     main_asset_texture_dds_path = dds_files_paths[0] if dds_files_paths else None

     all_submeshes_for_this_asset = []
     model_bones_for_this_asset = None # Store bones for the current asset group

     for pcm_path in pcms_paths:
         try:
             parsed_submeshes, pcm_bones = parse_pcm_file(pcm_path)
         except Exception as e:
             print(f"Skipping {pcm_path.name}: not PCM or failed to parse ({e})")
             continue
         if parsed_submeshes: all_submeshes_for_this_asset.extend(parsed_submeshes)
         if pcm_bones and model_bones_for_this_asset is None: # Take bones from the first PCM that has them
             model_bones_for_this_asset = pcm_bones

     if all_submeshes_for_this_asset:
         write_gltf_assets(all_submeshes_for_this_asset,
                           model_bones_for_this_asset, # Pass the collected bone data
                           asset_name_str,
                           main_asset_texture_dds_path,
                           strmap,
                           out_dir)
 print("Done.")

if __name__ == '__main__':
 main()