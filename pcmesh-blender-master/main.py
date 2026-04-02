#!/usr/bin/env python3
"""
Ultimate Spider-Man Asset Viewer
================================
PySide6 + OpenGL viewer for USM game assets (.pcmesh, .pcskel, .pcanim)

Usage:
    python main.py

Dependencies are installed automatically on first run.

LemonHaze - 2025
"""

import sys
import os
import subprocess
import importlib
import shutil
import urllib.request
from pathlib import Path as _Path

# ─── Auto-dependency bootstrapper (venv-aware) ───────────────────────────────

_DEPS = [
    # (import_name, pip_name)
    ("numpy",        "numpy"),
    ("PySide6",      "PySide6"),
    ("OpenGL",       "PyOpenGL"),
]

_VENV_DIR = _Path(__file__).resolve().parent / ".venv"


def _in_venv():
    return sys.prefix != sys.base_prefix


def _deps_missing():
    missing = []
    for mod_name, pip_name in _DEPS:
        try:
            importlib.import_module(mod_name)
        except ImportError:
            missing.append(pip_name)
    return missing


def _venv_python():
    if sys.platform == "win32":
        return str(_VENV_DIR / "Scripts" / "python.exe")
    return str(_VENV_DIR / "bin" / "python")


def _has_pip(python):
    return subprocess.run(
        [python, "-m", "pip", "--version"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0


def _ensure_venv_with_pip():
    """Create .venv and guarantee pip is available inside it."""
    vpython = _venv_python()

    if _VENV_DIR.exists() and os.path.isfile(vpython) and _has_pip(vpython):
        return vpython  # already good

    # Nuke stale venv if pip is broken
    if _VENV_DIR.exists():
        shutil.rmtree(_VENV_DIR, ignore_errors=True)

    # 1) Try creating with pip (works when python3-venv ships ensurepip)
    import venv as _venv_mod
    print(f"Creating virtual environment in {_VENV_DIR} ...")
    try:
        _venv_mod.create(str(_VENV_DIR), with_pip=True)
    except Exception:
        _venv_mod.create(str(_VENV_DIR), with_pip=False)

    if not os.path.isfile(vpython):
        print(f"ERROR: venv python not found at {vpython}\n"
              f"Try:  sudo apt install python3-venv")
        sys.exit(1)

    if _has_pip(vpython):
        return vpython

    # 2) Try ensurepip directly
    print("pip missing from venv, trying ensurepip ...")
    r = subprocess.run([vpython, "-m", "ensurepip", "--upgrade"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode == 0 and _has_pip(vpython):
        return vpython

    # 3) Download get-pip.py as last resort
    print("ensurepip unavailable, downloading get-pip.py ...")
    get_pip = str(_VENV_DIR / "get-pip.py")
    try:
        urllib.request.urlretrieve("https://bootstrap.pypa.io/get-pip.py", get_pip)
        subprocess.check_call([vpython, get_pip], stdout=subprocess.DEVNULL)
    except Exception as e:
        print(f"\nCould not bootstrap pip: {e}\n"
              f"Fix:  sudo apt install python3-venv python3-pip\n"
              f"Then delete {_VENV_DIR} and re-run.")
        sys.exit(1)

    if _has_pip(vpython):
        return vpython

    print(f"\nStill no pip after all attempts.\n"
          f"Fix:  sudo apt install python3-venv python3-pip\n"
          f"Then delete {_VENV_DIR} and re-run.")
    sys.exit(1)


def _bootstrap():
    missing = _deps_missing()
    if not missing:
        return  # everything importable, carry on

    # ── Already inside a venv → just pip install ──
    if _in_venv():
        print(f"[venv] Installing: {', '.join(missing)}")
        subprocess.check_call(
            [sys.executable, "-m", "pip", "install", "--upgrade", "-q"] + missing
        )
        return

    # ── Not in a venv → create one and re-launch ──
    vpython = _ensure_venv_with_pip()

    print(f"Installing dependencies into venv: {', '.join(missing)}")
    subprocess.check_call(
        [vpython, "-m", "pip", "install", "--upgrade", "-q"] + missing
    )

    # Re-exec this script under the venv interpreter
    print("Re-launching inside venv ...\n")
    os.execv(vpython, [vpython] + sys.argv)


_bootstrap()

# ─── Standard / third-party imports ──────────────────────────────────────────

import io
import struct
import math
import time
import traceback
from pathlib import Path
from ctypes import sizeof

import numpy as np

from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QSplitter, QTreeWidget, QTreeWidgetItem, QFileDialog, QToolBar,
    QStatusBar, QLabel, QSlider, QPushButton, QCheckBox, QComboBox,
    QGroupBox, QProgressDialog, QMenu, QMessageBox,
)
from PySide6.QtCore import Qt, QTimer, Signal, QThread
from PySide6.QtGui import QAction, QIcon, QFont
from PySide6.QtOpenGLWidgets import QOpenGLWidget
from PySide6.QtOpenGL import (
    QOpenGLShader, QOpenGLShaderProgram,
)

from OpenGL.GL import *
from OpenGL.GLU import *

# ─── Local asset modules ──────────────────────────────────────────────────────

# Ensure this directory is on the path so relative imports in the modules work
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)

from pcmesh import (
    init as pcmesh_init,
    read_meshfile,
    nglMeshFileHeader, sizeof as ctypes_sizeof,
    string_hash_dictionary,
    resource_key_type_ext,
)
from pcskel import NalSkeletonParser
from pcanim import open_pcanim, PCANIMParseError, ANIM_CONTAINER


# ─── Constants ────────────────────────────────────────────────────────────────

ASSET_TYPES = {
    ".pcmesh": "Mesh",
    ".pcskel": "Skeleton",
    ".pcanim": "Animation",
    ".pcpack": "Pack",
    ".dds": "Texture",
}

VERT_SHADER = """
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vNormal;
out vec3 vFragPos;
out vec2 vUV;

void main() {
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    gl_Position = uProj * uView * worldPos;
    vFragPos = worldPos.xyz;
    vNormal = mat3(transpose(inverse(uModel))) * aNormal;
    vUV = aUV;
}
"""

FRAG_SHADER = """
#version 330 core
in vec3 vNormal;
in vec3 vFragPos;
in vec2 vUV;

out vec4 FragColor;

uniform vec3 uLightDir;
uniform vec3 uBaseColor;
uniform vec3 uCamPos;
uniform bool uWireframe;

void main() {
    if (uWireframe) {
        FragColor = vec4(0.0, 1.0, 0.4, 1.0);
        return;
    }
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    vec3 L = normalize(uLightDir);

    float ambient = 0.18;
    float diff = max(dot(N, L), 0.0) * 0.65;

    vec3 V = normalize(uCamPos - vFragPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;

    vec3 color = uBaseColor * (ambient + diff) + vec3(spec);
    FragColor = vec4(color, 1.0);
}
"""

GRID_VERT = """
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
out float vDist;
void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
    vDist = length(aPos.xz);
}
"""

GRID_FRAG = """
#version 330 core
in float vDist;
out vec4 FragColor;
uniform vec3 uGridColor;
void main() {
    float alpha = clamp(1.0 - vDist / 50.0, 0.05, 0.35);
    FragColor = vec4(uGridColor, alpha);
}
"""

BONE_VERT = """
#version 330 core
layout(location = 0) in vec3 aPos;
uniform mat4 uView;
uniform mat4 uProj;
void main() {
    gl_Position = uProj * uView * vec4(aPos, 1.0);
}
"""

BONE_FRAG = """
#version 330 core
out vec4 FragColor;
uniform vec3 uBoneColor;
void main() {
    FragColor = vec4(uBoneColor, 1.0);
}
"""


# ─── Scanner ──────────────────────────────────────────────────────────────────

class AssetEntry:
    __slots__ = ("path", "asset_type", "name", "parent_pack", "offset", "size", "data")

    def __init__(self, path, asset_type, name=None, parent_pack=None, offset=0, size=0):
        self.path = path
        self.asset_type = asset_type
        self.name = name or os.path.basename(path)
        self.parent_pack = parent_pack
        self.offset = offset
        self.size = size
        self.data = None


def _detect_type_by_magic(filepath):
    """Try to detect asset type by reading file header magic bytes."""
    try:
        with open(filepath, "rb") as f:
            header = f.read(64)
        if len(header) < 4:
            return None
        if header[:4] == b"PCM ":
            return ".pcmesh"
        u32 = struct.unpack_from("<I", header, 0)[0]
        if u32 == ANIM_CONTAINER:
            return ".pcanim"
        # PCSKEL detection: class field is typically small, version field matches known values
        if len(header) >= 8:
            cls, ver = struct.unpack_from("<II", header, 0)
            if cls in (1, 2, 3) and ver in (0x10100, 0x10200, 0x10003):
                return ".pcskel"
    except Exception:
        pass
    return None


# ─── PCPACK archive scanner ──────────────────────────────────────────────────

_PCM_MAGIC  = b"PCM "
_ANIM_MAGIC = struct.pack("<I", ANIM_CONTAINER)   # 0x00010101 LE

def _scan_pcpack(filepath):
    """Scan a PCPACK for embedded PCMESH / PCANIM / PCSKEL resources.

    Returns a list of AssetEntry children with parent_pack, offset, and size set.
    """
    children = []
    try:
        blob = Path(filepath).read_bytes()
    except Exception:
        return children

    file_len = len(blob)
    pack_name = os.path.basename(filepath)

    # ── Find PCMESH blocks (magic "PCM ") ──
    pos = 0
    mesh_idx = 0
    while True:
        idx = blob.find(_PCM_MAGIC, pos)
        if idx < 0:
            break

        # Validate: read nglMeshFileHeader and sanity-check
        if idx + 20 <= file_len:
            tag = blob[idx:idx+4]
            version = struct.unpack_from("<I", blob, idx + 4)[0]
            if tag == _PCM_MAGIC and version in (0x601, 0x501, 0x600, 0x602):
                # Estimate size: from this header to the next PCM or end
                next_pcm = blob.find(_PCM_MAGIC, idx + 16)
                est_size = (next_pcm - idx) if next_pcm > idx else (file_len - idx)

                name = f"{pack_name}::mesh_{mesh_idx}"
                children.append(AssetEntry(
                    path=filepath,
                    asset_type=".pcmesh",
                    name=name,
                    parent_pack=filepath,
                    offset=idx,
                    size=est_size,
                ))
                mesh_idx += 1
        pos = idx + 4

    # ── Find PCANIM blocks (magic 0x00010101) ──
    pos = 0
    anim_idx = 0
    while True:
        idx = blob.find(_ANIM_MAGIC, pos)
        if idx < 0:
            break

        if idx + 64 <= file_len:
            ver = struct.unpack_from("<I", blob, idx)[0]
            if ver == ANIM_CONTAINER:
                # Read num_anims and first_anim from the header to sanity-check
                if idx + 52 <= file_len:
                    num_anims = struct.unpack_from("<i", blob, idx + 48)[0]
                    if 0 < num_anims < 10000:
                        next_hit = blob.find(_ANIM_MAGIC, idx + 16)
                        est_size = (next_hit - idx) if next_hit > idx else (file_len - idx)

                        name = f"{pack_name}::anim_{anim_idx}"
                        children.append(AssetEntry(
                            path=filepath,
                            asset_type=".pcanim",
                            name=name,
                            parent_pack=filepath,
                            offset=idx,
                            size=est_size,
                        ))
                        anim_idx += 1
        pos = idx + 4

    # ── Find PCSKEL blocks ──
    #    Heuristic: scan for (class 1..3, version 0x10100) pairs.
    #    These are 8-byte aligned and followed by a 32-byte tlFixedString.
    pos = 0
    skel_idx = 0
    while pos + 72 <= file_len:
        cls = struct.unpack_from("<I", blob, pos)[0]
        ver = struct.unpack_from("<I", blob, pos + 4)[0]
        if cls in (1, 2, 3) and ver == 0x10100:
            # Extra check: read the name field (32-byte tlFixedString at +8)
            name_bytes = blob[pos+12 : pos+40]
            try:
                skel_name = name_bytes.split(b"\x00", 1)[0].decode("ascii")
            except Exception:
                skel_name = ""
            if skel_name and skel_name.isprintable() and len(skel_name) >= 2:
                next_hit = pos + 72
                est_size = file_len - pos
                # Try to find the next skel header for size estimation
                scan = pos + 8
                while scan + 8 <= file_len:
                    c2 = struct.unpack_from("<I", blob, scan)[0]
                    v2 = struct.unpack_from("<I", blob, scan + 4)[0]
                    if c2 in (1, 2, 3) and v2 == 0x10100 and scan != pos:
                        est_size = scan - pos
                        break
                    scan += 4
                    if scan - pos > 0x80000:
                        break

                name = f"{pack_name}::skel_{skel_idx} ({skel_name})"
                children.append(AssetEntry(
                    path=filepath,
                    asset_type=".pcskel",
                    name=name,
                    parent_pack=filepath,
                    offset=pos,
                    size=est_size,
                ))
                skel_idx += 1
                pos += est_size
                continue
        pos += 4

    return children


# ─── Buffer-based mesh reader (for PCPACK sub-resources) ─────────────────────

def _read_mesh_from_buffer(buffer_bytes):
    """Parse PCMESH data from a raw byte buffer — same logic as read_meshfile."""
    from pcmesh import (
        nglMeshFileHeader, nglDirectoryEntry, nglMesh, nglMaterialBase,
        tlFixedString, TypeDirectoryEntry, Section, nglMeshSection,
        read_mesh, MeshData,
    )
    from ctypes import sizeof as c_sizeof

    Header = nglMeshFileHeader.from_buffer_copy(buffer_bytes[:c_sizeof(nglMeshFileHeader)])
    if Header.Tag != b"PCM ":
        raise ValueError(f"Bad PCMESH magic: {Header.Tag!r}")

    materials = []
    mesh_data = []

    for i in range(Header.NDirectoryEntries):
        off = Header.DirectoryEntries + i * c_sizeof(nglDirectoryEntry)
        if off + c_sizeof(nglDirectoryEntry) > len(buffer_bytes):
            break
        entry = nglDirectoryEntry.from_buffer_copy(
            buffer_bytes[off : off + c_sizeof(nglDirectoryEntry)]
        )
        type_dir = int.from_bytes(entry.typeDirectoryEntry, byteorder="big")

        if type_dir == int(TypeDirectoryEntry.MATERIAL):
            mat_off = entry.field_4
            if mat_off + c_sizeof(nglMaterialBase) > len(buffer_bytes):
                continue
            Material = nglMaterialBase.from_buffer_copy(
                buffer_bytes[mat_off : mat_off + c_sizeof(nglMaterialBase)]
            )
            name_off = Material.Name
            if name_off + c_sizeof(tlFixedString) > len(buffer_bytes):
                continue
            MatName = tlFixedString.from_buffer_copy(
                buffer_bytes[name_off : name_off + c_sizeof(tlFixedString)]
            )
            tex_off = name_off + c_sizeof(tlFixedString) * 2
            if tex_off + c_sizeof(tlFixedString) <= len(buffer_bytes):
                texFS = tlFixedString.from_buffer_copy(
                    buffer_bytes[tex_off : tex_off + c_sizeof(tlFixedString)]
                )
                tex_str = texFS.field_4.decode("utf-8", errors="ignore").rstrip("\x00")
                materials.append([MatName.field_4, f"{tex_str.upper()}.DDS"])
            else:
                materials.append([MatName.field_4, "UNKNOWN.DDS"])

        elif type_dir == int(TypeDirectoryEntry.MESH):
            mesh_off = entry.field_4
            if mesh_off + c_sizeof(nglMesh) > len(buffer_bytes):
                continue
            mesh = nglMesh.from_buffer_copy(
                buffer_bytes[mesh_off : mesh_off + c_sizeof(nglMesh)]
            )
            mesh_data.append(read_mesh(mesh, buffer_bytes, materials, False))

    return mesh_data


def scan_folder(root_path, progress_callback=None):
    """Recursively scan a folder for USM assets, including PCPACK contents."""
    assets = []
    root = Path(root_path)
    all_files = []
    for p in root.rglob("*"):
        if p.is_file():
            all_files.append(p)

    for i, p in enumerate(all_files):
        if progress_callback and i % 50 == 0:
            progress_callback(i, len(all_files), str(p.name))

        ext = p.suffix.lower()

        # Individual asset files
        if ext in (".pcmesh", ".pcskel", ".pcanim", ".dds"):
            assets.append(AssetEntry(str(p), ext, p.name))
            continue

        # PCPACK archives — scan for embedded resources
        if ext == ".pcpack":
            pack_entry = AssetEntry(str(p), ".pcpack", p.name)
            pack_entry.data = _scan_pcpack(str(p))   # stash children
            assets.append(pack_entry)
            continue

        # Extensionless / generic — try magic bytes
        if ext in ("", ".bin", ".dat"):
            detected = _detect_type_by_magic(str(p))
            if detected:
                assets.append(AssetEntry(str(p), detected, p.name))

    return assets


# ─── Mesh GPU Data ────────────────────────────────────────────────────────────

class GPUMeshSection:
    """Holds OpenGL buffer IDs for one mesh section."""
    __slots__ = ("vao", "vbo", "ebo", "index_count", "name")

    def __init__(self):
        self.vao = 0
        self.vbo = 0
        self.ebo = 0
        self.index_count = 0
        self.name = ""


class GPUMesh:
    """Complete mesh on the GPU."""
    def __init__(self):
        self.sections: list[GPUMeshSection] = []
        self.bounds_center = np.array([0.0, 0.0, 0.0], dtype=np.float32)
        self.bounds_radius = 1.0
        self.bone_matrices = []

    def release(self):
        for s in self.sections:
            if s.vao:
                glDeleteVertexArrays(1, [s.vao])
            if s.vbo:
                glDeleteBuffers(1, [s.vbo])
            if s.ebo:
                glDeleteBuffers(1, [s.ebo])
        self.sections.clear()


# ─── Camera ───────────────────────────────────────────────────────────────────

class ArcballCamera:
    def __init__(self):
        self.yaw = 45.0
        self.pitch = 25.0
        self.distance = 5.0
        self.target = np.array([0.0, 0.5, 0.0], dtype=np.float32)
        self.fov = 45.0
        self.near = 0.01
        self.far = 500.0

    def get_position(self):
        yr = math.radians(self.yaw)
        pr = math.radians(self.pitch)
        x = self.distance * math.cos(pr) * math.sin(yr)
        y = self.distance * math.sin(pr)
        z = self.distance * math.cos(pr) * math.cos(yr)
        return self.target + np.array([x, y, z], dtype=np.float32)

    def get_view_matrix(self):
        pos = self.get_position()
        return _look_at(pos, self.target, np.array([0, 1, 0], dtype=np.float32))

    def get_projection_matrix(self, aspect):
        return _perspective(self.fov, aspect, self.near, self.far)

    def orbit(self, dx, dy):
        self.yaw -= dx * 0.3
        self.pitch += dy * 0.3
        self.pitch = max(-89.0, min(89.0, self.pitch))

    def pan(self, dx, dy):
        yr = math.radians(self.yaw)
        right = np.array([math.cos(yr), 0, -math.sin(yr)], dtype=np.float32)
        up = np.array([0, 1, 0], dtype=np.float32)
        scale = self.distance * 0.002
        self.target -= right * dx * scale
        self.target += up * dy * scale

    def zoom(self, delta):
        self.distance *= 0.9 if delta > 0 else 1.1
        self.distance = max(0.1, min(200.0, self.distance))

    def focus_on(self, center, radius):
        self.target = np.array(center, dtype=np.float32)
        self.distance = max(0.5, radius * 2.5)


# ─── Math Helpers ─────────────────────────────────────────────────────────────

def _perspective(fov, aspect, near, far):
    f = 1.0 / math.tan(math.radians(fov) / 2.0)
    m = np.zeros((4, 4), dtype=np.float32)
    m[0, 0] = f / aspect
    m[1, 1] = f
    m[2, 2] = (far + near) / (near - far)
    m[2, 3] = -1.0
    m[3, 2] = (2.0 * far * near) / (near - far)
    return m


def _look_at(eye, center, up):
    f = center - eye
    f = f / np.linalg.norm(f)
    s = np.cross(f, up)
    sn = np.linalg.norm(s)
    if sn < 1e-8:
        s = np.array([1, 0, 0], dtype=np.float32)
    else:
        s = s / sn
    u = np.cross(s, f)
    m = np.eye(4, dtype=np.float32)
    m[0, :3] = s
    m[1, :3] = u
    m[2, :3] = -f
    m[3, 0] = -np.dot(s, eye)
    m[3, 1] = -np.dot(u, eye)
    m[3, 2] = np.dot(f, eye)
    return m


# ─── OpenGL Viewport ─────────────────────────────────────────────────────────

class GLViewport(QOpenGLWidget):
    status_message = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.camera = ArcballCamera()
        self.mesh: GPUMesh | None = None
        self.skeleton_vao = 0
        self.skeleton_vbo = 0
        self.skeleton_count = 0
        self.grid_vao = 0
        self.grid_vbo = 0
        self.grid_count = 0
        self.show_wireframe = False
        self.show_grid = True
        self.show_bones = True
        self.base_color = np.array([0.75, 0.75, 0.78], dtype=np.float32)

        self._last_mouse_pos = None
        self._mouse_button = None
        self._shader = None
        self._grid_shader = None
        self._bone_shader = None
        self._initialized = False

        # Animation state
        self._bone_positions = []  # list of (start_xyz, end_xyz) for bone lines
        self._anim_data = None
        self._anim_frame = 0
        self._anim_playing = False
        self._anim_timer = QTimer(self)
        self._anim_timer.timeout.connect(self._advance_frame)
        self._anim_fps = 30.0

        self.setMinimumSize(400, 300)
        self.setFocusPolicy(Qt.StrongFocus)

    # ── OpenGL Init ──

    def initializeGL(self):
        glClearColor(0.12, 0.12, 0.14, 1.0)
        glEnable(GL_DEPTH_TEST)
        glEnable(GL_BLEND)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
        glEnable(GL_MULTISAMPLE)

        self._shader = self._compile_program(VERT_SHADER, FRAG_SHADER)
        self._grid_shader = self._compile_program(GRID_VERT, GRID_FRAG)
        self._bone_shader = self._compile_program(BONE_VERT, BONE_FRAG)
        self._build_grid()
        self._initialized = True

    def _compile_program(self, vert_src, frag_src):
        prog = glCreateProgram()
        vs = glCreateShader(GL_VERTEX_SHADER)
        fs = glCreateShader(GL_FRAGMENT_SHADER)
        glShaderSource(vs, vert_src)
        glCompileShader(vs)
        if not glGetShaderiv(vs, GL_COMPILE_STATUS):
            log = glGetShaderInfoLog(vs).decode()
            print(f"Vertex shader error:\n{log}")
        glShaderSource(fs, frag_src)
        glCompileShader(fs)
        if not glGetShaderiv(fs, GL_COMPILE_STATUS):
            log = glGetShaderInfoLog(fs).decode()
            print(f"Fragment shader error:\n{log}")
        glAttachShader(prog, vs)
        glAttachShader(prog, fs)
        glLinkProgram(prog)
        if not glGetProgramiv(prog, GL_LINK_STATUS):
            log = glGetProgramInfoLog(prog).decode()
            print(f"Program link error:\n{log}")
        glDeleteShader(vs)
        glDeleteShader(fs)
        return prog

    def _build_grid(self):
        lines = []
        extent = 50
        step = 1.0
        y = 0.0
        v = -extent
        while v <= extent:
            lines.extend([v, y, -extent, v, y, extent])
            lines.extend([-extent, y, v, extent, y, v])
            v += step
        data = np.array(lines, dtype=np.float32)
        self.grid_count = len(lines) // 3

        self.grid_vao = glGenVertexArrays(1)
        self.grid_vbo = glGenBuffers(1)
        glBindVertexArray(self.grid_vao)
        glBindBuffer(GL_ARRAY_BUFFER, self.grid_vbo)
        glBufferData(GL_ARRAY_BUFFER, data.nbytes, data, GL_STATIC_DRAW)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, None)
        glEnableVertexAttribArray(0)
        glBindVertexArray(0)

    # ── Painting ──

    def paintGL(self):
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)
        w = max(1, self.width())
        h = max(1, self.height())
        aspect = w / h

        view = self.camera.get_view_matrix()
        proj = self.camera.get_projection_matrix(aspect)
        cam_pos = self.camera.get_position()

        # Grid
        if self.show_grid and self._grid_shader and self.grid_vao:
            glUseProgram(self._grid_shader)
            glUniformMatrix4fv(glGetUniformLocation(self._grid_shader, "uView"), 1, GL_TRUE, view)
            glUniformMatrix4fv(glGetUniformLocation(self._grid_shader, "uProj"), 1, GL_TRUE, proj)
            glUniform3f(glGetUniformLocation(self._grid_shader, "uGridColor"), 0.35, 0.35, 0.40)
            glBindVertexArray(self.grid_vao)
            glDrawArrays(GL_LINES, 0, self.grid_count)
            glBindVertexArray(0)

        # Mesh
        if self.mesh and self._shader:
            glUseProgram(self._shader)
            model = np.eye(4, dtype=np.float32)
            glUniformMatrix4fv(glGetUniformLocation(self._shader, "uModel"), 1, GL_TRUE, model)
            glUniformMatrix4fv(glGetUniformLocation(self._shader, "uView"), 1, GL_TRUE, view)
            glUniformMatrix4fv(glGetUniformLocation(self._shader, "uProj"), 1, GL_TRUE, proj)
            glUniform3f(glGetUniformLocation(self._shader, "uLightDir"), 0.4, 0.8, 0.6)
            glUniform3fv(glGetUniformLocation(self._shader, "uBaseColor"), 1, self.base_color)
            glUniform3f(glGetUniformLocation(self._shader, "uCamPos"), *cam_pos)
            glUniform1i(glGetUniformLocation(self._shader, "uWireframe"), 0)

            if self.show_wireframe:
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)
                glUniform1i(glGetUniformLocation(self._shader, "uWireframe"), 1)

            for sec in self.mesh.sections:
                glBindVertexArray(sec.vao)
                glDrawElements(GL_TRIANGLES, sec.index_count, GL_UNSIGNED_INT, None)

            if self.show_wireframe:
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL)

            glBindVertexArray(0)

        # Bones
        if self.show_bones and self.skeleton_vao and self.skeleton_count > 0:
            glUseProgram(self._bone_shader)
            glUniformMatrix4fv(glGetUniformLocation(self._bone_shader, "uView"), 1, GL_TRUE, view)
            glUniformMatrix4fv(glGetUniformLocation(self._bone_shader, "uProj"), 1, GL_TRUE, proj)
            glUniform3f(glGetUniformLocation(self._bone_shader, "uBoneColor"), 1.0, 0.3, 0.1)
            glLineWidth(2.0)
            glBindVertexArray(self.skeleton_vao)
            glDrawArrays(GL_LINES, 0, self.skeleton_count)
            glBindVertexArray(0)

            # Draw bone joint points
            glUniform3f(glGetUniformLocation(self._bone_shader, "uBoneColor"), 1.0, 1.0, 0.2)
            glPointSize(5.0)
            glBindVertexArray(self.skeleton_vao)
            glDrawArrays(GL_POINTS, 0, self.skeleton_count)
            glBindVertexArray(0)

    def resizeGL(self, w, h):
        glViewport(0, 0, w, h)

    # ── Mouse Controls ──

    def mousePressEvent(self, event):
        self._last_mouse_pos = event.position()
        self._mouse_button = event.button()

    def mouseMoveEvent(self, event):
        if self._last_mouse_pos is None:
            return
        pos = event.position()
        dx = pos.x() - self._last_mouse_pos.x()
        dy = pos.y() - self._last_mouse_pos.y()
        self._last_mouse_pos = pos

        if self._mouse_button == Qt.LeftButton:
            self.camera.orbit(dx, dy)
        elif self._mouse_button == Qt.MiddleButton:
            self.camera.pan(dx, dy)
        elif self._mouse_button == Qt.RightButton:
            self.camera.zoom(dy)
        self.update()

    def mouseReleaseEvent(self, event):
        self._last_mouse_pos = None
        self._mouse_button = None

    def wheelEvent(self, event):
        delta = event.angleDelta().y()
        self.camera.zoom(delta)
        self.update()

    # ── Mesh Loading ──

    def load_mesh_data(self, mesh_data_list):
        """Upload parsed MeshData list to GPU."""
        if not self._initialized:
            return
        self.makeCurrent()

        if self.mesh:
            self.mesh.release()
        self.mesh = GPUMesh()

        all_verts = []
        total_tris = 0

        for mesh_data in mesh_data_list:
            for section in mesh_data.sections:
                verts = section["vertices"]
                normals = section.get("normals", [])
                uvs = section.get("uvs", [])
                raw_indices = section["indices"]
                prim_type = section.get("primitive_type", 4)

                if not verts or not raw_indices:
                    continue

                # Convert strips to triangles
                if prim_type == 5:
                    triangles = []
                    for i in range(len(raw_indices) - 2):
                        a, b, c = raw_indices[i], raw_indices[i + 1], raw_indices[i + 2]
                        if len({a, b, c}) == 3:
                            if i % 2 == 1:
                                a, b = b, a
                            triangles.extend([a, b, c])
                    indices = triangles
                elif prim_type == 4:
                    indices = list(raw_indices)
                else:
                    continue

                if not indices:
                    continue

                # Build interleaved vertex buffer: pos(3) + normal(3) + uv(2) = 8 floats
                buf = np.zeros((len(verts), 8), dtype=np.float32)
                for i, v in enumerate(verts):
                    buf[i, 0:3] = v[:3]
                    all_verts.append(v[:3])
                    if i < len(normals) and normals[i]:
                        buf[i, 3:6] = normals[i][:3]
                    else:
                        buf[i, 4] = 1.0  # default up normal
                    if i < len(uvs) and uvs[i]:
                        buf[i, 6:8] = uvs[i][:2]

                idx_data = np.array(indices, dtype=np.uint32)
                total_tris += len(indices) // 3

                sec = GPUMeshSection()
                sec.name = section.get("name", "")
                sec.index_count = len(indices)

                sec.vao = glGenVertexArrays(1)
                sec.vbo = glGenBuffers(1)
                sec.ebo = glGenBuffers(1)

                glBindVertexArray(sec.vao)

                glBindBuffer(GL_ARRAY_BUFFER, sec.vbo)
                glBufferData(GL_ARRAY_BUFFER, buf.nbytes, buf, GL_STATIC_DRAW)

                stride = 8 * 4  # 8 floats * 4 bytes
                glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, ctypes.c_void_p(0))
                glEnableVertexAttribArray(0)
                glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, ctypes.c_void_p(12))
                glEnableVertexAttribArray(1)
                glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, ctypes.c_void_p(24))
                glEnableVertexAttribArray(2)

                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sec.ebo)
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_data.nbytes, idx_data, GL_STATIC_DRAW)

                glBindVertexArray(0)
                self.mesh.sections.append(sec)

        # Compute bounds
        if all_verts:
            pts = np.array(all_verts, dtype=np.float32)
            mn = pts.min(axis=0)
            mx = pts.max(axis=0)
            center = (mn + mx) / 2.0
            radius = float(np.linalg.norm(mx - mn)) / 2.0
            self.mesh.bounds_center = center
            self.mesh.bounds_radius = max(0.1, radius)
            self.camera.focus_on(center, radius)

        self.doneCurrent()
        self.update()
        self.status_message.emit(
            f"Loaded {len(self.mesh.sections)} sections, {total_tris} triangles"
        )

    def load_bone_lines(self, bone_lines):
        """Upload bone visualization lines to GPU. bone_lines = [(x1,y1,z1, x2,y2,z2), ...]"""
        if not self._initialized:
            return
        self.makeCurrent()

        if self.skeleton_vao:
            glDeleteVertexArrays(1, [self.skeleton_vao])
            glDeleteBuffers(1, [self.skeleton_vbo])
            self.skeleton_vao = 0
            self.skeleton_vbo = 0
            self.skeleton_count = 0

        if not bone_lines:
            self.doneCurrent()
            self.update()
            return

        data = np.array(bone_lines, dtype=np.float32).flatten()
        self.skeleton_count = len(data) // 3

        self.skeleton_vao = glGenVertexArrays(1)
        self.skeleton_vbo = glGenBuffers(1)
        glBindVertexArray(self.skeleton_vao)
        glBindBuffer(GL_ARRAY_BUFFER, self.skeleton_vbo)
        glBufferData(GL_ARRAY_BUFFER, data.nbytes, data, GL_STATIC_DRAW)
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, None)
        glEnableVertexAttribArray(0)
        glBindVertexArray(0)
        self.doneCurrent()
        self.update()

    # ── Animation Playback ──

    def set_anim_data(self, anim_data, fps=30.0):
        self._anim_data = anim_data
        self._anim_fps = fps
        self._anim_frame = 0

    def play_animation(self):
        if self._anim_data:
            self._anim_playing = True
            self._anim_timer.start(int(1000.0 / self._anim_fps))

    def pause_animation(self):
        self._anim_playing = False
        self._anim_timer.stop()

    def stop_animation(self):
        self._anim_playing = False
        self._anim_timer.stop()
        self._anim_frame = 0

    def set_frame(self, frame):
        self._anim_frame = frame
        self._update_animation_frame()

    def _advance_frame(self):
        if not self._anim_data:
            return
        frame_count = self._anim_data.get("frame_count", 1)
        self._anim_frame = (self._anim_frame + 1) % max(1, frame_count)
        self._update_animation_frame()

    def _update_animation_frame(self):
        """Update bone positions for current animation frame."""
        if not self._anim_data:
            return

        bone_lines = self._anim_data.get("frame_bone_lines", {})
        frame_data = bone_lines.get(self._anim_frame)
        if frame_data:
            self.load_bone_lines(frame_data)
        self.update()


import ctypes


# ─── Main Window ──────────────────────────────────────────────────────────────

class USMViewer(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Ultimate Spider-Man Asset Viewer")
        self.resize(1400, 900)
        self.assets: list[AssetEntry] = []
        self._current_skel_data = None
        self._current_skel_path = None

        self._init_hash_dict()
        self._build_ui()

    def _init_hash_dict(self):
        """Initialize pcmesh string hash dictionary."""
        try:
            pcmesh_init()
        except Exception as e:
            print(f"Warning: Could not load string hash dictionary: {e}")

    def _build_ui(self):
        # ── Toolbar ──
        toolbar = QToolBar("Main")
        toolbar.setMovable(False)
        self.addToolBar(toolbar)

        act_open = QAction("Open Folder...", self)
        act_open.setShortcut("Ctrl+O")
        act_open.triggered.connect(self._open_folder)
        toolbar.addAction(act_open)

        toolbar.addSeparator()

        self._chk_wireframe = QCheckBox("Wireframe")
        self._chk_wireframe.toggled.connect(self._toggle_wireframe)
        toolbar.addWidget(self._chk_wireframe)

        self._chk_grid = QCheckBox("Grid")
        self._chk_grid.setChecked(True)
        self._chk_grid.toggled.connect(self._toggle_grid)
        toolbar.addWidget(self._chk_grid)

        self._chk_bones = QCheckBox("Bones")
        self._chk_bones.setChecked(True)
        self._chk_bones.toggled.connect(self._toggle_bones)
        toolbar.addWidget(self._chk_bones)

        toolbar.addSeparator()

        self._lbl_skel = QLabel("  Skeleton: (none)")
        self._lbl_skel.setFont(QFont("monospace", 9))
        toolbar.addWidget(self._lbl_skel)

        # ── Status Bar ──
        self._statusbar = QStatusBar()
        self.setStatusBar(self._statusbar)
        self._statusbar.showMessage("Open a folder to begin scanning for assets.")

        # ── Main Splitter ──
        splitter = QSplitter(Qt.Horizontal)
        self.setCentralWidget(splitter)

        # Left: tree + anim controls
        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 0, 0)

        self._tree = QTreeWidget()
        self._tree.setHeaderLabels(["Name", "Type", "Path"])
        self._tree.setColumnWidth(0, 250)
        self._tree.setColumnWidth(1, 80)
        self._tree.itemDoubleClicked.connect(self._on_asset_double_click)
        self._tree.setContextMenuPolicy(Qt.CustomContextMenu)
        self._tree.customContextMenuRequested.connect(self._on_tree_context_menu)
        left_layout.addWidget(self._tree, stretch=3)

        # Animation controls
        anim_group = QGroupBox("Animation")
        anim_layout = QVBoxLayout(anim_group)

        self._anim_combo = QComboBox()
        self._anim_combo.addItem("(no animation)")
        self._anim_combo.currentIndexChanged.connect(self._on_anim_selected)
        anim_layout.addWidget(self._anim_combo)

        btn_row = QHBoxLayout()
        self._btn_play = QPushButton("▶ Play")
        self._btn_play.clicked.connect(self._play_anim)
        btn_row.addWidget(self._btn_play)
        self._btn_pause = QPushButton("⏸ Pause")
        self._btn_pause.clicked.connect(self._pause_anim)
        btn_row.addWidget(self._btn_pause)
        self._btn_stop = QPushButton("⏹ Stop")
        self._btn_stop.clicked.connect(self._stop_anim)
        btn_row.addWidget(self._btn_stop)
        anim_layout.addLayout(btn_row)

        slider_row = QHBoxLayout()
        self._frame_slider = QSlider(Qt.Horizontal)
        self._frame_slider.setMinimum(0)
        self._frame_slider.setMaximum(0)
        self._frame_slider.valueChanged.connect(self._on_frame_slider)
        slider_row.addWidget(self._frame_slider)
        self._lbl_frame = QLabel("0 / 0")
        self._lbl_frame.setMinimumWidth(70)
        slider_row.addWidget(self._lbl_frame)
        anim_layout.addLayout(slider_row)

        left_layout.addWidget(anim_group)
        splitter.addWidget(left_panel)

        # Right: OpenGL viewport
        self._viewport = GLViewport()
        self._viewport.status_message.connect(self._statusbar.showMessage)
        splitter.addWidget(self._viewport)
        splitter.setSizes([380, 1020])

        # Anim frame update timer
        self._frame_poll = QTimer(self)
        self._frame_poll.timeout.connect(self._poll_anim_frame)
        self._frame_poll.start(33)

    # ── Toolbar Toggles ──

    def _toggle_wireframe(self, checked):
        self._viewport.show_wireframe = checked
        self._viewport.update()

    def _toggle_grid(self, checked):
        self._viewport.show_grid = checked
        self._viewport.update()

    def _toggle_bones(self, checked):
        self._viewport.show_bones = checked
        self._viewport.update()

    # ── Folder Scanning ──

    def _open_folder(self):
        folder = QFileDialog.getExistingDirectory(self, "Select Asset Folder")
        if not folder:
            return

        progress = QProgressDialog("Scanning...", "Cancel", 0, 100, self)
        progress.setWindowModality(Qt.WindowModal)
        progress.setMinimumDuration(200)

        def on_progress(current, total, name):
            if total > 0:
                progress.setValue(int(current / total * 100))
                progress.setLabelText(f"Scanning: {name}")
            QApplication.processEvents()
            if progress.wasCanceled():
                raise StopIteration()

        try:
            self.assets = scan_folder(folder, on_progress)
        except StopIteration:
            self._statusbar.showMessage("Scan cancelled.")
            return
        finally:
            progress.close()

        self._populate_tree()
        self._statusbar.showMessage(f"Found {len(self.assets)} assets in {folder}")

    def _populate_tree(self):
        self._tree.clear()
        groups = {}

        for asset in self.assets:
            type_name = ASSET_TYPES.get(asset.asset_type, "Unknown")
            if type_name not in groups:
                groups[type_name] = QTreeWidgetItem(self._tree, [type_name, "", ""])
                groups[type_name].setExpanded(False)
                font = groups[type_name].font(0)
                font.setBold(True)
                groups[type_name].setFont(0, font)

            item = QTreeWidgetItem(groups[type_name], [
                asset.name,
                type_name,
                os.path.relpath(asset.path, os.path.dirname(asset.path)),
            ])
            item.setData(0, Qt.UserRole, asset)

            # ── PCPACK children ──
            if asset.asset_type == ".pcpack" and asset.data:
                children = asset.data
                for child in children:
                    child_type = ASSET_TYPES.get(child.asset_type, "Sub")
                    child_item = QTreeWidgetItem(item, [
                        child.name,
                        child_type,
                        f"@0x{child.offset:X}  ({child.size} bytes)",
                    ])
                    child_item.setData(0, Qt.UserRole, child)
                if children:
                    item.setText(0, f"{asset.name}  [{len(children)} resources]")

        for group_name, group_item in groups.items():
            count = group_item.childCount()
            group_item.setText(0, f"{group_name} ({count})")

        self._tree.sortItems(0, Qt.AscendingOrder)

    # ── Asset Actions ──

    def _on_asset_double_click(self, item, column):
        asset = item.data(0, Qt.UserRole)
        if not isinstance(asset, AssetEntry):
            return
        self._load_asset(asset)

    def _on_tree_context_menu(self, pos):
        item = self._tree.itemAt(pos)
        if not item:
            return
        asset = item.data(0, Qt.UserRole)
        if not isinstance(asset, AssetEntry):
            return

        menu = QMenu(self)
        if asset.asset_type == ".pcmesh":
            menu.addAction("Load Mesh").triggered.connect(lambda: self._load_mesh(asset))
        if asset.asset_type == ".pcskel":
            menu.addAction("Load Skeleton").triggered.connect(lambda: self._load_skeleton(asset))
            menu.addAction("Set as Active Skeleton").triggered.connect(lambda: self._set_active_skeleton(asset))
        if asset.asset_type == ".pcanim":
            menu.addAction("Load Animation").triggered.connect(lambda: self._load_animation(asset))
        menu.exec(self._tree.viewport().mapToGlobal(pos))

    def _load_asset(self, asset):
        if asset.asset_type == ".pcpack":
            # Expand the tree item to show children
            items = self._tree.findItems(asset.name, Qt.MatchContains | Qt.MatchRecursive, 0)
            for item in items:
                if item.data(0, Qt.UserRole) is asset:
                    item.setExpanded(True)
                    # Auto-load first mesh child if any
                    for ci in range(item.childCount()):
                        child = item.child(ci).data(0, Qt.UserRole)
                        if isinstance(child, AssetEntry) and child.asset_type == ".pcmesh":
                            self._load_mesh(child)
                            return
            return
        if asset.asset_type == ".pcmesh":
            self._load_mesh(asset)
        elif asset.asset_type == ".pcskel":
            self._load_skeleton(asset)
        elif asset.asset_type == ".pcanim":
            self._load_animation(asset)

    # ── Helpers for PCPACK sub-resources ──

    def _read_asset_bytes(self, asset):
        """Read raw bytes for an asset — handles both standalone files and PCPACK offsets."""
        if asset.parent_pack and asset.offset >= 0:
            with open(asset.parent_pack, "rb") as f:
                f.seek(asset.offset)
                return f.read(asset.size)
        else:
            with open(asset.path, "rb") as f:
                return f.read()

    def _extract_to_tmpfile(self, asset, suffix):
        """Write asset bytes to a temp file and return the path.
        Caller must delete the file when done."""
        import tempfile
        data = self._read_asset_bytes(asset)
        fd, path = tempfile.mkstemp(suffix=suffix)
        try:
            os.write(fd, data)
        finally:
            os.close(fd)
        return path

    # ── Mesh Loading ──

    def _load_mesh(self, asset):
        self._statusbar.showMessage(f"Loading mesh: {asset.name}...")
        QApplication.processEvents()

        try:
            old_stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                if asset.parent_pack:
                    # Read from PCPACK and parse from buffer
                    buf = self._read_asset_bytes(asset)
                    mesh_data_list = _read_mesh_from_buffer(buf)
                else:
                    mesh_data_list = read_meshfile(asset.path, write_obj=False)
            finally:
                sys.stdout = old_stdout

            if not mesh_data_list:
                self._statusbar.showMessage(f"No mesh data found in {asset.name}")
                return

            self._viewport.load_mesh_data(mesh_data_list)

            for md in mesh_data_list:
                if hasattr(md, 'bones') and md.bones:
                    self._viewport.mesh.bone_matrices = md.bones

        except Exception as e:
            self._statusbar.showMessage(f"Error loading mesh: {e}")
            traceback.print_exc()

    # ── Skeleton Loading ──

    def _load_skeleton(self, asset):
        self._set_active_skeleton(asset)

        if self._current_skel_data:
            bone_lines = self._extract_bone_lines_from_skel(self._current_skel_data)
            self._viewport.load_bone_lines(bone_lines)
            self._statusbar.showMessage(
                f"Loaded skeleton: {asset.name} ({len(bone_lines) // 2} bones)"
            )

    def _set_active_skeleton(self, asset):
        tmp_path = None
        try:
            if asset.parent_pack:
                tmp_path = self._extract_to_tmpfile(asset, ".pcskel")
                parse_path = tmp_path
            else:
                parse_path = asset.path

            parser = NalSkeletonParser(parse_path)
            self._current_skel_data = parser.parse()
            self._current_skel_path = asset.path
            name = self._current_skel_data.get("header", {}).get("name", asset.name)
            self._lbl_skel.setText(f"  Skeleton: {name}")
            self._statusbar.showMessage(f"Active skeleton set: {name}")
        except Exception as e:
            self._statusbar.showMessage(f"Error loading skeleton: {e}")
            traceback.print_exc()
        finally:
            if tmp_path and os.path.exists(tmp_path):
                os.unlink(tmp_path)

    def _extract_bone_lines_from_skel(self, skel_data):
        """Extract bone parent→child lines from skeleton data for visualization."""
        bone_lines = []
        bone_map = skel_data.get("bone_map", {})
        parent_map = skel_data.get("parent_map", {})

        # Try to get default pose positions from component data
        default_poses = skel_data.get("default_poses", {})

        # For character skeletons, try to build basic bone hierarchy visualization
        # from the component blocks
        components = skel_data.get("components", [])
        if not components:
            return bone_lines

        # Build a simple skeleton from torso/legs/arms bone indices
        # This is a simplified visualization - just show hierarchy connections
        for comp in components:
            comp_type = comp.get("type_id", 0)
            block = comp.get("block_data", {})
            if not block:
                continue

            bone_ixs = block.get("bone_indices", [])
            offsets = block.get("offsets", [])

            # If we have offset positions, use them for visualization
            if offsets and len(offsets) >= 2:
                for i in range(len(offsets) - 1):
                    if offsets[i] and offsets[i + 1]:
                        bone_lines.append(offsets[i])
                        bone_lines.append(offsets[i + 1])

        return bone_lines

    # ── Animation Loading ──

    def _load_animation(self, asset):
        self._statusbar.showMessage(f"Loading animation: {asset.name}...")
        QApplication.processEvents()

        tmp_path = None
        try:
            if asset.parent_pack:
                tmp_path = self._extract_to_tmpfile(asset, ".pcanim")
                parse_path = tmp_path
            else:
                parse_path = asset.path

            anim_result = open_pcanim(
                parse_path,
                skel_data=self._current_skel_data,
                decode_tracks=True,
                apply_root_motion=False,
                solve_ik=False,
            )

            animations = anim_result.get("animations", [])
            if not animations:
                self._statusbar.showMessage("No animations found in file.")
                return

            # Populate combo box
            self._anim_combo.blockSignals(True)
            self._anim_combo.clear()
            for i, anim in enumerate(animations):
                name = anim.get("name", f"anim_{i}")
                frames = anim.get("frame_count", 0)
                loop = "L" if anim.get("is_looping") else ""
                self._anim_combo.addItem(f"{name} ({frames}f {loop})", userData=anim)
            self._anim_combo.blockSignals(False)

            self._statusbar.showMessage(
                f"Loaded {len(animations)} animation(s) from {asset.name}"
            )

            if animations:
                self._anim_combo.setCurrentIndex(0)
                self._on_anim_selected(0)

        except PCANIMParseError as e:
            self._statusbar.showMessage(f"Animation parse error: {e}")
        except Exception as e:
            self._statusbar.showMessage(f"Error loading animation: {e}")
            traceback.print_exc()
        finally:
            if tmp_path and os.path.exists(tmp_path):
                os.unlink(tmp_path)

    def _on_anim_selected(self, index):
        if index < 0:
            return
        anim = self._anim_combo.itemData(index)
        if not anim:
            return

        frame_count = max(1, anim.get("frame_count", 1))
        self._frame_slider.setMaximum(frame_count - 1)
        self._frame_slider.setValue(0)
        self._lbl_frame.setText(f"0 / {frame_count - 1}")

        # Build animation visualization data
        anim_viz = self._build_anim_viz(anim)
        self._viewport.set_anim_data(anim_viz, fps=30.0)

        warnings = anim.get("decode_warnings", [])
        if warnings:
            self._statusbar.showMessage(f"Animation loaded with {len(warnings)} warning(s)")

    def _build_anim_viz(self, anim):
        """Build visualization data from decoded animation bone tracks."""
        frame_count = max(1, anim.get("frame_count", 1))
        bone_tracks = anim.get("bone_tracks", {})

        frame_bone_lines = {}

        if bone_tracks:
            for frame_idx in range(frame_count):
                lines = []
                # bone_tracks is dict: bone_name -> {"frames": [{quat, pos}, ...]}
                positions = {}
                for bone_name, track_data in bone_tracks.items():
                    frames = track_data.get("frames", [])
                    if frame_idx < len(frames):
                        fdata = frames[frame_idx]
                        pos = fdata.get("position")
                        if pos:
                            positions[bone_name] = (float(pos[0]), float(pos[1]), float(pos[2]))

                    parent = track_data.get("parent")
                    if parent and parent in positions and bone_name in positions:
                        lines.append(positions[parent])
                        lines.append(positions[bone_name])

                frame_bone_lines[frame_idx] = lines

        # Also build lines from decoded components raw data as fallback
        if not frame_bone_lines:
            components = anim.get("decoded_components", [])
            for comp in components:
                comp_frames = comp.get("frames", [])
                for fi, frame_data in enumerate(comp_frames):
                    if fi not in frame_bone_lines:
                        frame_bone_lines[fi] = []

        return {
            "frame_count": frame_count,
            "frame_bone_lines": frame_bone_lines,
        }

    def _play_anim(self):
        self._viewport.play_animation()
        self._statusbar.showMessage("Playing animation...")

    def _pause_anim(self):
        self._viewport.pause_animation()
        self._statusbar.showMessage("Animation paused.")

    def _stop_anim(self):
        self._viewport.stop_animation()
        self._frame_slider.setValue(0)
        self._statusbar.showMessage("Animation stopped.")

    def _on_frame_slider(self, value):
        self._viewport.set_frame(value)
        max_frame = self._frame_slider.maximum()
        self._lbl_frame.setText(f"{value} / {max_frame}")

    def _poll_anim_frame(self):
        """Sync slider with viewport's current animation frame."""
        if self._viewport._anim_playing and self._viewport._anim_data:
            frame = self._viewport._anim_frame
            self._frame_slider.blockSignals(True)
            self._frame_slider.setValue(frame)
            self._frame_slider.blockSignals(False)
            max_frame = self._frame_slider.maximum()
            self._lbl_frame.setText(f"{frame} / {max_frame}")


# ─── Entry Point ──────────────────────────────────────────────────────────────

def main():
    app = QApplication(sys.argv)
    app.setApplicationName("USM Viewer")
    app.setStyle("Fusion")

    # Dark palette
    from PySide6.QtGui import QPalette, QColor
    palette = QPalette()
    palette.setColor(QPalette.Window, QColor(45, 45, 48))
    palette.setColor(QPalette.WindowText, QColor(208, 208, 208))
    palette.setColor(QPalette.Base, QColor(30, 30, 34))
    palette.setColor(QPalette.AlternateBase, QColor(40, 40, 44))
    palette.setColor(QPalette.ToolTipBase, QColor(50, 50, 55))
    palette.setColor(QPalette.ToolTipText, QColor(208, 208, 208))
    palette.setColor(QPalette.Text, QColor(208, 208, 208))
    palette.setColor(QPalette.Button, QColor(55, 55, 60))
    palette.setColor(QPalette.ButtonText, QColor(208, 208, 208))
    palette.setColor(QPalette.BrightText, QColor(255, 100, 80))
    palette.setColor(QPalette.Link, QColor(80, 160, 255))
    palette.setColor(QPalette.Highlight, QColor(60, 110, 180))
    palette.setColor(QPalette.HighlightedText, QColor(240, 240, 240))
    app.setPalette(palette)

    window = USMViewer()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()