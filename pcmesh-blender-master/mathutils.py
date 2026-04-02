"""Minimal mathutils stub replacing Blender's mathutils.Matrix for standalone use."""

class Matrix:
    __slots__ = ("_rows",)

    def __init__(self, rows=None):
        if rows is None:
            self._rows = [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]]
        else:
            self._rows = [list(r) for r in rows]

    def __getitem__(self, i):
        return self._rows[i]

    def __repr__(self):
        return f"Matrix({self._rows!r})"

    def to_4x4(self):
        return Matrix(self._rows)

    def transposed(self):
        r = self._rows
        return Matrix([[r[j][i] for j in range(4)] for i in range(4)])