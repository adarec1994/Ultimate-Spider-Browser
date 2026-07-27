#include "SpiderManTool.h"
#include "NalIntegration.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <functional>
#include <cmath>
#include <sstream>

static const int MAX_BONES = 64;
static const int MAX_GLOBAL_BONES = 256;

static void Mat4Identity(float* m) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void Mat4Multiply(const float* a, const float* b, float* out) {
    float r[16];
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            r[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void Mat4TransformPoint(const float* m, const float* in, float* out) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

static void Mat4TransformVec4(const float* m, const float* in, float* out) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12] * in[3];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13] * in[3];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
    out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

static void Mat4AxisRotation(int axis, float angle, float* m) {
    Mat4Identity(m);
    float c = cosf(angle);
    float s = sinf(angle);
    if (axis == 0) {
        m[5] = c;  m[6] = s;
        m[9] = -s; m[10] = c;
    } else if (axis == 1) {
        m[0] = c;  m[2] = -s;
        m[8] = s;  m[10] = c;
    } else {
        m[0] = c;  m[1] = s;
        m[4] = -s; m[5] = c;
    }
}

static void Mat4PivotEuler(const std::array<float, 3>& angles, const float* pivot, float* out) {
    float rx[16], ry[16], rz[16], tmp[16], rot[16];
    Mat4AxisRotation(0, angles[0], rx);
    Mat4AxisRotation(1, angles[1], ry);
    Mat4AxisRotation(2, angles[2], rz);
    Mat4Multiply(ry, rx, tmp);
    Mat4Multiply(rz, tmp, rot);

    memcpy(out, rot, sizeof(float) * 16);
    float rotatedPivot[3];
    Mat4TransformPoint(rot, pivot, rotatedPivot);
    out[12] = pivot[0] - rotatedPivot[0];
    out[13] = pivot[1] - rotatedPivot[1];
    out[14] = pivot[2] - rotatedPivot[2];
}

static bool HasAnyRotation(const std::array<float, 3>& angles) {
    return fabsf(angles[0]) > 0.00001f || fabsf(angles[1]) > 0.00001f || fabsf(angles[2]) > 0.00001f;
}

struct QuatWXYZ {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

static QuatWXYZ QuatNormalize(QuatWXYZ q) {
    float len2 = q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z;
    if (len2 <= 1e-20f) return {};
    float inv = 1.0f / sqrtf(len2);
    q.w *= inv;
    q.x *= inv;
    q.y *= inv;
    q.z *= inv;
    return q;
}

static QuatWXYZ QuatFromXYZ(float x, float y, float z) {
    float w2 = 1.0f - (x * x + y * y + z * z);
    return QuatNormalize({sqrtf(fabsf(w2)), x, y, z});
}

static QuatWXYZ QuatFromNal(const NalQuat& q) {
    return QuatNormalize({q.w, q.x, q.y, q.z});
}

static QuatWXYZ QuatInverse(QuatWXYZ q) {
    q = QuatNormalize(q);
    return {q.w, -q.x, -q.y, -q.z};
}

static QuatWXYZ QuatMul(QuatWXYZ a, QuatWXYZ b) {
    a = QuatNormalize(a);
    b = QuatNormalize(b);
    return QuatNormalize({
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w
    });
}

static float QuatDot(QuatWXYZ a, QuatWXYZ b) {
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

static QuatWXYZ QuatSlerp(QuatWXYZ a, QuatWXYZ b, float t) {
    a = QuatNormalize(a);
    b = QuatNormalize(b);
    float dot = QuatDot(a, b);
    if (dot < 0.0f) {
        b = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    dot = std::max(-1.0f, std::min(1.0f, dot));
    if (dot > 0.9995f) {
        return QuatNormalize({
            a.w + t * (b.w - a.w), a.x + t * (b.x - a.x),
            a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)
        });
    }
    float theta = acosf(dot);
    float sinTheta = sinf(theta);
    float wa = sinf((1.0f - t) * theta) / sinTheta;
    float wb = sinf(t * theta) / sinTheta;
    return QuatNormalize({
        wa * a.w + wb * b.w, wa * a.x + wb * b.x,
        wa * a.y + wb * b.y, wa * a.z + wb * b.z
    });
}

static float SampleTrack(const std::vector<float>& a, const std::vector<float>& b,
                         int index, float t) {
    if (index < 0 || index >= (int)a.size()) return 0.0f;
    float av = a[index];
    if (index >= (int)b.size()) return av;
    return av + t * (b[index] - av);
}

static bool ConsumeSampleVec3(const std::vector<float>& a, const std::vector<float>& b,
                              int& cursor, float t, float out[3]) {
    if (cursor + 2 >= (int)a.size()) return false;
    out[0] = SampleTrack(a, b, cursor++, t);
    out[1] = SampleTrack(a, b, cursor++, t);
    out[2] = SampleTrack(a, b, cursor++, t);
    return true;
}

static bool ConsumeSampleQuat(const std::vector<float>& a, const std::vector<float>& b,
                              int& cursor, float t, QuatWXYZ& out) {
    if (cursor + 2 >= (int)a.size()) return false;
    QuatWXYZ qa = QuatFromXYZ(a[cursor], a[cursor + 1], a[cursor + 2]);
    QuatWXYZ qb = qa;
    if (cursor + 2 < (int)b.size())
        qb = QuatFromXYZ(b[cursor], b[cursor + 1], b[cursor + 2]);
    cursor += 3;
    out = QuatSlerp(qa, qb, t);
    return true;
}

static QuatWXYZ QuatAxisAngle(int axis, float angle) {
    float half = angle * 0.5f;
    float s = sinf(half);
    float c = cosf(half);
    if (axis == 0) return QuatNormalize({c, s, 0.0f, 0.0f});
    if (axis == 1) return QuatNormalize({c, 0.0f, s, 0.0f});
    return QuatNormalize({c, 0.0f, 0.0f, s});
}

static QuatWXYZ QuatFromYZAngles(float yAngle, float zAngle) {
    return QuatMul(QuatAxisAngle(1, yAngle), QuatAxisAngle(2, zAngle));
}

static float FingerTipHingeAngle(float angle, bool isThumb) {
    if (!isThumb) return angle;
    float tip = 2.0f * angle;
    float halfPi = 1.57079632679f;
    if (tip > halfPi) return halfPi;
    if (tip < -halfPi) return -halfPi;
    return tip;
}

static void Mat4FromQuat(QuatWXYZ q, float* m) {
    q = QuatNormalize(q);
    Mat4Identity(m);

    float x = -q.x;
    float y = -q.y;
    float z = -q.z;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = q.w * x;
    float wy = q.w * y;
    float wz = q.w * z;

    m[0]  = 1.0f - 2.0f * (yy + zz);
    m[1]  = 2.0f * (xy + wz);
    m[2]  = 2.0f * (xz - wy);
    m[4]  = 2.0f * (xy - wz);
    m[5]  = 1.0f - 2.0f * (xx + zz);
    m[6]  = 2.0f * (yz + wx);
    m[8]  = 2.0f * (xz + wy);
    m[9]  = 2.0f * (yz - wx);
    m[10] = 1.0f - 2.0f * (xx + yy);
}

static void Mat4BuildTwistLineXform(const std::array<float, 3>& pos, float angle, float* m) {
    float s = sinf(angle);
    float c = cosf(angle);
    Mat4Identity(m);

    m[5] = c;
    m[6] = -s;
    m[9] = s;
    m[10] = c;
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
}

static float WrapPi(float angle) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTau = 2.0f * kPi;
    while (angle > kPi) angle -= kTau;
    while (angle < -kPi) angle += kTau;
    return angle;
}

static float ExtractForearmTwist(QuatWXYZ handQuat, bool left) {

    handQuat = QuatNormalize(handQuat);
    float axial = 2.0f * atan2f(handQuat.x, handQuat.w);
    float reference = left ? -1.57079632679f : 1.57079632679f;
    return WrapPi(axial - reference);
}

static void Mat4Fing52HingeLocal(float angle, const std::array<float, 3>& pos, float* m) {
    float s = sinf(angle);
    float c = cosf(angle);
    Mat4Identity(m);
    m[0] = c;
    m[2] = s;
    m[8] = -s;
    m[10] = c;
    m[12] = pos[0];
    m[13] = pos[1];
    m[14] = pos[2];
}

static void Vec3Sub(const float* a, const float* b, float* out) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void Vec3Scale(const float* a, float s, float* out) {
    out[0] = a[0] * s;
    out[1] = a[1] * s;
    out[2] = a[2] * s;
}

static float Vec3Len(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void Vec3Normalize(float* v) {
    float len = Vec3Len(v);
    if (len <= 1e-12f) {
        v[0] = 0.0f;
        v[1] = 0.0f;
        v[2] = 0.0f;
        return;
    }
    float inv = 1.0f / len;
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

static float Vec3DotLocal(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void Vec3CrossLocal(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void Mat4ComposeBasis(const float* xAxis, const float* yAxis, const float* zAxis,
                             const float* pos, float* out) {
    Mat4Identity(out);
    out[0] = xAxis[0]; out[1] = xAxis[1]; out[2] = xAxis[2];
    out[4] = yAxis[0]; out[5] = yAxis[1]; out[6] = yAxis[2];
    out[8] = zAxis[0]; out[9] = zAxis[1]; out[10] = zAxis[2];
    out[12] = pos[0]; out[13] = pos[1]; out[14] = pos[2];
}

static void Mat4AxisVec(const float* m, int axis, float* out) {
    const int base = axis * 4;
    out[0] = m[base + 0];
    out[1] = m[base + 1];
    out[2] = m[base + 2];
}

static void Mat4ProjectPointOntoXform(const float* point, const float* xform, float* out) {
    out[0] = xform[12] + point[0] * xform[0] + point[1] * xform[4] + point[2] * xform[8];
    out[1] = xform[13] + point[0] * xform[1] + point[1] * xform[5] + point[2] * xform[9];
    out[2] = xform[14] + point[0] * xform[2] + point[1] * xform[6] + point[2] * xform[10];
}

static void RemapNalIkRows(float* m) {
    float x[3] = {m[0], m[1], m[2]};
    float y[3] = {m[4], m[5], m[6]};
    float z[3] = {m[8], m[9], m[10]};
    m[0] = -x[0]; m[1] = -x[1]; m[2] = -x[2];
    m[4] = -z[0]; m[5] = -z[1]; m[6] = -z[2];
    m[8] = -y[0]; m[9] = -y[1]; m[10] = -y[2];
}

static void SolveNalIk(const float* baseModelMatrix,
                       const std::array<float, 3>& baseJoint,
                       const float* targetModelMatrix,
                       const NalIKData& ikData,
                       float spinAngle,
                       bool armPlaneCallback,
                       bool mirrorArm,
                       float* upperOut,
                       float* lowerOut) {
    float baseJointArr[3] = {baseJoint[0], baseJoint[1], baseJoint[2]};
    float modelBaseJoint[3];
    Mat4ProjectPointOntoXform(baseJointArr, baseModelMatrix, modelBaseJoint);

    float targetPos[3] = {targetModelMatrix[12], targetModelMatrix[13], targetModelMatrix[14]};
    float targetDir[3];
    Vec3Sub(targetPos, modelBaseJoint, targetDir);
    float dist = Vec3Len(targetDir);
    if (dist <= 1e-12f) dist = 1e-12f;
    Vec3Scale(targetDir, 1.0f / dist, targetDir);

    float cosUpper = dist * ikData.fUpperIKc + (1.0f / dist) * ikData.fUpperIKInvc;
    float cosLower = dist * ikData.fLowerIKc + (1.0f / dist) * ikData.fLowerIKInvc;
    cosUpper = std::max(-1.0f, std::min(1.0f, cosUpper));
    cosLower = std::max(-1.0f, std::min(1.0f, cosLower));
    float sinUpper = sqrtf(std::max(0.0f, 1.0f - cosUpper * cosUpper));
    float sinLower = sqrtf(std::max(0.0f, 1.0f - cosLower * cosLower));

    float midDir[3];
    if (!armPlaneCallback) {
        float targetY[3];
        Mat4AxisVec(targetModelMatrix, 1, targetY);
        Vec3CrossLocal(targetDir, targetY, midDir);
    } else {

        float row0[3], row1[3], row2[3];
        Mat4AxisVec(baseModelMatrix, 0, row0);
        Mat4AxisVec(baseModelMatrix, 1, row1);
        Mat4AxisVec(baseModelMatrix, 2, row2);
        if (mirrorArm) {
            row1[0] = -row1[0]; row1[1] = -row1[1]; row1[2] = -row1[2];
            Vec3CrossLocal(row1, targetDir, midDir);
        } else {
            Vec3CrossLocal(targetDir, row1, midDir);
        }
        float sign = Vec3DotLocal(targetDir, row1);
        float adjusted[3];
        if (sign < 0.0f) {
            float negSum[3] = {-row0[0] - row2[0], -row0[1] - row2[1], -row0[2] - row2[2]};
            adjusted[0] = midDir[0] * (sign + 1.0f) + negSum[0] * (-sign);
            adjusted[1] = midDir[1] * (sign + 1.0f) + negSum[1] * (-sign);
            adjusted[2] = midDir[2] * (sign + 1.0f) + negSum[2] * (-sign);
        } else {
            float diff[3] = {row2[0] - row0[0], row2[1] - row0[1], row2[2] - row0[2]};
            adjusted[0] = midDir[0] * (1.0f - sign) + diff[0] * sign;
            adjusted[1] = midDir[1] * (1.0f - sign) + diff[1] * sign;
            adjusted[2] = midDir[2] * (1.0f - sign) + diff[2] * sign;
        }
        memcpy(midDir, adjusted, sizeof(adjusted));
    }

    float zAxis[3], yAxis[3], xAxis[3] = {targetDir[0], targetDir[1], targetDir[2]};
    Vec3CrossLocal(targetDir, midDir, zAxis);
    Vec3Normalize(zAxis);
    Vec3CrossLocal(zAxis, targetDir, yAxis);
    float basis[16];
    Mat4ComposeBasis(xAxis, yAxis, zAxis, modelBaseJoint, basis);

    float sinTwist = sinf(spinAngle);
    float cosTwist = cosf(spinAngle);
    float zero[3] = {0.0f, 0.0f, 0.0f};
    float upperX[3] = {cosUpper, sinUpper * cosTwist, sinUpper * sinTwist};
    float upperY[3] = {-sinUpper, cosUpper * cosTwist, cosUpper * sinTwist};
    float upperZ[3] = {0.0f, -sinTwist, cosTwist};
    float upperLocal[16];
    Mat4ComposeBasis(upperX, upperY, upperZ, zero, upperLocal);
    Mat4Multiply(basis, upperLocal, upperOut);

    float lowerPos[3] = {
        ikData.fUpperArmLength * cosUpper,
        cosTwist * (ikData.fUpperArmLength * sinUpper),
        sinTwist * (ikData.fUpperArmLength * sinUpper)
    };
    float lowerX[3] = {cosLower, -(sinLower * cosTwist), -(sinLower * sinTwist)};
    float lowerY[3] = {sinLower, cosLower * cosTwist, cosLower * sinTwist};
    float lowerZ[3] = {0.0f, -sinTwist, cosTwist};
    float lowerLocal[16];
    Mat4ComposeBasis(lowerX, lowerY, lowerZ, lowerPos, lowerLocal);
    Mat4Multiply(basis, lowerLocal, lowerOut);

    RemapNalIkRows(upperOut);
    RemapNalIkRows(lowerOut);
}

static QuatWXYZ QuatFromMat4(const float* m) {
    float m00 = m[0],  m01 = m[4],  m02 = m[8];
    float m10 = m[1],  m11 = m[5],  m12 = m[9];
    float m20 = m[2],  m21 = m[6],  m22 = m[10];

    QuatWXYZ q;
    float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = sqrtf(std::max(1e-20f, 1.0f + m00 - m11 - m22)) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = sqrtf(std::max(1e-20f, 1.0f + m11 - m00 - m22)) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = sqrtf(std::max(1e-20f, 1.0f + m22 - m00 - m11)) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }

    return QuatNormalize({q.w, -q.x, -q.y, -q.z});
}

static bool GetDefaultQuat(const NalComponentData* comp, int index, QuatWXYZ& out) {
    if (!comp || index < 0 || index >= (int)comp->default_pose.quats.size()) return false;
    out = QuatFromNal(comp->default_pose.quats[index]);
    return true;
}

struct TentaclePreviewChain {
    std::string name;
    std::vector<std::array<float, 3>> controlPoints;
    float diameter = 0.0f;
    float activity = 0.0f;
    float pull = 0.0f;
    bool tongue = false;
};

static std::array<float, 3> TentacleCatmullRom(
    const std::array<float, 3>& p0, const std::array<float, 3>& p1,
    const std::array<float, 3>& p2, const std::array<float, 3>& p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    std::array<float, 3> out{};
    for (int axis = 0; axis < 3; ++axis) {
        out[axis] = 0.5f * ((2.0f * p1[axis]) +
            (-p0[axis] + p2[axis]) * t +
            (2.0f * p0[axis] - 5.0f * p1[axis] + 4.0f * p2[axis] - p3[axis]) * t2 +
            (-p0[axis] + 3.0f * p1[axis] - 3.0f * p2[axis] + p3[axis]) * t3);
    }
    return out;
}

static void UpdateTentaclePreviewMesh(RenderMesh& mesh,
                                      const std::vector<TentaclePreviewChain>& chains,
                                      bool carnage) {
    constexpr int kCurveStepsPerSpan = 4;
    constexpr int kTongueCurveStepsPerSpan = 8;
    constexpr int kTubeSides = 8;
    constexpr int kTongueSides = 12;
    constexpr int kFloatsPerVertex = 20;
    constexpr float kTau = 6.2831853071795864769f;

    std::vector<float> vertices;
    std::vector<uint16_t> indices;
    mesh.positions.clear();
    mesh.normals.clear();
    mesh.uvs.clear();
    mesh.colors.clear();
    mesh.bboxMin[0] = mesh.bboxMin[1] = mesh.bboxMin[2] =
        std::numeric_limits<float>::max();
    mesh.bboxMax[0] = mesh.bboxMax[1] = mesh.bboxMax[2] =
        std::numeric_limits<float>::lowest();

    for (const auto& chain : chains) {
        if (chain.controlPoints.size() < 2 || chain.diameter <= 0.0001f) continue;

        const bool carnageBlade = carnage && !chain.tongue;
        const int curveSteps = (chain.tongue || carnageBlade)
            ? kTongueCurveStepsPerSpan : kCurveStepsPerSpan;
        const int ringSides = chain.tongue ? kTongueSides : (carnageBlade ? 10 : kTubeSides);
        std::vector<std::array<float, 3>> curve;
        curve.reserve((chain.controlPoints.size() - 1) * curveSteps + 1);
        for (size_t span = 0; span + 1 < chain.controlPoints.size(); ++span) {
            const auto& p0 = chain.controlPoints[span == 0 ? span : span - 1];
            const auto& p1 = chain.controlPoints[span];
            const auto& p2 = chain.controlPoints[span + 1];
            const auto& p3 = chain.controlPoints[
                std::min(span + 2, chain.controlPoints.size() - 1)];
            for (int step = 0; step < curveSteps; ++step) {
                curve.push_back(TentacleCatmullRom(
                    p0, p1, p2, p3,
                    static_cast<float>(step) / static_cast<float>(curveSteps)));
            }
        }
        curve.push_back(chain.controlPoints.back());

        if (curve.size() * ringSides + vertices.size() / kFloatsPerVertex > 65535)
            break;
        const uint16_t baseVertex = static_cast<uint16_t>(vertices.size() / kFloatsPerVertex);
        float transportedSide[3] = {0.0f, 0.0f, 0.0f};
        bool hasTransportedSide = false;

        for (size_t pointIndex = 0; pointIndex < curve.size(); ++pointIndex) {
            const auto& previous = curve[pointIndex == 0 ? 0 : pointIndex - 1];
            const auto& next = curve[std::min(pointIndex + 1, curve.size() - 1)];
            float tangent[3] = {
                next[0] - previous[0], next[1] - previous[1], next[2] - previous[2]};
            Vec3Normalize(tangent);
            if (Vec3Len(tangent) < 0.5f) {
                tangent[0] = 0.0f; tangent[1] = 1.0f; tangent[2] = 0.0f;
            }

            float side[3];
            if (hasTransportedSide) {

                const float projection = transportedSide[0] * tangent[0] +
                    transportedSide[1] * tangent[1] + transportedSide[2] * tangent[2];
                side[0] = transportedSide[0] - tangent[0] * projection;
                side[1] = transportedSide[1] - tangent[1] * projection;
                side[2] = transportedSide[2] - tangent[2] * projection;
                Vec3Normalize(side);
                if (Vec3Len(side) < 0.5f) hasTransportedSide = false;
            }
            if (!hasTransportedSide) {
                float reference[3] = {0.0f, 1.0f, 0.0f};
                if (fabsf(tangent[1]) > 0.9f) {
                    reference[0] = 1.0f; reference[1] = 0.0f;
                }
                Vec3CrossLocal(tangent, reference, side);
                Vec3Normalize(side);
                hasTransportedSide = true;
            }
            transportedSide[0] = side[0];
            transportedSide[1] = side[1];
            transportedSide[2] = side[2];
            float up[3];
            Vec3CrossLocal(side, tangent, up);
            Vec3Normalize(up);

            const float along = curve.size() > 1
                ? static_cast<float>(pointIndex) / static_cast<float>(curve.size() - 1)
                : 0.0f;

            const float diameter = std::min(chain.diameter, 4.0f);
            const float radius = diameter * 0.5f * (1.0f - 0.72f * along);

            const float tongueRoot = std::min(1.0f, 0.58f + along * 3.0f);
            const float tongueTip = along < 0.52f
                ? 1.0f
                : std::max(0.0f, 1.0f - (along - 0.52f) / 0.48f);
            const float tongueProfile = tongueRoot * tongueTip;
            const float halfWidth = diameter * 0.22f * tongueProfile;
            const float halfThickness = diameter * 0.052f * tongueProfile;

            const float carnageRoot = std::min(1.0f, 0.72f + along * 2.0f);
            const float carnageTip = along < 0.44f
                ? 1.0f
                : powf(std::max(0.0f, 1.0f - (along - 0.44f) / 0.56f), 1.18f);
            const float carnageProfile = carnageRoot * carnageTip;
            const float carnageHalfWidth = diameter * 0.26f * carnageProfile;
            const float carnageHalfThickness = diameter * 0.070f * carnageProfile;
            const float baseColor[3] = {
                chain.tongue ? 0.55f : (carnage ? 0.62f : 0.16f),
                chain.tongue ? 0.060f : (carnage ? 0.035f : 0.055f),
                chain.tongue ? 0.095f : (carnage ? 0.025f : 0.23f)};

            for (int sideIndex = 0; sideIndex < ringSides; ++sideIndex) {
                const float angle = kTau * static_cast<float>(sideIndex) /
                    static_cast<float>(ringSides);
                const float cosine = cosf(angle);
                const float sine = sinf(angle);
                float normal[3] = {
                    side[0] * cosine + up[0] * sine,
                    side[1] * cosine + up[1] * sine,
                    side[2] * cosine + up[2] * sine};
                float offset[3] = {normal[0] * radius, normal[1] * radius, normal[2] * radius};
                if (chain.tongue || carnageBlade) {
                    const float profileWidth = chain.tongue ? halfWidth : carnageHalfWidth;
                    const float profileThickness = chain.tongue ? halfThickness : carnageHalfThickness;
                    offset[0] = side[0] * cosine * profileWidth + up[0] * sine * profileThickness;
                    offset[1] = side[1] * cosine * profileWidth + up[1] * sine * profileThickness;
                    offset[2] = side[2] * cosine * profileWidth + up[2] * sine * profileThickness;

                    const float safeWidth = std::max(profileWidth, 0.0001f);
                    const float safeThickness = std::max(profileThickness, 0.0001f);
                    normal[0] = side[0] * cosine / safeWidth + up[0] * sine / safeThickness;
                    normal[1] = side[1] * cosine / safeWidth + up[1] * sine / safeThickness;
                    normal[2] = side[2] * cosine / safeWidth + up[2] * sine / safeThickness;
                    Vec3Normalize(normal);
                }
                const float position[3] = {
                    curve[pointIndex][0] + offset[0],
                    curve[pointIndex][1] + offset[1],
                    curve[pointIndex][2] + offset[2]};

                const float crease = chain.tongue && sine > 0.7f ? 0.72f : 1.0f;

                const float packed[kFloatsPerVertex] = {
                    position[0], position[1], position[2],
                    normal[0], normal[1], normal[2],
                    static_cast<float>(sideIndex) / static_cast<float>(ringSides), along,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f,
                    baseColor[0] * crease, baseColor[1] * crease,
                    baseColor[2] * crease, 1.0f};
                vertices.insert(vertices.end(), packed, packed + kFloatsPerVertex);
                mesh.positions.insert(mesh.positions.end(), position, position + 3);
                mesh.normals.insert(mesh.normals.end(), normal, normal + 3);
                mesh.uvs.push_back(packed[6]);
                mesh.uvs.push_back(packed[7]);
                mesh.colors.insert(mesh.colors.end(), packed + 16, packed + 20);
                for (int axis = 0; axis < 3; ++axis) {
                    mesh.bboxMin[axis] = std::min(mesh.bboxMin[axis], position[axis]);
                    mesh.bboxMax[axis] = std::max(mesh.bboxMax[axis], position[axis]);
                }
            }
        }

        for (size_t ring = 0; ring + 1 < curve.size(); ++ring) {
            for (int sideIndex = 0; sideIndex < ringSides; ++sideIndex) {
                const uint16_t a = static_cast<uint16_t>(baseVertex + ring * ringSides + sideIndex);
                const uint16_t b = static_cast<uint16_t>(baseVertex + ring * ringSides +
                    (sideIndex + 1) % ringSides);
                const uint16_t c = static_cast<uint16_t>(a + ringSides);
                const uint16_t d = static_cast<uint16_t>(b + ringSides);
                indices.push_back(a); indices.push_back(c); indices.push_back(b);
                indices.push_back(b); indices.push_back(c); indices.push_back(d);
            }
        }
    }

    mesh.indices = indices;
    mesh.indexCount = static_cast<int>(indices.size());
    mesh.mode = GL_TRIANGLES;
    mesh.meshName = "Animated Tentacles";
    mesh.skipPicking = true;
    mesh.isHidden = false;
    mesh.isTranslucent = false;
    mesh.blendMode = NGLBM_OPAQUE;
    mesh.textureId = 0;

    if (vertices.empty() || indices.empty()) return;
    if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
    if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
    if (!mesh.ebo) glGenBuffers(1, &mesh.ebo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_DYNAMIC_DRAW);
    const GLsizei stride = kFloatsPerVertex * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, (void*)(8 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)(12 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)(16 * sizeof(float)));
    glEnableVertexAttribArray(5);
    glBindVertexArray(0);
}

struct DDS_PIXELFORMAT {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DDS_HEADER {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

unsigned int SpiderManTool::LoadTextureFromData(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(DDS_HEADER) + 4) return 0;
    uint32_t magic = *(uint32_t*)data.data();
    if (magic != 0x20534444) return 0;

    const DDS_HEADER* header = (const DDS_HEADER*)(data.data() + 4);
    int width = header->dwWidth;
    int height = header->dwHeight;
    uint32_t pfFlags = header->ddspf.dwFlags;
    uint32_t fourCC = header->ddspf.dwFourCC;
    uint32_t rgbBits = header->ddspf.dwRGBBitCount;
    const uint8_t* pixelData = data.data() + 128;
    size_t pixelDataSize = data.size() - 128;

    unsigned int tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (pfFlags & 0x4) {
        GLenum format = 0;
        if (fourCC == 0x31545844) format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        else if (fourCC == 0x33545844) format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        else if (fourCC == 0x35545844) format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        if (format == 0) { glDeleteTextures(1, &tex); return 0; }
        uint32_t blockSize = (format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT) ? 8 : 16;
        uint32_t imageSize = ((width + 3) / 4) * ((height + 3) / 4) * blockSize;
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, imageSize, pixelData);
    } else if (pfFlags & 0x40) {
        uint32_t aMask = header->ddspf.dwABitMask;
        if (rgbBits == 32) {
            size_t numPx = (size_t)width * height;
            std::vector<uint8_t> rgba(numPx * 4);
            for (size_t i = 0; i < numPx && i * 4 + 3 < pixelDataSize; i++) {
                rgba[i*4+0] = pixelData[i*4+2]; rgba[i*4+1] = pixelData[i*4+1];
                rgba[i*4+2] = pixelData[i*4+0]; rgba[i*4+3] = aMask ? pixelData[i*4+3] : 255;
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else if (rgbBits == 24) {
            size_t numPx = (size_t)width * height;
            std::vector<uint8_t> rgb(numPx * 3);
            for (size_t i = 0; i < numPx && i * 3 + 2 < pixelDataSize; i++) {
                rgb[i*3+0] = pixelData[i*3+2]; rgb[i*3+1] = pixelData[i*3+1]; rgb[i*3+2] = pixelData[i*3+0];
            }
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
        } else { glDeleteTextures(1, &tex); return 0; }
    } else if (pfFlags & 0x20000) {
        size_t numPx = (size_t)width * height;
        std::vector<uint8_t> rgba(numPx * 4);
        if (rgbBits == 8) {
            for (size_t i = 0; i < numPx && i < pixelDataSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=pixelData[i]; rgba[i*4+3]=255;
            }
        } else if (rgbBits == 16) {
            for (size_t i = 0; i < numPx && i*2+1 < pixelDataSize; i++) {
                rgba[i*4+0]=rgba[i*4+1]=rgba[i*4+2]=pixelData[i*2]; rgba[i*4+3]=pixelData[i*2+1];
            }
        } else { glDeleteTextures(1, &tex); return 0; }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else { glDeleteTextures(1, &tex); return 0; }

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

unsigned int SpiderManTool::LoadTextureFromHash(uint32_t hash) {
    if (textureCache.count(hash)) return textureCache[hash];

    int foundIdx = -1;
    for(int i=0; i<(int)entries.size(); i++) {
        if (entries[i].hash == hash && entries[i].isDds) {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx == -1) {
        textureCache[hash] = 0;
        return 0;
    }

    const auto& e = entries[foundIdx];
    if (e.offset + e.size > pcPackData.size()) return 0;

    std::vector<uint8_t> ddsData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
    unsigned int tex = LoadTextureFromData(ddsData);

    if (tex != 0) {
        textureCache[hash] = tex;
    }
    return tex;
}

unsigned int SpiderManTool::LoadTextureByName(const std::string& textureName) {
    if (textureName.empty()) return 0;

    std::string nameLower = StrToLower(textureName);

    if (textureNameCache.count(nameLower)) {
        return textureNameCache[nameLower];
    }

    if (!textureAnimationCache.count(nameLower)) {
        textureAnimationCache[nameLower] = {};
        const FileEntry* iflEntry = nullptr;
        for (const auto& entry : entries) {
            if (entry.type != RES_KEY_IFL) continue;
            std::string entryName = StrToLower(entry.name);
            while (entryName.size() >= 4 && entryName.substr(entryName.size() - 4) == ".ifl") {
                entryName.resize(entryName.size() - 4);
            }
            if (entryName == nameLower) {
                iflEntry = &entry;
                break;
            }
        }
        if (iflEntry && static_cast<size_t>(iflEntry->offset) + iflEntry->size <= pcPackData.size()) {
            const char* begin = reinterpret_cast<const char*>(pcPackData.data() + iflEntry->offset);
            std::string iflText(begin, begin + iflEntry->size);
            std::istringstream lines(iflText);
            std::string line;
            auto& frames = textureAnimationCache[nameLower];
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::istringstream fields(line);
                std::string frameName;
                int repetitions = 1;
                if (!(fields >> frameName)) continue;
                fields >> repetitions;
                repetitions = std::max(1, std::min(repetitions, 1024));
                const size_t dot = frameName.find('.');
                if (dot != std::string::npos) frameName.resize(dot);
                const unsigned int frameTexture = LoadTextureByName(frameName);
                if (frameTexture == 0) continue;
                for (int repeat = 0; repeat < repetitions && frames.size() < 1024; ++repeat) {
                    frames.push_back(frameTexture);
                }
            }
            if (!frames.empty()) {
                textureNameCache[nameLower] = frames.front();
                return frames.front();
            }
        }
    } else if (!textureAnimationCache[nameLower].empty()) {
        return textureAnimationCache[nameLower].front();
    }

    int foundIdx = -1;
    for (int i = 0; i < (int)entries.size(); i++) {
        if (!entries[i].isDds) continue;

        std::string entryName = StrToLower(entries[i].name);
        std::string entryBase = entryName;
        if (entryBase.size() > 4 && entryBase.substr(entryBase.size() - 4) == ".dds") {
            entryBase = entryBase.substr(0, entryBase.size() - 4);
        }

        if (entryBase == nameLower || entryName == nameLower ||
            entryBase == nameLower + ".dds" || entryName == nameLower + ".dds") {
            foundIdx = i;
            break;
        }
    }

    if (foundIdx != -1) {
        const auto& e = entries[foundIdx];
        if (e.offset + e.size <= pcPackData.size()) {
            std::vector<uint8_t> ddsData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    if (globalTextureNameIndex.count(nameLower)) {
        auto& loc = globalTextureNameIndex[nameLower];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    uint32_t hash1 = CalculateCRC32(nameLower + ".dds");
    if (globalTextureIndex.count(hash1)) {
        auto& loc = globalTextureIndex[hash1];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    uint32_t hash2 = CalculateCRC32(nameLower);
    if (globalTextureIndex.count(hash2)) {
        auto& loc = globalTextureIndex[hash2];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();

            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    uint32_t hashTga = CalculateCRC32(nameLower + ".tga");
    if (globalTextureIndex.count(hashTga)) {
        auto& loc = globalTextureIndex[hashTga];
        std::ifstream texFile(loc.packPath, std::ios::binary);
        if (texFile.is_open()) {
            texFile.seekg(loc.offset);
            std::vector<uint8_t> ddsData(loc.size);
            texFile.read((char*)ddsData.data(), loc.size);
            texFile.close();
            unsigned int tex = LoadTextureFromData(ddsData);
            if (tex != 0) {
                textureNameCache[nameLower] = tex;
                return tex;
            }
        }
    }

    auto tryFallback = [&](const std::string& candidate) -> unsigned int {
        if (candidate.empty() || candidate == nameLower) return 0;
        unsigned int tex = LoadTextureByName(candidate);
        if (tex != 0) {

            textureNameCache[nameLower] = tex;
        }
        return tex;
    };

    if (nameLower == "us_char_spheremap_ink") {
        unsigned int tex = tryFallback("us_char_spheremap_inkb");
        if (tex != 0) return tex;
    } else if (nameLower == "us_char_spheremap_noink") {
        unsigned int tex = tryFallback("us_char_spheremap_noink_3");
        if (tex != 0) return tex;
        tex = tryFallback("us_char_spheremap_noink_2b");
        if (tex != 0) return tex;
    }
    if (nameLower.compare(0, 4, "nat_") != 0) {

        size_t under = nameLower.find('_');
        if (under == 2 || under == 3) {
            std::string rest = nameLower.substr(under + 1);
            if (!rest.empty()) {
                unsigned int tex = tryFallback("nat_" + rest);
                if (tex != 0) return tex;
                tex = tryFallback(rest);
                if (tex != 0) return tex;
            }
        }
    }

    textureNameCache[nameLower] = 0;
    return 0;
}

void SpiderManTool::InitModelPreview() {
    if (viewportTextureId == 0) {
        if (msFbo != 0) glDeleteFramebuffers(1, &msFbo);
        if (msColor != 0) glDeleteTextures(1, &msColor);
        if (msRbo != 0) glDeleteRenderbuffers(1, &msRbo);
        if (modelFbo != 0) glDeleteFramebuffers(1, &modelFbo);

        if (viewportTextureId != 0) glDeleteTextures(1, &viewportTextureId);

        int width = 3840;
        int height = 2160;

        glGenFramebuffers(1, &msFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);

        glGenTextures(1, &msColor);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, msColor);
        glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, 4, GL_RGBA, width, height, GL_TRUE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D_MULTISAMPLE, msColor, 0);

        glGenRenderbuffers(1, &msRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, msRbo);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, 4, GL_DEPTH24_STENCIL8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, msRbo);

        glGenFramebuffers(1, &modelFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, modelFbo);

        glGenTextures(1, &viewportTextureId);
        glBindTexture(GL_TEXTURE_2D, viewportTextureId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, viewportTextureId, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (modelProgram != 0 && skeletonProgram != 0) return;

    const char* vShaderCode = "#version 130\n"
        "in vec3 pos;\n"
        "in vec3 normal;\n"
        "in vec2 texCoord;\n"
        "in vec4 boneIndices;\n"
        "in vec4 boneWeights;\n"
        "in vec4 vertColor;\n"
        "in vec4 instanceTransform0;\n"
        "in vec4 instanceTransform1;\n"
        "in vec4 instanceTransform2;\n"
        "in vec4 instanceTransform3;\n"
        "in float instanceIndex;\n"
        "out vec2 TexCoord;\n"
        "out vec4 VertColor;\n"
        "out vec3 WorldPos;\n"
        "out vec3 WorldNormal;\n"
        "out vec3 ViewNormal;\n"
        "out float InstanceIndex;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "uniform bool useSkinning;\n"
        "uniform bool useInstancing;\n"
        "uniform mat4 boneMatrices[64];\n"

        "uniform bool isWater;\n"
        "uniform float time;\n"
        "void main() {\n"
        "    vec4 skinnedPos;\n"
        "    vec3 skinnedNormal = normal;\n"
        "    if (useSkinning && (boneWeights.x + boneWeights.y + boneWeights.z + boneWeights.w) > 0.01) {\n"
        "        mat4 skinMat = mat4(0.0);\n"
        "        float usedWeight = 0.0;\n"
        "        for (int i = 0; i < 4; i++) {\n"
        "            int idx = int(boneIndices[i] + 0.5);\n"
        "            float w = boneWeights[i];\n"
        "            if (idx >= 0 && idx < 64 && w > 0.0) {\n"
        "                skinMat += w * boneMatrices[idx];\n"
        "                usedWeight += w;\n"
        "            }\n"
        "        }\n"
        "        if (usedWeight > 0.001) {\n"
        "            if (usedWeight < 0.999) skinMat += (1.0 - usedWeight) * mat4(1.0);\n"
        "            skinnedPos = skinMat * vec4(pos, 1.0);\n"
        "            skinnedNormal = mat3(skinMat) * normal;\n"
        "        } else {\n"
        "            skinnedPos = vec4(pos, 1.0);\n"
        "        }\n"
        "    } else {\n"
        "        skinnedPos = vec4(pos, 1.0);\n"
        "    }\n"
        "    if (isWater) {\n"

        "        float wave1 = sin(skinnedPos.x * 0.0040 + time * 0.7);\n"
        "        float wave2 = sin(skinnedPos.z * 0.0055 + time * 0.9);\n"
        "        float wave3 = sin((skinnedPos.x + skinnedPos.z) * 0.012 + time * 1.6);\n"
        "        skinnedPos.y += wave1 * 6.0 + wave2 * 5.0 + wave3 * 1.5;\n"
        "    }\n"
        "    mat4 instanceTransform = mat4(instanceTransform0, instanceTransform1, instanceTransform2, instanceTransform3);\n"
        "    vec4 placedPos = useInstancing ? (instanceTransform * skinnedPos) : skinnedPos;\n"
        "    vec4 worldPos = model * placedPos;\n"
        "    WorldPos = worldPos.xyz;\n"
        "    vec3 placedNormal = useInstancing ? (mat3(instanceTransform) * skinnedNormal) : skinnedNormal;\n"
        "    WorldNormal = normalize(mat3(model) * placedNormal);\n"

        "    ViewNormal = normalize(mat3(view) * WorldNormal);\n"
        "    InstanceIndex = useInstancing ? instanceIndex : -1.0;\n"
        "    if (isWater) {\n"

        "        TexCoord = texCoord + vec2(time * 0.012, time * 0.009);\n"
        "    } else {\n"
        "        TexCoord = texCoord;\n"
        "    }\n"
        "    VertColor = vertColor;\n"
        "    gl_Position = projection * view * worldPos;\n"
        "}\n";

    const char* fShaderCode = "#version 130\n"
        "in vec2 TexCoord;\n"
        "in vec4 VertColor;\n"
        "in vec3 WorldPos;\n"
        "in vec3 WorldNormal;\n"
        "in vec3 ViewNormal;\n"
        "in float InstanceIndex;\n"
        "out vec4 FragColor;\n"
        "uniform sampler2D diffTexture;\n"
        "uniform sampler2D detailTexture;\n"
        "uniform bool hasTexture;\n"
        "uniform bool hasDetailTexture;\n"
        "uniform bool isPersonMaterial;\n"
        "uniform bool personLighting;\n"
        "uniform bool personUseInk;\n"
        "uniform bool proceduralLit;\n"
        "uniform vec4 personBaseColor;\n"
        "uniform vec3 previewLightDir;\n"
        "uniform int  blendMode;\n"
        "uniform float alphaRef;\n"
        "uniform bool isFakeShadow;\n"
        "uniform bool isColorVolume;\n"
        "uniform bool isHighlighted;\n"
        "uniform float selectedInstanceIndex;\n"

        "uniform bool debugTransparent;\n"

        "uniform bool isWater;\n"
        "uniform float time;\n"
        "uniform vec3 viewPosWorld;\n"
        "void main() {\n"
        "    vec4 result;\n"
        "    if (isWater) {\n"

        "        vec3 deepColor    = vec3(0.05, 0.20, 0.32);\n"
        "        vec3 shallowColor = vec3(0.30, 0.55, 0.62);\n"
        "        vec4 base = vec4(deepColor, 0.85);\n"
        "        if (hasTexture) {\n"
        "            vec4 tex1 = texture(diffTexture, TexCoord);\n"
        "            vec4 tex2 = texture(diffTexture, TexCoord * 1.7 + vec2(time * -0.018, time * 0.014));\n"
        "            float caustic = (tex1.r + tex2.r) * 0.5;\n"
        "            base.rgb = mix(deepColor, shallowColor, caustic);\n"
        "        }\n"

        "        vec3 viewDir = normalize(viewPosWorld - WorldPos);\n"
        "        float fres = pow(1.0 - clamp(viewDir.y, 0.0, 1.0), 3.0);\n"
        "        base.rgb = mix(base.rgb, shallowColor, fres * 0.4);\n"
        "        base.a   = mix(0.78, 0.94, fres);\n"

        "        float glint = pow(max(0.0, viewDir.y), 8.0) * 0.25;\n"
        "        base.rgb += vec3(glint);\n"
        "        result = base;\n"
        "    } else if (debugTransparent) {\n"
        "        result = vec4(0.85, 0.85, 0.9, 0.18);\n"
        "    } else if (isFakeShadow || isColorVolume) {\n"
        "        result = vec4(0.0, 0.0, 0.0, 0.3);\n"
        "    } else if (isPersonMaterial) {\n"

        "        vec4 texColor = hasTexture ? texture(diffTexture, TexCoord) : vec4(1.0);\n"
        "        if (blendMode == 1 && texColor.a < alphaRef) discard;\n"
        "        if (blendMode >= 2 && texColor.a < 0.004) discard;\n"
        "        vec3 n = normalize(WorldNormal);\n"
        "        float diffuse = max(dot(n, normalize(previewLightDir)), 0.0);\n"

        "        vec3 lightColor = personBaseColor.rgb;\n"
        "        if (personLighting) {\n"
        "            lightColor += (vec3(1.0) - personBaseColor.rgb) * diffuse;\n"
        "        }\n"
        "        result = vec4(texColor.rgb * lightColor,\n"
        "                      texColor.a * personBaseColor.a);\n"
        "        if (personUseInk && hasDetailTexture) {\n"
        "            vec3 viewN = normalize(ViewNormal);\n"
        "            vec2 sphereUv = vec2(viewN.x * 0.5 + 0.5, 0.5 - viewN.y * 0.5);\n"
        "            vec4 ink = texture(detailTexture, sphereUv);\n"
        "            result.rgb = result.rgb * ink.a + ink.rgb;\n"
        "        }\n"
        "    } else if (proceduralLit) {\n"
        "        vec3 n = normalize(WorldNormal);\n"
        "        float diffuse = max(dot(n, normalize(previewLightDir)), 0.0);\n"
        "        result = vec4(VertColor.rgb * (0.18 + 0.82 * diffuse), VertColor.a);\n"
        "    } else if (hasTexture) {\n"
        "        vec4 texColor = texture(diffTexture, TexCoord);\n"
        "        if (blendMode == 1) {\n"
        "            if (texColor.a < alphaRef) discard;\n"
        "        } else if (blendMode >= 2) {\n"

        "            if (texColor.a < 0.004) discard;\n"
        "        }\n"
        "        result = texColor * VertColor;\n"
        "    } else {\n"

        "        result = VertColor;\n"
        "    }\n"
        "    if (isHighlighted || (selectedInstanceIndex >= 0.0 && abs(InstanceIndex - selectedInstanceIndex) < 0.25)) {\n"
        "        result.rgb = mix(result.rgb, vec3(0.2, 1.0, 0.3), 0.6);\n"
        "    }\n"
        "    FragColor = result;\n"
        "}\n";

    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, NULL);
    glCompileShader(vertex);

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, NULL);
    glCompileShader(fragment);

    modelProgram = glCreateProgram();
    glAttachShader(modelProgram, vertex);
    glAttachShader(modelProgram, fragment);
    glBindAttribLocation(modelProgram, 0, "pos");
    glBindAttribLocation(modelProgram, 1, "normal");
    glBindAttribLocation(modelProgram, 2, "texCoord");
    glBindAttribLocation(modelProgram, 3, "boneIndices");
    glBindAttribLocation(modelProgram, 4, "boneWeights");
    glBindAttribLocation(modelProgram, 5, "vertColor");
    glBindAttribLocation(modelProgram, 6, "instanceTransform0");
    glBindAttribLocation(modelProgram, 7, "instanceTransform1");
    glBindAttribLocation(modelProgram, 8, "instanceTransform2");
    glBindAttribLocation(modelProgram, 9, "instanceTransform3");
    glBindAttribLocation(modelProgram, 10, "instanceIndex");
    glLinkProgram(modelProgram);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    const char* skelVS = "#version 130\n"
        "in vec3 pos;\n"
        "in vec3 color;\n"
        "out vec3 vertColor;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "void main() {\n"
        "    vertColor = color;\n"
        "    gl_Position = projection * view * vec4(pos, 1.0);\n"
        "}\n";

    const char* skelFS = "#version 130\n"
        "in vec3 vertColor;\n"
        "out vec4 FragColor;\n"
        "uniform float highlightBone;\n"
        "void main() {\n"
        "    FragColor = vec4(vertColor, 1.0);\n"
        "}\n";

    unsigned int sv = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(sv, 1, &skelVS, NULL);
    glCompileShader(sv);

    unsigned int sf = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sf, 1, &skelFS, NULL);
    glCompileShader(sf);

    skeletonProgram = glCreateProgram();
    glAttachShader(skeletonProgram, sv);
    glAttachShader(skeletonProgram, sf);
    glBindAttribLocation(skeletonProgram, 0, "pos");
    glBindAttribLocation(skeletonProgram, 1, "color");
    glLinkProgram(skeletonProgram);
    glDeleteShader(sv);
    glDeleteShader(sf);
}

void SpiderManTool::UpdateWorldCamera(bool isHovered) {
    GLFWwindow* window = glfwGetCurrentContext();
    if (!window) return;

    const bool rightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!rightMouseDown && isFlyCameraMouseLocked) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPos(window, flyCameraLockX, flyCameraLockY);
        isFlyCameraMouseLocked = false;
    }
    if (!isHovered && !isFlyCameraMouseLocked && !rightMouseDown) return;

    float dt = ImGui::GetIO().DeltaTime;

    if (rightMouseDown) {
        if (!isFlyCameraMouseLocked) {
            glfwGetCursorPos(window, &flyCameraLockX, &flyCameraLockY);
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            isFlyCameraMouseLocked = true;
        }

        float xoffset = ImGui::GetIO().MouseDelta.x;
        float yoffset = ImGui::GetIO().MouseDelta.y;

        float sensitivity = 0.2f;
        xoffset *= sensitivity;
        yoffset *= sensitivity;

        camYaw += xoffset;
        camPitch -= yoffset;

        if (camPitch > 89.0f) camPitch = 89.0f;
        if (camPitch < -89.0f) camPitch = -89.0f;

        float front[3];
        front[0] = cos(camYaw * 3.14159f / 180.0f) * cos(camPitch * 3.14159f / 180.0f);
        front[1] = sin(camPitch * 3.14159f / 180.0f);
        front[2] = sin(camYaw * 3.14159f / 180.0f) * cos(camPitch * 3.14159f / 180.0f);
        Normalize(front);
        camFront[0] = front[0]; camFront[1] = front[1]; camFront[2] = front[2];

        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            float multiplier = 1.0f + (0.2f * wheel);
            camSpeed *= multiplier;
            if (camSpeed < 0.1f) camSpeed = 0.1f;
            if (camSpeed > 20000.0f) camSpeed = 20000.0f;
        }

        float velocity = camSpeed * dt;

        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            camPos[0] += camFront[0] * velocity;
            camPos[1] += camFront[1] * velocity;
            camPos[2] += camFront[2] * velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            camPos[0] -= camFront[0] * velocity;
            camPos[1] -= camFront[1] * velocity;
            camPos[2] -= camFront[2] * velocity;
        }

        float right[3];
        Cross(camFront, camUp, right);
        Normalize(right);

        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            camPos[0] -= right[0] * velocity;
            camPos[1] -= right[1] * velocity;
            camPos[2] -= right[2] * velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            camPos[0] += right[0] * velocity;
            camPos[1] += right[1] * velocity;
            camPos[2] += right[2] * velocity;
        }

        if (ImGui::IsKeyDown(ImGuiKey_Z)) {
            camPos[1] += velocity;
        }
        if (ImGui::IsKeyDown(ImGuiKey_X)) {
            camPos[1] -= velocity;
        }
    }
}

void SpiderManTool::UpdateModelOrbitCamera(bool isHovered) {
    GLFWwindow* window = glfwGetCurrentContext();
    if (window && isFlyCameraMouseLocked) {
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        glfwSetCursorPos(window, flyCameraLockX, flyCameraLockY);
        isFlyCameraMouseLocked = false;
    }
    if (!isHovered) return;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kRotateSensitivity = 0.30f;

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        camYaw += ImGui::GetIO().MouseDelta.x * kRotateSensitivity;
        camPitch += ImGui::GetIO().MouseDelta.y * kRotateSensitivity;
        camPitch = std::max(-85.0f, std::min(85.0f, camPitch));
    }

    // Persistent orbit radius. Deriving it from camPos each frame would feed back into the
    // moving focal point below and drift, so keep it as state (seeded from the camera once).
    if (!std::isfinite(orbitDistance) || orbitDistance <= 0.0f) {
        const float dx = camPos[0] - modelCenter[0];
        const float dy = camPos[1] - modelCenter[1];
        const float dz = camPos[2] - modelCenter[2];
        orbitDistance = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!std::isfinite(orbitDistance) || orbitDistance < 0.001f)
            orbitDistance = std::max(1.0f, modelRadius * 3.2f);
    }

    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) orbitDistance *= powf(0.85f, wheel);
    const float minDistance = std::max(0.05f, modelRadius * 0.20f);
    const float maxDistance = std::max(100.0f, modelRadius * 40.0f);
    orbitDistance = std::max(minDistance, std::min(maxDistance, orbitDistance));

    // Focal point pans from the body centre (zoomed out) up to the head (zoomed in), and
    // eases back down as you zoom out. t: 0 when far, 1 when close.
    const float farD  = std::max(1.0f, modelRadius * 3.0f);
    const float nearD = std::max(0.05f, modelRadius * 0.8f);
    float t = (farD - orbitDistance) / std::max(0.001f, farD - nearD);
    t = std::max(0.0f, std::min(1.0f, t));
    const float target[3] = {
        modelCenter[0] + (modelHeadTarget[0] - modelCenter[0]) * t,
        modelCenter[1] + (modelHeadTarget[1] - modelCenter[1]) * t,
        modelCenter[2] + (modelHeadTarget[2] - modelCenter[2]) * t
    };

    const float yaw = camYaw * kPi / 180.0f;
    const float pitch = camPitch * kPi / 180.0f;
    const float horizontal = cosf(pitch) * orbitDistance;
    camPos[0] = target[0] + cosf(yaw) * horizontal;
    camPos[1] = target[1] + sinf(pitch) * orbitDistance;
    camPos[2] = target[2] + sinf(yaw) * horizontal;

    camFront[0] = target[0] - camPos[0];
    camFront[1] = target[1] - camPos[1];
    camFront[2] = target[2] - camPos[2];
    Normalize(camFront);
    camUp[0] = 0.0f;
    camUp[1] = 1.0f;
    camUp[2] = 0.0f;
}

void SpiderManTool::ApplyMorphTargets() {
    if (isWorldMode || !loadedMorphFile.valid || morphTargetWeights.empty()) return;

    for (auto& mesh : previewMeshes) {
        if (!mesh.vbo || mesh.morphVertexData.empty() ||
            mesh.positions.size() % 3 != 0) continue;
        const size_t vertexCount = mesh.positions.size() / 3;
        if (mesh.morphVertexData.size() != vertexCount * 20) continue;

        for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
            float* destination = mesh.morphVertexData.data() + vertex * 20;
            destination[0] = mesh.positions[vertex * 3 + 0];
            destination[1] = mesh.positions[vertex * 3 + 1];
            destination[2] = mesh.positions[vertex * 3 + 2];
        }
        const size_t targetCount = std::min({
            morphTargetWeights.size(),
            mesh.morphPositionDeltas.size(),
            loadedMorphFile.sets.size()});
        for (size_t target = 1; target < targetCount; ++target) {
            const float weight = morphTargetWeights[target];
            if (weight == 0.0f) continue;
            const auto& deltas = mesh.morphPositionDeltas[target];
            const auto& changed = mesh.morphPositionChanged[target];
            if (deltas.size() != vertexCount || changed.size() != vertexCount) continue;
            for (size_t vertex = 0; vertex < vertexCount; ++vertex) {
                if (!changed[vertex]) continue;
                float* destination = mesh.morphVertexData.data() + vertex * 20;
                destination[0] += weight * deltas[vertex][0];
                destination[1] += weight * deltas[vertex][1];
                destination[2] += weight * deltas[vertex][2];
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(mesh.morphVertexData.size() * sizeof(float)),
                        mesh.morphVertexData.data());
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void SpiderManTool::RenderModelPreview() {
    int width = 3840;
    int height = 2160;
    if (!skipDrawAfterPoseEvaluation) {
        glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
        glViewport(0, 0, width, height);
        glClearColor(0.15f, 0.15f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    if (previewMeshes.empty()) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    ApplyMorphTargets();

    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(modelProgram);

    float fov = 1.0f;
    float aspect = (float)width / (float)height;
    float znear = 0.1f;
    float zfar = 20000.0f;

    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float model[16];

    float target[3] = { camPos[0] + camFront[0], camPos[1] + camFront[1], camPos[2] + camFront[2] };
    LookAt(camPos, target, camUp, view);

    memset(model, 0, sizeof(model));
    model[0] = 1; model[5] = 1; model[10] = 1; model[15] = 1;

    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "view"), 1, GL_FALSE, view);
    glUniformMatrix4fv(glGetUniformLocation(modelProgram, "model"), 1, GL_FALSE, model);
    glUniform3f(glGetUniformLocation(modelProgram, "viewPos"), camPos[0], camPos[1], camPos[2]);

    GLint locUseSkinning = glGetUniformLocation(modelProgram, "useSkinning");
    GLint locBoneMatrices = glGetUniformLocation(modelProgram, "boneMatrices");

    const int meshBoneCount = std::min((int)skeletonBones.size(), MAX_GLOBAL_BONES);
    const bool entityBoneMapActive = activeBoneMapping.valid &&
        activeBoneMapping.meshPoseCount == (uint32_t)meshBoneCount &&
        activeBoneMapping.meshToLogical.size() == (size_t)meshBoneCount &&
        !activeBoneMapping.logicalToMesh.empty() &&
        activeBoneMapping.logicalToMesh.size() <= (size_t)MAX_GLOBAL_BONES;
    int skeletonLogicalCount = meshBoneCount;
    if (loadedSkeleton) {
        auto includeLogical = [&](int index) {
            if (index >= 0 && index < MAX_GLOBAL_BONES)
                skeletonLogicalCount = std::max(skeletonLogicalCount, index + 1);
        };
        for (const auto& entry : loadedSkeleton->bone_map) includeLogical(entry.first);
        for (const auto& entry : loadedSkeleton->parent_map) {
            includeLogical(entry.first);
            includeLogical(entry.second);
        }
        for (const auto& component : loadedSkeleton->components) {
            for (int index : component.bone_indices) includeLogical(index);
            for (const auto& node : component.arb_nodes) {
                includeLogical(node.my_matrix_ix);
                includeLogical(node.parent_matrix_ix);
            }
        }
    }
    const int logicalBoneCount = std::min(MAX_GLOBAL_BONES, std::max(
        skeletonLogicalCount,
        entityBoneMapActive ? static_cast<int>(activeBoneMapping.logicalToMesh.size()) : 0));
    auto nalToMeshBone = [&](int logicalIdx) -> int {
        if (logicalIdx < 0 || logicalIdx >= logicalBoneCount) return -1;
        if (entityBoneMapActive) {
            if (logicalIdx >= static_cast<int>(activeBoneMapping.logicalToMesh.size())) return -1;
            const int mesh = activeBoneMapping.logicalToMesh[logicalIdx];
            return mesh >= 0 && mesh < meshBoneCount ? mesh : -1;
        }
        return logicalIdx < meshBoneCount ? logicalIdx : -1;
    };

    std::vector<TentaclePreviewChain> evaluatedTentacles;

    bool skinningActive = false;
    std::vector<float> globalBoneMatData(MAX_GLOBAL_BONES * 16, 0.f);

    for (int i = 0; i < MAX_GLOBAL_BONES; i++) {
        Mat4Identity(&globalBoneMatData[i * 16]);
    }

    std::vector<float> bonePosWorld(MAX_GLOBAL_BONES * 3, 0.f);
    for (int i = 0; i < meshBoneCount; i++) {
        bonePosWorld[i*3+0] = skeletonBones[i].position[0];
        bonePosWorld[i*3+1] = skeletonBones[i].position[1];
        bonePosWorld[i*3+2] = skeletonBones[i].position[2];
    }

    if (!isWorldMode && loadedAnimFile && loadedSkeleton && selectedAnimIndex >= 0 &&
        selectedAnimIndex < (int)loadedAnimFile->animations.size() &&
        (!loadedAnimFile->animations[selectedAnimIndex].skeleton ||
         nal_skeleton_pose_inheritable(
             loadedAnimFile->animations[selectedAnimIndex].skeleton.get(),
             loadedSkeleton.get())) &&
        logicalBoneCount > 0 && meshBoneCount > 0) {

        const auto& anim = loadedAnimFile->animations[selectedAnimIndex];
        if (anim.is_gen_anim() && anim.generic_decoded.complete &&
            !anim.generic_decoded.world_frames.empty()) {
            const int playbackFrameCount = static_cast<int>(anim.generic_decoded.world_frames.size());
            const int frame0 = std::max(0, std::min(currentAnimFrame, playbackFrameCount - 1));
            int frame1 = frame0 + 1;
            if (frame1 >= playbackFrameCount) frame1 = anim.is_looping() ? 0 : frame0;
            const float fraction = frame0 != frame1 ? animFrameFraction : 0.0f;

            const auto& worlds0 = anim.generic_decoded.world_frames[frame0];
            const auto& worlds1 = anim.generic_decoded.world_frames[frame1];
            const size_t matrixCount = std::min({
                worlds0.size(), worlds1.size(),
                static_cast<size_t>(logicalBoneCount)});

            for (size_t logical = 0; logical < matrixCount; ++logical) {
                const int mesh = nalToMeshBone(static_cast<int>(logical));
                if (mesh < 0) continue;

                float sampled[16];
                if (fraction > 0.0f) {
                    const QuatWXYZ q0 = QuatFromMat4(worlds0[logical].data());
                    const QuatWXYZ q1 = QuatFromMat4(worlds1[logical].data());
                    Mat4FromQuat(QuatSlerp(q0, q1, fraction), sampled);
                    sampled[12] = worlds0[logical][12] +
                        fraction * (worlds1[logical][12] - worlds0[logical][12]);
                    sampled[13] = worlds0[logical][13] +
                        fraction * (worlds1[logical][13] - worlds0[logical][13]);
                    sampled[14] = worlds0[logical][14] +
                        fraction * (worlds1[logical][14] - worlds0[logical][14]);
                } else {
                    memcpy(sampled, worlds0[logical].data(), sizeof(sampled));
                }

                Mat4Multiply(sampled, skeletonBones[mesh].invBindMatrix,
                             &globalBoneMatData[mesh * 16]);

                bonePosWorld[mesh * 3 + 0] = sampled[12];
                bonePosWorld[mesh * 3 + 1] = sampled[13];
                bonePosWorld[mesh * 3 + 2] = sampled[14];
                if (nalBonePositions.count(static_cast<int>(logical))) {
                    nalBonePositions[static_cast<int>(logical)] = {
                        sampled[12], sampled[13], sampled[14]};
                }
            }
            skinningActive = true;
        } else if (!anim.is_gen_anim()) {

        const NalSkeletonData* animSkel = anim.skeleton ? anim.skeleton.get() : loadedSkeleton.get();
        // The rig we drive is always the LOADED skeleton. For an inherited clip (a costume variant
        // playing its base character's animation) animSkel has different bone indices, so component
        // data must come from the loaded rig or the pose lands on the wrong bones.
        const NalSkeletonData* rigSkel = loadedSkeleton.get();
        int playbackFrameCount = anim.playback_frame_count();
        int frame0 = std::max(0, std::min(currentAnimFrame, std::max(0, playbackFrameCount - 1)));
        int frame1 = frame0 + 1;
        if (frame1 >= playbackFrameCount)
            frame1 = anim.is_looping() ? 0 : frame0;
        float frameFrac = animFrameFraction;

        struct BonePoseDelta {
            QuatWXYZ rot;
            bool hasRot = false;
        };
        std::vector<BonePoseDelta> poseDeltas(MAX_GLOBAL_BONES);
        float rootOffset[3] = {0.0f, 0.0f, 0.0f};
        float rootRotMat[16]; Mat4Identity(rootRotMat);
        bool hasRootRot = false;
        std::map<int, std::array<float, 16>> runtimeWorld;
        std::array<float, 15> tentaclePose{};
        bool hasTentaclePose = false;

        auto parentOf = [&](int idx) -> int {
            auto it = rigSkel->parent_map.find(idx);
            if (it == rigSkel->parent_map.end()) return -1;
            int parent = it->second;
            return (parent >= 0 && parent < logicalBoneCount && parent != idx) ? parent : -1;
        };

        std::vector<float> restLocal(MAX_GLOBAL_BONES * 16, 0.0f);
        std::vector<float> animLocal(MAX_GLOBAL_BONES * 16, 0.0f);
        std::vector<float> animWorld(MAX_GLOBAL_BONES * 16, 0.0f);
        for (int i = 0; i < MAX_GLOBAL_BONES; ++i) {
            Mat4Identity(&restLocal[i * 16]);
            Mat4Identity(&animLocal[i * 16]);
            Mat4Identity(&animWorld[i * 16]);
        }

        std::vector<QuatWXYZ> restLocalQuat(MAX_GLOBAL_BONES);
        std::vector<uint8_t> hasRestLocalQuat(MAX_GLOBAL_BONES, 0);
        for (int i = 0; i < logicalBoneCount; ++i) {
            int parent = parentOf(i);
            int meshI = nalToMeshBone(i);
            int meshParent = nalToMeshBone(parent);
            if (meshI < 0) continue;
            if (parent >= 0 && meshParent >= 0) {
                Mat4Multiply(skeletonBones[meshParent].invBindMatrix,
                             skeletonBones[meshI].bindMatrix,
                             &restLocal[i * 16]);
            } else {
                memcpy(&restLocal[i * 16], skeletonBones[meshI].bindMatrix, sizeof(float) * 16);
            }

            restLocalQuat[i] = QuatFromMat4(&restLocal[i * 16]);
            hasRestLocalQuat[i] = 1;
        }

        auto findComponentIn = [&](const NalSkeletonData* skel,
                                   const NalAnimComponent& comp) -> const NalComponentData* {
            if (!skel) return nullptr;
            if (comp.slot_ix >= 0 && comp.slot_ix < (int)skel->components.size()) {
                const auto& bySlot = skel->components[comp.slot_ix];
                if (nal_type_to_comp_id(bySlot.type_id) == comp.comp_ix) return &bySlot;
            }
            for (const auto& sc : skel->components) {
                if (nal_type_to_comp_id(sc.type_id) == comp.comp_ix) return &sc;
            }
            return nullptr;
        };

        // Component data comes from the rig being driven; the anim's own skeleton is used only to
        // verify the pose layout matches. An inherited component whose block doesn't line up (a
        // variant's accessory/ArbitraryPO) returns null here, so it is left at rest rather than
        // driven with data meant for a different rig.
        auto findComponentForAnim = [&](const NalAnimComponent& comp) -> const NalComponentData* {
            const NalComponentData* rigComp = findComponentIn(rigSkel, comp);
            if (!rigComp) return nullptr;
            if (animSkel != rigSkel) {
                const NalComponentData* srcComp = findComponentIn(animSkel, comp);
                if (srcComp && !nal_component_pose_compatible(*srcComp, *rigComp)) return nullptr;
            }
            return rigComp;
        };

        auto setBoneDeltaQuat = [&](int boneIdx, QuatWXYZ delta) {
            if (boneIdx < 0 || boneIdx >= logicalBoneCount) return;
            auto& dst = poseDeltas[boneIdx];
            dst.rot = dst.hasRot ? QuatMul(dst.rot, delta) : QuatNormalize(delta);
            dst.hasRot = true;
        };

        auto setAbsoluteTrackQuat = [&](int boneIdx, QuatWXYZ q, bool hasDefault, QuatWXYZ defaultQ) {
            if (boneIdx < 0 || boneIdx >= logicalBoneCount) return;
            if (hasRestLocalQuat[boneIdx]) {
                defaultQ = restLocalQuat[boneIdx];
                hasDefault = true;
            }
            QuatWXYZ delta = hasDefault ? QuatMul(QuatInverse(defaultQ), q) : q;
            setBoneDeltaQuat(boneIdx, delta);
        };

        auto defaultQuatOrIdentity = [&](const NalComponentData* compData, int index) -> QuatWXYZ {
            QuatWXYZ q;
            return GetDefaultQuat(compData, index, q) ? q : QuatWXYZ{};
        };

        auto matFromQuatPos = [&](QuatWXYZ q, const std::array<float, 3>& pos, float* out) {
            Mat4FromQuat(q, out);
            out[12] = pos[0];
            out[13] = pos[1];
            out[14] = pos[2];
        };

        auto storeRuntimeWorld = [&](int boneIdx, const float* m) {
            if (boneIdx < 0 || boneIdx >= logicalBoneCount) return;
            if (runtimeWorld.count(boneIdx)) return;
            auto& dst = runtimeWorld[boneIdx];
            memcpy(dst.data(), m, sizeof(float) * 16);
        };

        auto getRuntimeWorld = [&](int boneIdx, float* out) -> bool {
            auto it = runtimeWorld.find(boneIdx);
            if (it == runtimeWorld.end()) return false;
            memcpy(out, it->second.data(), sizeof(float) * 16);
            return true;
        };

        auto localToRuntimeParent = [&](const float* local, int parentBoneIdx, float* out) {
            float parent[16];
            if (getRuntimeWorld(parentBoneIdx, parent)) {
                Mat4Multiply(parent, local, out);
            } else if (parentBoneIdx >= 0 && parentBoneIdx < logicalBoneCount) {
                int meshParent = nalToMeshBone(parentBoneIdx);
                if (meshParent >= 0)
                    Mat4Multiply(skeletonBones[meshParent].bindMatrix, local, out);
                else
                    memcpy(out, local, sizeof(float) * 16);
            } else {
                memcpy(out, local, sizeof(float) * 16);
            }
        };

        bool animHasStdLegs = false;
        bool animHasStdArms = false;
        for (   const auto& comp : anim.components) {
            if (!comp.decoded.frames.empty()) {
                animHasStdLegs = animHasStdLegs || comp.comp_ix == NalComp::LEGS;
                animHasStdArms = animHasStdArms || comp.comp_ix == NalComp::ARMS;
            }
        }
        bool solveLegsIk = !animHasStdLegs;
        bool solveArmsIk = !animHasStdArms;

        std::vector<const NalAnimComponent*> sortedComponents;
        sortedComponents.reserve(anim.components.size());
        for (const auto& comp : anim.components) sortedComponents.push_back(&comp);
        std::sort(sortedComponents.begin(), sortedComponents.end(),
            [](const NalAnimComponent* a, const NalAnimComponent* b) {
                int as = (a->slot_ix >= 0) ? a->slot_ix : (1 << 30);
                int bs = (b->slot_ix >= 0) ? b->slot_ix : (1 << 30);
                if (as != bs) return as < bs;
                return a->comp_ix < b->comp_ix;
            });

        for (const NalAnimComponent* compPtr : sortedComponents) {
            const NalAnimComponent& comp = *compPtr;
            if (frame0 < 0 || frame0 >= (int)comp.decoded.frames.size()) continue;
            const auto& fv0 = comp.decoded.frames[frame0];
            const auto& fv1 = (frame1 >= 0 && frame1 < (int)comp.decoded.frames.size())
                ? comp.decoded.frames[frame1]
                : fv0;
            float sampleFrac = (frame0 != frame1) ? frameFrac : 0.0f;

            const NalComponentData* sc = findComponentForAnim(comp);
            if (!sc) continue;
            if (comp.comp_ix == NalComp::LEGS_IK && animHasStdLegs) continue;
            if (comp.comp_ix == NalComp::ARMS_IK && animHasStdArms) continue;

            int cursor = 0;
            if (comp.comp_ix == NalComp::TORSO_HEAD || comp.comp_ix == NalComp::TORSO_HEAD_STD) {
                if (sc->bone_indices.size() < TorsoBone::COUNT) continue;
                int roles[] = {TorsoBone::SPINE, TorsoBone::SPINE1, TorsoBone::SPINE2, TorsoBone::NECK, TorsoBone::HEAD};
                QuatWXYZ torsoState[6];
                for (int i = 0; i < 6; ++i) torsoState[i] = defaultQuatOrIdentity(sc, i);
                std::array<float, 3> pelvisPos = sc->default_pose.pelvis_pos;

                for (int bit = 0; bit < 5; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, torsoState[bit])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[roles[bit]], torsoState[bit], true, defaultQuatOrIdentity(sc, bit));
                }
                if (comp.mask & 0x20) {
                    if (ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, torsoState[5])) {
                        setAbsoluteTrackQuat(sc->bone_indices[TorsoBone::PELVIS], torsoState[5], true, defaultQuatOrIdentity(sc, 5));
                    }
                    float loc[3];
                    if (ConsumeSampleVec3(fv0, fv1, cursor, sampleFrac, loc)) {
                        pelvisPos = {loc[0], loc[1], loc[2]};
                        rootOffset[0] += loc[0] - sc->default_pose.pelvis_pos[0];
                        rootOffset[1] += loc[1] - sc->default_pose.pelvis_pos[1];
                        rootOffset[2] += loc[2] - sc->default_pose.pelvis_pos[2];
                    }
                }

                if (sc->offset_locs.size() >= 5) {
                    float local[16];
                    float world[16];
                    int pelvis = sc->bone_indices[TorsoBone::PELVIS];
                    matFromQuatPos(torsoState[5], pelvisPos, world);
                    storeRuntimeWorld(pelvis, world);

                    for (int i = 0; i < 5; ++i) {
                        int childRole = roles[i];
                        int child = sc->bone_indices[childRole];
                        int parent = sc->bone_indices[childRole - 1];
                        matFromQuatPos(torsoState[i], sc->offset_locs[i], local);
                        localToRuntimeParent(local, parent, world);
                        storeRuntimeWorld(child, world);
                    }

                    if (sc->type_id == NalCompType::TorsoHead_TwoNeck &&
                        sc->bone_indices.size() > TorsoBone::NECK_AUX) {
                        int neckAux = sc->bone_indices[TorsoBone::NECK_AUX];
                        QuatWXYZ emptyQ = QuatNormalize({
                            sc->empty_neck_orient[3],
                            sc->empty_neck_orient[0],
                            sc->empty_neck_orient[1],
                            sc->empty_neck_orient[2]
                        });
                        matFromQuatPos(emptyQ, sc->empty_neck_pos, local);
                        localToRuntimeParent(local, sc->bone_indices[TorsoBone::SPINE2], world);
                        storeRuntimeWorld(neckAux, world);
                    }
                }
            }
            else if (comp.comp_ix == NalComp::LEGS) {
                if (sc->bone_indices.size() < LegStdBone::COUNT) continue;
                QuatWXYZ legState[8];
                for (int i = 0; i < 8; ++i) legState[i] = defaultQuatOrIdentity(sc, i);
                for (int bit = 0; bit < 8; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, legState[bit])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], legState[bit], true, defaultQuatOrIdentity(sc, bit));
                }

                if (sc->offset_locs.size() >= 8) {
                    int parentRole[8] = {-1, LegStdBone::L_THIGH, LegStdBone::L_CALF, LegStdBone::L_FOOT,
                                         -1, LegStdBone::R_THIGH, LegStdBone::R_CALF, LegStdBone::R_FOOT};
                    int anchor = sc->bone_indices[LegStdBone::ROOT];
                    float local[16];
                    float world[16];
                    for (int role = 0; role < 8; ++role) {
                        int bone = sc->bone_indices[role];
                        matFromQuatPos(legState[role], sc->offset_locs[role], local);
                        int parent = parentRole[role] >= 0 ? sc->bone_indices[parentRole[role]] : anchor;
                        localToRuntimeParent(local, parent, world);
                        storeRuntimeWorld(bone, world);
                    }
                }
            }
            else if (comp.comp_ix == NalComp::ARMS) {
                if (sc->bone_indices.size() < ArmBone::COUNT) continue;
                QuatWXYZ armState[8];
                for (int i = 0; i < 8; ++i) armState[i] = defaultQuatOrIdentity(sc, i);
                for (int bit = 0; bit < 8; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, armState[bit])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], armState[bit], true, defaultQuatOrIdentity(sc, bit));
                }

                if (sc->offset_locs.size() >= 8) {
                    int anchor = sc->bone_indices[ArmBone::NECK_PARENT];
                    int order[8] = {ArmBone::L_CLAV, ArmBone::L_UPPER, ArmBone::L_FORE, ArmBone::L_HAND,
                                    ArmBone::R_CLAV, ArmBone::R_UPPER, ArmBone::R_FORE, ArmBone::R_HAND};
                    float local[16];
                    float world[16];
                    for (int oi = 0; oi < 8; ++oi) {
                        int role = order[oi];
                        int bone = sc->bone_indices[role];
                        matFromQuatPos(armState[role], sc->offset_locs[role], local);
                        int parent = (role == ArmBone::L_CLAV || role == ArmBone::R_CLAV)
                            ? anchor
                            : sc->bone_indices[role - 1];
                        localToRuntimeParent(local, parent, world);
                        storeRuntimeWorld(bone, world);
                    }

                    if (sc->fore_twist_locs.size() >= 4 && sc->bone_indices.size() > ArmBone::R_TWIST1) {
                        int twistBones[4] = {ArmBone::L_TWIST0, ArmBone::L_TWIST1, ArmBone::R_TWIST0, ArmBone::R_TWIST1};
                        int twistParents[4] = {ArmBone::L_FORE, ArmBone::L_TWIST0, ArmBone::R_FORE, ArmBone::R_TWIST0};
                        for (int i = 0; i < 4; ++i) {
                            int bone = sc->bone_indices[twistBones[i]];
                            int parent = sc->bone_indices[twistParents[i]];

                            Mat4BuildTwistLineXform(sc->fore_twist_locs[i], 0.0f, local);
                            localToRuntimeParent(local, parent, world);
                            storeRuntimeWorld(bone, world);
                        }
                    }
                }
            }
            else if (comp.comp_ix == NalComp::LEGS_IK) {
                if (sc->bone_indices.size() < LegBone::COUNT) continue;
                QuatWXYZ toeState[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
                QuatWXYZ footState[2] = {defaultQuatOrIdentity(sc, 2), defaultQuatOrIdentity(sc, 3)};
                std::array<float, 3> footPos[2] = {sc->default_pose.foot_pos[0], sc->default_pose.foot_pos[1]};
                float kneeSpin[2] = {sc->default_pose.knee_spin[0], sc->default_pose.knee_spin[1]};

                for (int bit = 0; bit < 2; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, toeState[bit])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], toeState[bit], true, defaultQuatOrIdentity(sc, bit));
                }
                for (int bit = 2; bit < 4; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    int side = bit - 2;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, footState[side])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], footState[side], true, defaultQuatOrIdentity(sc, bit));

                    float pos[3];
                    if (ConsumeSampleVec3(fv0, fv1, cursor, sampleFrac, pos)) {
                        footPos[side] = {pos[0], pos[1], pos[2]};
                    }
                    if (cursor < (int)fv0.size()) kneeSpin[side] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                }

                int anchor = sc->bone_indices[LegBone::PELVIS];
                float identity[16];
                Mat4Identity(identity);
                float local[16], world[16];
                for (int side = 0; side < 2; ++side) {
                    int footRole = side == 0 ? LegBone::L_FOOT : LegBone::R_FOOT;
                    int toeRole = side == 0 ? LegBone::L_TOE : LegBone::R_TOE;
                    int thighRole = side == 0 ? LegBone::L_THIGH : LegBone::R_THIGH;
                    int calfRole = side == 0 ? LegBone::L_CALF : LegBone::R_CALF;
                    int foot = sc->bone_indices[footRole];
                    int toe = sc->bone_indices[toeRole];

                    matFromQuatPos(footState[side], footPos[side], local);
                    if (solveLegsIk && sc->has_ik && sc->offset_locs.size() > (size_t)thighRole) {
                        float upperLocal[16], lowerLocal[16];
                        SolveNalIk(identity, sc->offset_locs[thighRole], local, sc->ik_data[side],
                                   kneeSpin[side], false, false, upperLocal, lowerLocal);
                        localToRuntimeParent(upperLocal, anchor, world);
                        storeRuntimeWorld(sc->bone_indices[thighRole], world);
                        localToRuntimeParent(lowerLocal, anchor, world);
                        storeRuntimeWorld(sc->bone_indices[calfRole], world);
                    }

                    localToRuntimeParent(local, anchor, world);
                    storeRuntimeWorld(foot, world);

                    std::array<float, 3> toeOff = (sc->offset_locs.size() > (size_t)toeRole)
                        ? sc->offset_locs[toeRole]
                        : std::array<float, 3>{0.0f, 0.0f, 0.0f};
                    matFromQuatPos(toeState[side], toeOff, local);
                    localToRuntimeParent(local, foot, world);
                    storeRuntimeWorld(toe, world);
                }
            }
            else if (comp.comp_ix == NalComp::ARMS_IK) {
                if (sc->bone_indices.size() < ArmIKBone::COUNT) continue;
                QuatWXYZ clavState[2] = {defaultQuatOrIdentity(sc, 0), defaultQuatOrIdentity(sc, 1)};
                QuatWXYZ handState[2] = {QuatWXYZ{}, QuatWXYZ{}};
                for (int i = 0; i < 2; ++i) {
                    if (i < (int)sc->default_pose.hand_quats.size()) handState[i] = QuatFromNal(sc->default_pose.hand_quats[i]);
                }
                std::array<float, 3> handPos[2] = {sc->default_pose.hand_pos[0], sc->default_pose.hand_pos[1]};
                float elbowSpin[2] = {sc->default_pose.elbow_spin[0], sc->default_pose.elbow_spin[1]};

                for (int bit = 0; bit < 2; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, clavState[bit])) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], clavState[bit], true, defaultQuatOrIdentity(sc, bit));
                }
                for (int bit = 2; bit < 4; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    int side = bit - 2;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, handState[side])) break;
                    QuatWXYZ defaultQ = side < (int)sc->default_pose.hand_quats.size()
                        ? QuatFromNal(sc->default_pose.hand_quats[side])
                        : QuatWXYZ{};
                    setAbsoluteTrackQuat(sc->bone_indices[bit], handState[side], true, defaultQ);

                    float pos[3];
                    if (ConsumeSampleVec3(fv0, fv1, cursor, sampleFrac, pos)) {
                        handPos[side] = {pos[0], pos[1], pos[2]};
                    }
                    if (cursor < (int)fv0.size()) elbowSpin[side] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                }

                if (sc->offset_locs.size() >= 2) {
                    int anchor = sc->bone_indices[ArmIKBone::NECK_PARENT];
                    int handAnchor = (sc->bone_indices.size() > ArmIKBone::PELVIS && sc->bone_indices[ArmIKBone::PELVIS] >= 0)
                        ? sc->bone_indices[ArmIKBone::PELVIS]
                        : anchor;
                    float local[16], world[16];

                    int clavRoles[2] = {ArmIKBone::L_CLAV, ArmIKBone::R_CLAV};
                    int handRoles[2] = {ArmIKBone::L_HAND, ArmIKBone::R_HAND};
                    for (int side = 0; side < 2; ++side) {
                        int clav = sc->bone_indices[clavRoles[side]];
                        matFromQuatPos(clavState[side], sc->offset_locs[side], local);
                        localToRuntimeParent(local, anchor, world);
                        storeRuntimeWorld(clav, world);
                        float clavWorld[16];
                        memcpy(clavWorld, world, sizeof(clavWorld));

                        int hand = sc->bone_indices[handRoles[side]];
                        matFromQuatPos(handState[side], handPos[side], local);
                        localToRuntimeParent(local, handAnchor, world);
                        storeRuntimeWorld(hand, world);
                        float handWorld[16];
                        memcpy(handWorld, world, sizeof(handWorld));

                        int upperRole = side == 0 ? ArmIKBone::L_UPPER : ArmIKBone::R_UPPER;
                        int foreRole = side == 0 ? ArmIKBone::L_FORE : ArmIKBone::R_FORE;
                        if (solveArmsIk && sc->has_ik && sc->offset_locs.size() > (size_t)upperRole) {
                            float upperWorld[16], foreWorld[16];
                            SolveNalIk(clavWorld, sc->offset_locs[upperRole], handWorld, sc->ik_data[side],
                                       elbowSpin[side], true, side == 1, upperWorld, foreWorld);
                            storeRuntimeWorld(sc->bone_indices[upperRole], upperWorld);
                            storeRuntimeWorld(sc->bone_indices[foreRole], foreWorld);
                        }
                    }

                    if (sc->fore_twist_locs.size() >= 4 && sc->bone_indices.size() > ArmIKBone::R_TWIST1) {
                        int twistBones[4] = {ArmIKBone::L_TWIST0, ArmIKBone::L_TWIST1,
                                             ArmIKBone::R_TWIST0, ArmIKBone::R_TWIST1};
                        int twistParents[4] = {ArmIKBone::L_FORE, ArmIKBone::L_TWIST0,
                                               ArmIKBone::R_FORE, ArmIKBone::R_TWIST0};
                        for (int i = 0; i < 4; ++i) {

                            Mat4BuildTwistLineXform(sc->fore_twist_locs[i], 0.0f, local);
                            localToRuntimeParent(local, sc->bone_indices[twistParents[i]], world);
                            storeRuntimeWorld(sc->bone_indices[twistBones[i]], world);
                        }
                    }
                }
            }
            else if (comp.comp_ix == NalComp::FAKEROOT_STD) {
                int fc = 0;
                if (comp.mask & 0x1) {
                    QuatWXYZ rootQ{};
                    ConsumeSampleQuat(fv0, fv1, fc, sampleFrac, rootQ);
                    Mat4FromQuat(rootQ, rootRotMat);
                    hasRootRot = true;

                    float pos[3];
                    if (ConsumeSampleVec3(fv0, fv1, fc, sampleFrac, pos)) {
                        rootOffset[0] += pos[0] - sc->default_pose.pelvis_pos[0];
                        rootOffset[1] += pos[1] - sc->default_pose.pelvis_pos[1];
                        rootOffset[2] += pos[2] - sc->default_pose.pelvis_pos[2];
                    }
                }
            }
            else if (comp.comp_ix == NalComp::TENTACLE) {
                for (size_t valueIndex = 0;
                     valueIndex < tentaclePose.size() &&
                     valueIndex < sc->default_pose.tentacle_values.size(); ++valueIndex) {
                    tentaclePose[valueIndex] = sc->default_pose.tentacle_values[valueIndex];
                }
                for (int channel = 0; channel < 15; ++channel) {
                    if ((comp.mask & (1u << channel)) == 0) continue;
                    if (cursor >= static_cast<int>(fv0.size())) break;
                    tentaclePose[channel] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                }
                hasTentaclePose = true;
            }
            else if (comp.comp_ix == NalComp::ARBITRARY_PO) {

                int quatCount = std::max(0, sc->default_pose.quat_count);
                int posCount = std::max(0, sc->default_pose.position_count);
                int channelCount = quatCount + posCount;
                std::vector<QuatWXYZ> arbStateQuats(quatCount, QuatWXYZ{});
                std::vector<std::array<float, 3>> arbStatePositions(posCount, {0.0f, 0.0f, 0.0f});
                for (int i = 0; i < quatCount && i < (int)sc->default_pose.quats.size(); ++i)
                    arbStateQuats[i] = QuatFromNal(sc->default_pose.quats[i]);
                for (int i = 0; i < posCount && i < (int)sc->default_pose.positions.size(); ++i)
                    arbStatePositions[i] = sc->default_pose.positions[i];

                for (int channel = 0; channel < channelCount; ++channel) {
                    if (!comp.channel_enabled(channel)) continue;
                    if (channel < quatCount) {
                        if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, arbStateQuats[channel])) break;
                    } else {
                        float xyz[3];
                        if (!ConsumeSampleVec3(fv0, fv1, cursor, sampleFrac, xyz)) break;
                        arbStatePositions[channel - quatCount] = {xyz[0], xyz[1], xyz[2]};
                    }
                }

                for (size_t oi = 0; oi < sc->arb_eval_order.size(); ++oi) {
                    uint32_t nodeIndex = sc->arb_eval_order[oi];
                    if (nodeIndex >= sc->arb_nodes.size()) continue;
                    const auto& node = sc->arb_nodes[nodeIndex];
                    int bid = node.my_matrix_ix;
                    if (bid < 0 || bid >= logicalBoneCount) continue;

                    QuatWXYZ quat{};
                    if (node.is_quat_anim && node.quat_ix < arbStateQuats.size())
                        quat = arbStateQuats[node.quat_ix];
                    else if (node.quat_ix < sc->arb_skel_quats.size())
                        quat = QuatFromNal(sc->arb_skel_quats[node.quat_ix]);
                    else if (node.quat_ix < sc->default_pose.quats.size())
                        quat = QuatFromNal(sc->default_pose.quats[node.quat_ix]);

                    std::array<float, 3> pos = {0.0f, 0.0f, 0.0f};
                    if (node.is_pos_anim && node.pos_ix < arbStatePositions.size())
                        pos = arbStatePositions[node.pos_ix];
                    else if (node.pos_ix < sc->arb_skel_positions.size())
                        pos = sc->arb_skel_positions[node.pos_ix];
                    else if (node.pos_ix < sc->default_pose.positions.size())
                        pos = sc->default_pose.positions[node.pos_ix];

                    float local[16], world[16];
                    matFromQuatPos(quat, pos, local);
                    localToRuntimeParent(local, node.parent_matrix_ix, world);
                    storeRuntimeWorld(bid, world);
                }
            }
            else if (comp.comp_ix == NalComp::FING5) {
                if (sc->bone_indices.size() < 30) continue;
                for (int bit = 0; bit < 30; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    QuatWXYZ sampled;
                    if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, sampled)) break;
                    setAbsoluteTrackQuat(sc->bone_indices[bit], sampled, true, defaultQuatOrIdentity(sc, bit));
                }
            }
            else if (comp.comp_ix == NalComp::FING5_REDUCED) {
                if (sc->bone_indices.size() < 30) continue;
                float baseY[8] = {};
                float baseZ[8] = {};
                float midHinge[10] = {};
                float tipHinge[10] = {};
                float defaultBaseY[8] = {};
                float defaultBaseZ[8] = {};
                float defaultMidHinge[10] = {};
                float defaultTipHinge[10] = {};
                for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                    baseY[i] = defaultBaseY[i] = sc->default_pose.base_y_tracks[i];
                for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                    baseZ[i] = defaultBaseZ[i] = sc->default_pose.base_z_tracks[i];
                for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                    midHinge[i] = defaultMidHinge[i] = sc->default_pose.hinge_tracks[i];
                for (int i = 0; i < 10 && i < (int)sc->default_pose.other_tracks.size(); ++i)
                    tipHinge[i] = defaultTipHinge[i] = sc->default_pose.other_tracks[i];
                QuatWXYZ thumb[2] = {};
                for (int i = 0; i < 2; ++i) {
                    QuatWXYZ q;
                    thumb[i] = GetDefaultQuat(sc, i, q) ? q : QuatWXYZ{};
                }

                for (int bit = 0; bit < 30; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (bit < 2) {
                        if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, thumb[bit])) break;
                    } else if (bit < 10) {
                        if (cursor + 1 >= (int)fv0.size()) break;
                        baseZ[bit - 2] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                        baseY[bit - 2] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    } else if (bit < 20) {
                        if (cursor >= (int)fv0.size()) break;
                        midHinge[bit - 10] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    } else {
                        if (cursor >= (int)fv0.size()) break;
                        tipHinge[bit - 20] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    }
                }

                for (int base = 0; base < 10; ++base) {
                    if (base < 2) {
                        QuatWXYZ def;
                        bool hasDef = GetDefaultQuat(sc, base, def);
                        setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                    } else {
                        setAbsoluteTrackQuat(sc->bone_indices[base],
                            QuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true,
                            QuatFromYZAngles(defaultBaseY[base - 2], defaultBaseZ[base - 2]));
                    }
                    setBoneDeltaQuat(sc->bone_indices[10 + base],
                        QuatAxisAngle(0, midHinge[base] - defaultMidHinge[base]));
                    setBoneDeltaQuat(sc->bone_indices[20 + base],
                        QuatAxisAngle(0, tipHinge[base] - defaultTipHinge[base]));
                }
            }
            else if (comp.comp_ix == NalComp::FING5_CURL) {
                if (sc->bone_indices.size() < 30) continue;
                float baseZ[8] = {};
                float hinge[10] = {};
                float defaultHinge[10] = {};
                for (int i = 0; i < 10 && i < (int)sc->default_pose.finger_curl.size(); ++i)
                    hinge[i] = defaultHinge[i] = sc->default_pose.finger_curl[i];
                QuatWXYZ thumb[2] = {};
                for (int i = 0; i < 2; ++i) {
                    QuatWXYZ q;
                    thumb[i] = GetDefaultQuat(sc, i, q) ? q : QuatWXYZ{};
                }

                for (int bit = 0; bit < 10; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (bit < 2) {
                        if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, thumb[bit])) break;
                        if (cursor < (int)fv0.size()) hinge[bit] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    } else {
                        if (cursor + 1 >= (int)fv0.size()) break;
                        baseZ[bit - 2] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                        hinge[bit] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    }
                }

                for (int base = 0; base < 10; ++base) {
                    if (base < 2) {
                        QuatWXYZ def;
                        bool hasDef = GetDefaultQuat(sc, base, def);
                        setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                    } else {
                        QuatWXYZ def = QuatFromYZAngles(defaultHinge[base], 0.0f);
                        setAbsoluteTrackQuat(sc->bone_indices[base], QuatFromYZAngles(hinge[base], baseZ[base - 2]), true, def);
                    }
                    setBoneDeltaQuat(sc->bone_indices[10 + base], QuatAxisAngle(0, hinge[base] - defaultHinge[base]));
                    setBoneDeltaQuat(sc->bone_indices[20 + base], QuatAxisAngle(0,
                        FingerTipHingeAngle(hinge[base], base < 2) - FingerTipHingeAngle(defaultHinge[base], base < 2)));
                }
            }
            else if (comp.comp_ix == NalComp::FING52) {
                if (sc->bone_indices.size() < 30) continue;
                QuatWXYZ thumb[2] = {};
                for (int i = 0; i < 2; ++i) {
                    QuatWXYZ q;
                    thumb[i] = GetDefaultQuat(sc, i, q) ? q : QuatWXYZ{};
                }
                float baseY[8] = {};
                float baseZ[8] = {};
                float hinge[10] = {};
                for (int i = 0; i < 8 && i < (int)sc->default_pose.base_y_tracks.size(); ++i)
                    baseY[i] = sc->default_pose.base_y_tracks[i];
                for (int i = 0; i < 8 && i < (int)sc->default_pose.base_z_tracks.size(); ++i)
                    baseZ[i] = sc->default_pose.base_z_tracks[i];
                for (int i = 0; i < 10 && i < (int)sc->default_pose.hinge_tracks.size(); ++i)
                    hinge[i] = sc->default_pose.hinge_tracks[i];

                for (int bit = 0; bit < 20; ++bit) {
                    if ((comp.mask & (1u << bit)) == 0) continue;
                    if (bit < 2) {
                        if (!ConsumeSampleQuat(fv0, fv1, cursor, sampleFrac, thumb[bit])) break;
                    } else if (bit < 10) {
                        if (cursor + 1 >= (int)fv0.size()) break;
                        baseY[bit - 2] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                        baseZ[bit - 2] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    } else {
                        if (cursor >= (int)fv0.size()) break;
                        hinge[bit - 10] = SampleTrack(fv0, fv1, cursor++, sampleFrac);
                    }
                }

                std::map<int, std::array<float, 16>> baseWorldByChain;
                if (sc->offset_locs.size() >= 30) {
                    auto anchorIdForChain = [&](int chain) -> int {
                        int slot = (chain == 1 || chain >= 6) ? 1 : 0;
                        int idx = FingerBone::L_HAND_PARENT + slot;
                        return (idx < (int)sc->bone_indices.size()) ? sc->bone_indices[idx] : -1;
                    };
                    auto loadAnchorWorld = [&](int anchorId, float* out) {
                        if (anchorId >= 0 && getRuntimeWorld(anchorId, out)) return;
                        if (anchorId >= 0 && anchorId < logicalBoneCount) {
                            int meshAnchor = nalToMeshBone(anchorId);
                            if (meshAnchor >= 0) {
                                memcpy(out, skeletonBones[meshAnchor].bindMatrix, sizeof(float) * 16);
                                return;
                            }
                        }
                        Mat4Identity(out);
                    };

                    float local[16], world[16], anchorWorld[16];
                    for (int base = 0; base < 10; ++base) {
                        QuatWXYZ q = (base < 2)
                            ? thumb[base]
                            : QuatFromYZAngles(baseY[base - 2], baseZ[base - 2]);
                        matFromQuatPos(q, sc->offset_locs[base], local);
                        loadAnchorWorld(anchorIdForChain(base), anchorWorld);
                        Mat4Multiply(anchorWorld, local, world);

                        std::array<float, 16> stored{};
                        memcpy(stored.data(), world, sizeof(float) * 16);
                        baseWorldByChain[base] = stored;
                        storeRuntimeWorld(sc->bone_indices[base], world);
                    }

                    for (int chain = 0; chain < 10; ++chain) {
                        auto baseIt = baseWorldByChain.find(chain);
                        if (baseIt == baseWorldByChain.end()) continue;

                        float midLocal[16], tipLocal[16], midWorld[16], tipWorld[16];
                        Mat4Fing52HingeLocal(hinge[chain], sc->offset_locs[10 + chain], midLocal);
                        Mat4Fing52HingeLocal(FingerTipHingeAngle(hinge[chain], chain < 2), sc->offset_locs[20 + chain], tipLocal);
                        Mat4Multiply(baseIt->second.data(), midLocal, midWorld);
                        Mat4Multiply(midWorld, tipLocal, tipWorld);
                        storeRuntimeWorld(sc->bone_indices[10 + chain], midWorld);
                        storeRuntimeWorld(sc->bone_indices[20 + chain], tipWorld);
                    }
                }

                for (int base = 0; base < 10; ++base) {
                    if (base < 2) {
                        QuatWXYZ def;
                        bool hasDef = GetDefaultQuat(sc, base, def);
                        setAbsoluteTrackQuat(sc->bone_indices[base], thumb[base], hasDef, def);
                    } else {
                        QuatWXYZ def = QuatFromYZAngles(
                            (base - 2 < (int)sc->default_pose.base_y_tracks.size()) ? sc->default_pose.base_y_tracks[base - 2] : 0.0f,
                            (base - 2 < (int)sc->default_pose.base_z_tracks.size()) ? sc->default_pose.base_z_tracks[base - 2] : 0.0f);
                        setAbsoluteTrackQuat(sc->bone_indices[base], QuatFromYZAngles(baseY[base - 2], baseZ[base - 2]), true, def);
                    }
                    float defHinge = (base < (int)sc->default_pose.hinge_tracks.size()) ? sc->default_pose.hinge_tracks[base] : 0.0f;
                    setBoneDeltaQuat(sc->bone_indices[10 + base], QuatAxisAngle(0, hinge[base] - defHinge));
                    setBoneDeltaQuat(sc->bone_indices[20 + base], QuatAxisAngle(0,
                        FingerTipHingeAngle(hinge[base], base < 2) - FingerTipHingeAngle(defHinge, base < 2)));
                }
            }
        }

        for (int i = 0; i < logicalBoneCount; ++i) {
            if (poseDeltas[i].hasRot) {
                float rot[16];
                Mat4FromQuat(poseDeltas[i].rot, rot);
                Mat4Multiply(&restLocal[i * 16], rot, &animLocal[i * 16]);
            } else {
                memcpy(&animLocal[i * 16], &restLocal[i * 16], sizeof(float) * 16);
            }
        }

        std::vector<uint8_t> visitState(logicalBoneCount, 0);
        std::function<void(int)> buildWorld = [&](int boneIdx) {
            if (boneIdx < 0 || boneIdx >= logicalBoneCount) return;
            if (visitState[boneIdx] == 2) return;
            if (visitState[boneIdx] == 1) {
                memcpy(&animWorld[boneIdx * 16], &animLocal[boneIdx * 16], sizeof(float) * 16);
                visitState[boneIdx] = 2;
                return;
            }
            visitState[boneIdx] = 1;
            auto runtimeIt = runtimeWorld.find(boneIdx);
            if (runtimeIt != runtimeWorld.end()) {
                memcpy(&animWorld[boneIdx * 16], runtimeIt->second.data(), sizeof(float) * 16);
            } else if (int parent = parentOf(boneIdx); parent >= 0) {
                buildWorld(parent);
                Mat4Multiply(&animWorld[parent * 16], &animLocal[boneIdx * 16], &animWorld[boneIdx * 16]);
            } else {
                memcpy(&animWorld[boneIdx * 16], &animLocal[boneIdx * 16], sizeof(float) * 16);
            }
            visitState[boneIdx] = 2;
        };

        for (int i = 0; i < logicalBoneCount; ++i) buildWorld(i);

        if (hasRootRot) {
            for (int i = 0; i < logicalBoneCount; i++) {
                float tmp[16];
                Mat4Multiply(rootRotMat, &animWorld[i * 16], tmp);
                memcpy(&animWorld[i * 16], tmp, sizeof(float) * 16);
            }
        }

        if (runtimeWorld.empty()) {
            for (int i = 0; i < logicalBoneCount; i++) {
                animWorld[i*16+12] += rootOffset[0];
                animWorld[i*16+13] += rootOffset[1];
                animWorld[i*16+14] += rootOffset[2];
            }
        }

        if (hasTentaclePose) {
            const NalComponentData* arbitrary = nullptr;
            for (const auto& component : animSkel->components) {
                if (component.type_id == NalCompType::ArbitraryPO) {
                    arbitrary = &component;
                    break;
                }
            }
            if (arbitrary) {
                static const char* kPrefixes[5] = {
                    "uplefttent_", "uprighttent_", "lowlefttent_",
                    "lowrighttent_", "tongue_"};
                static const char* kNames[5] = {
                    "UpLeftTent", "UpRightTent", "LowLeftTent",
                    "LowRightTent", "Tongue"};
                std::array<std::map<int, std::array<float, 3>>, 5> controlPoints;
                for (const auto& node : arbitrary->arb_nodes) {
                    std::string lowerName = node.name;
                    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                    for (int chainIndex = 0; chainIndex < 5; ++chainIndex) {
                        const std::string prefix = kPrefixes[chainIndex];
                        if (lowerName.rfind(prefix, 0) != 0) continue;
                        const int pointNumber = std::atoi(lowerName.c_str() + prefix.size());
                        const int logical = node.my_matrix_ix;
                        if (pointNumber <= 0 || logical < 0 || logical >= logicalBoneCount)
                            break;
                        controlPoints[chainIndex][pointNumber] = {
                            animWorld[logical * 16 + 12],
                            animWorld[logical * 16 + 13],
                            animWorld[logical * 16 + 14]};
                        nalBonePositions[logical] = controlPoints[chainIndex][pointNumber];
                        nalMaxBoneIndex = std::max(nalMaxBoneIndex, logical);
                        break;
                    }
                }

                for (int chainIndex = 0; chainIndex < 5; ++chainIndex) {
                    if (controlPoints[chainIndex].size() < 2) continue;
                    TentaclePreviewChain chain;
                    chain.name = kNames[chainIndex];
                    chain.diameter = std::max(0.0f, tentaclePose[chainIndex * 3]);
                    chain.activity = tentaclePose[chainIndex * 3 + 1];
                    chain.pull = tentaclePose[chainIndex * 3 + 2];
                    chain.tongue = chainIndex == 4;
                    for (const auto& point : controlPoints[chainIndex])
                        chain.controlPoints.push_back(point.second);
                    if (chain.diameter > 0.0001f)
                        evaluatedTentacles.push_back(std::move(chain));
                }
            }
        }

        for (int logical = 0; logical < logicalBoneCount; logical++) {
            int mesh = nalToMeshBone(logical);
            if (mesh < 0) continue;
            Mat4Multiply(&animWorld[logical * 16], skeletonBones[mesh].invBindMatrix,
                         &globalBoneMatData[mesh * 16]);
        }

        for (int logical = 0; logical < logicalBoneCount; logical++) {
            int mesh = nalToMeshBone(logical);
            if (mesh < 0) continue;
            bonePosWorld[mesh * 3 + 0] = animWorld[logical * 16 + 12];
            bonePosWorld[mesh * 3 + 1] = animWorld[logical * 16 + 13];
            bonePosWorld[mesh * 3 + 2] = animWorld[logical * 16 + 14];
            if (nalBonePositions.count(logical)) {
                nalBonePositions[logical] = {bonePosWorld[mesh * 3 + 0],
                                             bonePosWorld[mesh * 3 + 1],
                                             bonePosWorld[mesh * 3 + 2]};
            }
        }

        skinningActive = true;
        }
    }

    std::map<int, std::array<float, 16>> manualPivotTransforms;
    for (const auto& entry : manualBoneRotations) {
        int boneIdx = entry.first;
        int meshIdx = nalToMeshBone(boneIdx);
        const auto& angles = entry.second;
        if (meshIdx < 0 || !HasAnyRotation(angles)) continue;

        float pivot[3] = {
            bonePosWorld[meshIdx * 3 + 0],
            bonePosWorld[meshIdx * 3 + 1],
            bonePosWorld[meshIdx * 3 + 2]
        };

        std::array<float, 16> transform{};
        Mat4PivotEuler(angles, pivot, transform.data());
        manualPivotTransforms[boneIdx] = transform;
    }

    if (!manualPivotTransforms.empty()) {
        auto parentOf = [&](int idx) -> int {
            if (!loadedSkeleton) return -1;
            auto it = loadedSkeleton->parent_map.find(idx);
            return (it != loadedSkeleton->parent_map.end()) ? it->second : -1;
        };

        auto boneDepth = [&](int idx) -> int {
            int depth = 0;
            for (int guard = 0; guard < 256; ++guard) {
                idx = parentOf(idx);
                if (idx < 0) break;
                ++depth;
            }
            return depth;
        };

        auto isDescendantOrSelf = [&](int boneIdx, int ancestorIdx) -> bool {
            if (boneIdx == ancestorIdx) return true;
            for (int guard = 0; guard < 256; ++guard) {
                boneIdx = parentOf(boneIdx);
                if (boneIdx < 0) return false;
                if (boneIdx == ancestorIdx) return true;
            }
            return false;
        };

        for (int boneIdx = 0; boneIdx < logicalBoneCount; ++boneIdx) {
            int meshIdx = nalToMeshBone(boneIdx);
            if (meshIdx < 0) continue;
            std::vector<int> affectingBones;
            for (const auto& entry : manualPivotTransforms) {
                if (isDescendantOrSelf(boneIdx, entry.first)) {
                    affectingBones.push_back(entry.first);
                }
            }
            if (affectingBones.empty()) continue;

            std::sort(affectingBones.begin(), affectingBones.end(),
                [&](int a, int b) { return boneDepth(a) < boneDepth(b); });

            float combined[16];
            Mat4Identity(combined);
            for (int activeBone : affectingBones) {
                float next[16];
                Mat4Multiply(combined, manualPivotTransforms[activeBone].data(), next);
                memcpy(combined, next, sizeof(next));
            }

            float finalSkin[16];
            Mat4Multiply(combined, &globalBoneMatData[meshIdx * 16], finalSkin);
            memcpy(&globalBoneMatData[meshIdx * 16], finalSkin, sizeof(finalSkin));

            float bindPos[3] = {
                skeletonBones[meshIdx].position[0],
                skeletonBones[meshIdx].position[1],
                skeletonBones[meshIdx].position[2]
            };
            float posedPos[3];
            Mat4TransformPoint(finalSkin, bindPos, posedPos);
            bonePosWorld[meshIdx * 3 + 0] = posedPos[0];
            bonePosWorld[meshIdx * 3 + 1] = posedPos[1];
            bonePosWorld[meshIdx * 3 + 2] = posedPos[2];
            if (nalBonePositions.count(boneIdx)) {
                nalBonePositions[boneIdx] = {posedPos[0], posedPos[1], posedPos[2]};
            }
        }

        skinningActive = true;
    }

    if (skinningActive && skeletonVbo && loadedSkeleton && !nalBoneVboOrder.empty()) {
        struct BoneVert { float x, y, z, r, g, b; };
        std::vector<BoneVert> pointVerts;
        std::vector<BoneVert> lineVerts;
        pointVerts.reserve(nalBoneVboOrder.size());

        for (int nalIdx : nalBoneVboOrder) {
            auto it = nalBonePositions.find(nalIdx);
            if (it == nalBonePositions.end()) continue;
            const auto& p = it->second;
            pointVerts.push_back({p[0], p[1], p[2], 1.0f, 1.0f, 0.0f});
        }

        for (const auto& [childIdx, parentIdx] : loadedSkeleton->parent_map) {
            if (parentIdx < 0) continue;
            auto childIt = nalBonePositions.find(childIdx);
            auto parentIt = nalBonePositions.find(parentIdx);
            if (childIt == nalBonePositions.end() || parentIt == nalBonePositions.end()) continue;
            const auto& cp = childIt->second;
            const auto& pp = parentIt->second;
            lineVerts.push_back({pp[0], pp[1], pp[2], 0.0f, 0.9f, 1.0f});
            lineVerts.push_back({cp[0], cp[1], cp[2], 0.0f, 0.9f, 1.0f});
        }

        std::vector<BoneVert> allVerts;
        allVerts.reserve(pointVerts.size() + lineVerts.size());
        allVerts.insert(allVerts.end(), pointVerts.begin(), pointVerts.end());
        allVerts.insert(allVerts.end(), lineVerts.begin(), lineVerts.end());
        skeletonBoneCount = (int)pointVerts.size();
        skeletonLineVertCount = (int)lineVerts.size();

        glBindBuffer(GL_ARRAY_BUFFER, skeletonVbo);
        glBufferData(GL_ARRAY_BUFFER, allVerts.size() * sizeof(BoneVert), allVerts.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    std::string skeletonLabel = loadedSkeletonName;
    std::transform(skeletonLabel.begin(), skeletonLabel.end(), skeletonLabel.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    bool carnageTentacleStyle = skeletonLabel.find("carnage") != std::string::npos;
    if (!carnageTentacleStyle) {
        for (const auto& previewMesh : previewMeshes) {
            std::string meshLabel = previewMesh.meshName + " " + previewMesh.sourcePack;
            std::transform(meshLabel.begin(), meshLabel.end(), meshLabel.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (meshLabel.find("carnage") != std::string::npos) {
                carnageTentacleStyle = true;
                break;
            }
        }
    }
    UpdateTentaclePreviewMesh(
        proceduralTentacleMesh, evaluatedTentacles, carnageTentacleStyle);

    if (captureEvaluatedSkinMatrices) {
        evaluatedSkinningActive = skinningActive;
        evaluatedGlobalBoneMatrices = globalBoneMatData;
    }
    if (skipDrawAfterPoseEvaluation) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    glUniform1i(locUseSkinning, skinningActive ? 1 : 0);

    GLint locDiffTexture = glGetUniformLocation(modelProgram, "diffTexture");
    GLint locDetailTexture = glGetUniformLocation(modelProgram, "detailTexture");
    GLint locHasTexture = glGetUniformLocation(modelProgram, "hasTexture");
    GLint locHasDetailTexture = glGetUniformLocation(modelProgram, "hasDetailTexture");
    GLint locIsPersonMaterial = glGetUniformLocation(modelProgram, "isPersonMaterial");
    GLint locPersonLighting = glGetUniformLocation(modelProgram, "personLighting");
    GLint locPersonUseInk = glGetUniformLocation(modelProgram, "personUseInk");
    GLint locProceduralLit = glGetUniformLocation(modelProgram, "proceduralLit");
    GLint locPersonBaseColor = glGetUniformLocation(modelProgram, "personBaseColor");
    GLint locPreviewLightDir = glGetUniformLocation(modelProgram, "previewLightDir");
    GLint locBlendMode = glGetUniformLocation(modelProgram, "blendMode");
    GLint locAlphaRef = glGetUniformLocation(modelProgram, "alphaRef");
    GLint locIsFakeShadow = glGetUniformLocation(modelProgram, "isFakeShadow");
    GLint locIsColorVolume = glGetUniformLocation(modelProgram, "isColorVolume");
    GLint locIsHighlighted = glGetUniformLocation(modelProgram, "isHighlighted");
    GLint locUseInstancing = glGetUniformLocation(modelProgram, "useInstancing");
    GLint locSelectedInstanceIndex = glGetUniformLocation(modelProgram, "selectedInstanceIndex");
    GLint locDebugTransparent = glGetUniformLocation(modelProgram, "debugTransparent");
    GLint locIsWater = glGetUniformLocation(modelProgram, "isWater");
    GLint locTime = glGetUniformLocation(modelProgram, "time");
    GLint locViewPosWorld = glGetUniformLocation(modelProgram, "viewPosWorld");

    float nowSeconds = (float)glfwGetTime();
    glUniform1f(locTime, nowSeconds);
    glUniform3f(locViewPosWorld, camPos[0], camPos[1], camPos[2]);

    glUniform3f(locPreviewLightDir, 0.0f, 0.57735026f, 0.81649655f);

    auto uploadSectionBoneMatrices = [&](const RenderMesh& mesh) {
        if (!skinningActive) return;

        std::vector<float> sectionBoneMatData(MAX_BONES * 16, 0.f);
        for (int i = 0; i < MAX_BONES; ++i) {
            Mat4Identity(&sectionBoneMatData[i * 16]);
        }

        if (!mesh.bonePalette.empty()) {
            int localCount = std::min((int)mesh.bonePalette.size(), MAX_BONES);
            for (int localIdx = 0; localIdx < localCount; ++localIdx) {
                int globalIdx = mesh.bonePalette[localIdx];
                if (globalIdx < 0 || globalIdx >= meshBoneCount) continue;
                memcpy(&sectionBoneMatData[localIdx * 16],
                       &globalBoneMatData[globalIdx * 16],
                       sizeof(float) * 16);
            }
        } else {
            int localCount = std::min(meshBoneCount, MAX_BONES);
            for (int localIdx = 0; localIdx < localCount; ++localIdx) {
                memcpy(&sectionBoneMatData[localIdx * 16],
                       &globalBoneMatData[localIdx * 16],
                       sizeof(float) * 16);
            }
        }

        glUniformMatrix4fv(locBoneMatrices, MAX_BONES, GL_FALSE, sectionBoneMatData.data());
    };

    auto isInFrustum = [&](const float bboxMin[3], const float bboxMax[3]) -> bool {
        float cx = (bboxMin[0] + bboxMax[0]) * 0.5f;
        float cy = (bboxMin[1] + bboxMax[1]) * 0.5f;
        float cz = (bboxMin[2] + bboxMax[2]) * 0.5f;

        float rx = (bboxMax[0] - bboxMin[0]) * 0.5f;
        float ry = (bboxMax[1] - bboxMin[1]) * 0.5f;
        float rz = (bboxMax[2] - bboxMin[2]) * 0.5f;
        float radius = sqrt(rx*rx + ry*ry + rz*rz);

        float dx = cx - camPos[0];
        float dy = cy - camPos[1];
        float dz = cz - camPos[2];

        float dist = dx * camFront[0] + dy * camFront[1] + dz * camFront[2];

        if (dist < -radius) return false;
        if (dist > 15000.0f + radius) return false;

        return true;
    };

    glDisable(GL_CULL_FACE);

    std::vector<int> opaqueBucket, punchBucket, blendBucket;
    opaqueBucket.reserve(previewMeshes.size());
    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];
        if ((!isWorldMode && m.isHidden) || m.indexCount <= 0) continue;
        if (!isWorldMode && !isInFrustum(m.bboxMin, m.bboxMax)) continue;

        if (m.isDebugTransparent) {
            blendBucket.push_back(i);
        } else if (m.isFakeShadow) {
            blendBucket.push_back(i);
        } else if (m.isAlphaTest) {
            punchBucket.push_back(i);
        } else if (m.isTranslucent) {
            blendBucket.push_back(i);
        } else {
            opaqueBucket.push_back(i);
        }
    }

    auto drawOne = [&](int i) {
        const auto& m = previewMeshes[i];
        unsigned int activeDiffuseTexture = m.textureId;
        if (!m.textureFrames.empty()) {

            const size_t frame = static_cast<size_t>(std::max(0.0f, floorf(nowSeconds * 30.0f))) %
                m.textureFrames.size();
            activeDiffuseTexture = m.textureFrames[frame];
        }
        if (activeDiffuseTexture != 0) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, activeDiffuseTexture);
            glUniform1i(locDiffTexture, 0);
            glUniform1i(locHasTexture, 1);
        } else {
            glUniform1i(locHasTexture, 0);
        }
        if (m.secondaryTextureId != 0) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, m.secondaryTextureId);
            glUniform1i(locDetailTexture, 1);
            glUniform1i(locHasDetailTexture, 1);
            glActiveTexture(GL_TEXTURE0);
        } else {
            glUniform1i(locHasDetailTexture, 0);
        }
        glUniform1i(locIsPersonMaterial, m.isPersonMaterial ? 1 : 0);
        glUniform1i(locPersonLighting, (m.isPersonMaterial && m.personLighting != 0) ? 1 : 0);
        glUniform1i(locPersonUseInk,
                    (m.isPersonMaterial && (m.personEnvA != 0 || m.personEnvB != 0)) ? 1 : 0);
        glUniform1i(locProceduralLit, 0);
        glUniform4f(locPersonBaseColor, m.personBaseColor[0], m.personBaseColor[1],
                    m.personBaseColor[2], m.personBaseColor[3]);

        int effectiveBlend = (m.isFakeShadow || m.isColorVolume || m.isDebugTransparent)
                             ? 2 : (int)m.blendMode;
        glUniform1i(locBlendMode, effectiveBlend);
        glUniform1f(locAlphaRef, 0.5f);
        glUniform1i(locIsFakeShadow, m.isFakeShadow ? 1 : 0);
        glUniform1i(locIsColorVolume, m.isColorVolume ? 1 : 0);
        const bool instanced = !m.instances.empty();
        glUniform1i(locUseInstancing, instanced ? 1 : 0);
        glUniform1i(locIsHighlighted, (!instanced && i == selectedMeshIndex) ? 1 : 0);
        glUniform1f(locSelectedInstanceIndex,
                    (instanced && i == selectedMeshIndex) ? (float)selectedMeshInstanceIndex : -1.0f);
        glUniform1i(locDebugTransparent, m.isDebugTransparent ? 1 : 0);
        glUniform1i(locIsWater, m.isWater ? 1 : 0);
        uploadSectionBoneMatrices(m);
        glBindVertexArray(m.vao);
        if (instanced) {
            if (m.instanceDrawCount > 0) {
                glDrawElementsInstanced(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0,
                                        m.instanceDrawCount);
            }
        } else {
            glDrawElements(m.mode, m.indexCount, GL_UNSIGNED_SHORT, 0);
        }
    };

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    for (int i : opaqueBucket) drawOne(i);

    if (proceduralTentacleMesh.vao && proceduralTentacleMesh.indexCount > 0) {
        glUniform1i(locUseSkinning, 0);
        glUniform1i(locHasTexture, 0);
        glUniform1i(locHasDetailTexture, 0);
        glUniform1i(locIsPersonMaterial, 0);
        glUniform1i(locPersonLighting, 0);
        glUniform1i(locPersonUseInk, 0);
        glUniform1i(locProceduralLit, 1);
        glUniform4f(locPersonBaseColor, 1.0f, 1.0f, 1.0f, 1.0f);
        glUniform1i(locBlendMode, static_cast<int>(NGLBM_OPAQUE));
        glUniform1f(locAlphaRef, 0.5f);
        glUniform1i(locIsFakeShadow, 0);
        glUniform1i(locIsColorVolume, 0);
        glUniform1i(locUseInstancing, 0);
        glUniform1i(locIsHighlighted, 0);
        glUniform1f(locSelectedInstanceIndex, -1.0f);
        glUniform1i(locDebugTransparent, 0);
        glUniform1i(locIsWater, 0);
        glBindVertexArray(proceduralTentacleMesh.vao);
        glDrawElements(proceduralTentacleMesh.mode, proceduralTentacleMesh.indexCount,
                       GL_UNSIGNED_SHORT, nullptr);
        glUniform1i(locUseSkinning, skinningActive ? 1 : 0);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    for (int i : punchBucket) drawOne(i);

    if (!blendBucket.empty()) {
        std::sort(blendBucket.begin(), blendBucket.end(), [&](int a, int b) {
            const auto& ma = previewMeshes[a];
            const auto& mb = previewMeshes[b];
            float ca[3] = { (ma.bboxMin[0]+ma.bboxMax[0])*0.5f,
                            (ma.bboxMin[1]+ma.bboxMax[1])*0.5f,
                            (ma.bboxMin[2]+ma.bboxMax[2])*0.5f };
            float cb[3] = { (mb.bboxMin[0]+mb.bboxMax[0])*0.5f,
                            (mb.bboxMin[1]+mb.bboxMax[1])*0.5f,
                            (mb.bboxMin[2]+mb.bboxMax[2])*0.5f };
            float da = (ca[0]-camPos[0])*(ca[0]-camPos[0]) +
                       (ca[1]-camPos[1])*(ca[1]-camPos[1]) +
                       (ca[2]-camPos[2])*(ca[2]-camPos[2]);
            float db = (cb[0]-camPos[0])*(cb[0]-camPos[0]) +
                       (cb[1]-camPos[1])*(cb[1]-camPos[1]) +
                       (cb[2]-camPos[2])*(cb[2]-camPos[2]);
            return da > db;
        });

        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);

        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);
        for (int i : blendBucket) {
            const auto& m = previewMeshes[i];

            switch (m.blendMode) {
                case NGLBM_ADDITIVE:
                case NGLBM_CONST_ADDITIVE:
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                    break;
                case NGLBM_SUBTRACTIVE:
                case NGLBM_CONST_SUBTRACTIVE:
                    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                    break;
                case NGLBM_DESTALPHA_ADDITIVE:
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_ZERO, GL_SRC_ALPHA);
                    break;
                case NGLBM_BLEND:
                case NGLBM_CONST_BLEND:
                default:
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    break;
            }
            drawOne(i);
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBlendEquation(GL_FUNC_ADD);

    if (showCollision) {
        if (collisionVertCount == 0) BuildCollisionVisual();
        RenderCollisionOverlay();
    }

    if (showSkeleton && !isWorldMode && skeletonBoneCount > 0) {
        RenderSkeletonOverlay();
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, modelFbo);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void SpiderManTool::ComputeNALBonePositions() {
    nalBonePositions.clear();
    nalMaxBoneIndex = -1;
    if (!loadedSkeleton) {
        return;
    }
    if (loadedSkeleton->bone_map.empty() && skeletonBones.empty()) {
        return;
    }

    auto nameToPos = [](const std::string& name) -> std::array<float,3> {

        if (name == "pelvis")       return {0.0f, 2.8f, 0.0f};
        if (name == "spine")        return {0.0f, 3.2f, 0.0f};
        if (name == "spine1")       return {0.0f, 3.6f, 0.0f};
        if (name == "spine2")       return {0.0f, 4.0f, 0.0f};
        if (name == "neck")         return {0.0f, 4.5f, 0.0f};
        if (name == "head")         return {0.0f, 5.0f, 0.0f};
        if (name == "l_clavicle")   return {0.3f, 4.3f, 0.0f};
        if (name == "l_upperarm")   return {0.8f, 4.2f, 0.0f};
        if (name == "l_forearm")    return {1.4f, 4.0f, 0.0f};
        if (name == "l_hand")       return {1.9f, 3.8f, 0.0f};
        if (name == "r_clavicle")   return {-0.3f, 4.3f, 0.0f};
        if (name == "r_upperarm")   return {-0.8f, 4.2f, 0.0f};
        if (name == "r_forearm")    return {-1.4f, 4.0f, 0.0f};
        if (name == "r_hand")       return {-1.9f, 3.8f, 0.0f};
        if (name == "l_thigh")      return {0.3f, 2.5f, 0.0f};
        if (name == "l_calf")       return {0.3f, 1.5f, 0.0f};
        if (name == "l_foot")       return {0.3f, 0.3f, 0.2f};
        if (name == "l_toe")        return {0.3f, 0.0f, 0.5f};
        if (name == "r_thigh")      return {-0.3f, 2.5f, 0.0f};
        if (name == "r_calf")       return {-0.3f, 1.5f, 0.0f};
        if (name == "r_foot")       return {-0.3f, 0.3f, 0.2f};
        if (name == "r_toe")        return {-0.3f, 0.0f, 0.5f};
        if (name == "l_fore_twist0") return {1.5f, 4.0f, 0.0f};
        if (name == "l_fore_twist1") return {1.6f, 3.9f, 0.0f};
        if (name == "r_fore_twist0") return {-1.5f, 4.0f, 0.0f};
        if (name == "r_fore_twist1") return {-1.6f, 3.9f, 0.0f};
        return {0.0f, 3.0f, 0.0f};
    };

    auto setPos = [&](int idx, float x, float y, float z) {
        if (idx < 0) return;
        nalBonePositions[idx] = {x, y, z};
        if (idx > nalMaxBoneIndex) nalMaxBoneIndex = idx;
    };
    auto getPos = [&](int idx) -> std::array<float,3> {
        if (nalBonePositions.count(idx)) return nalBonePositions[idx];
        return {0,0,0};
    };
    auto chainBone = [&](int childIdx, int parentIdx, const std::array<float,3>& offset) {
        auto pp = getPos(parentIdx);
        setPos(childIdx, pp[0]+offset[0], pp[1]+offset[1], pp[2]+offset[2]);
    };

    if (!skeletonBones.empty()) {
        int named = 0;
        const bool entityMapMatches = activeBoneMapping.valid &&
            activeBoneMapping.meshPoseCount == skeletonBones.size() &&
            activeBoneMapping.meshToLogical.size() == skeletonBones.size();
        for (int meshIdx = 0; meshIdx < static_cast<int>(skeletonBones.size()); ++meshIdx) {
            const int logicalIdx = entityMapMatches
                ? activeBoneMapping.meshToLogical[meshIdx]
                : meshIdx;
            const auto& bone = skeletonBones[meshIdx];
            setPos(logicalIdx, bone.position[0], bone.position[1], bone.position[2]);
            if (loadedSkeleton->bone_map.count(logicalIdx)) ++named;
        }

        if (nalBonePositions.size() >= 3) {
            (void)named;
            return;
        }

        nalBonePositions.clear();
        nalMaxBoneIndex = -1;
    }

    bool hasOffsetLocs = false;
    for (auto& c : loadedSkeleton->components) {
        if (c.type_id != NalCompType::TorsoHead_TwoNeck && c.type_id != NalCompType::TorsoHead_OneNeck) continue;
        if (c.bone_indices.size() >= 6 && c.offset_locs.size() >= 5) {

            for (auto& ol : c.offset_locs) {
                if (fabsf(ol[0]) > 0.001f || fabsf(ol[1]) > 0.001f || fabsf(ol[2]) > 0.001f) {
                    hasOffsetLocs = true;
                    break;
                }
            }
        }
        break;
    }

    if (hasOffsetLocs) {

        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::TorsoHead_TwoNeck && c.type_id != NalCompType::TorsoHead_OneNeck) continue;
            if (c.bone_indices.size() < 6 || c.offset_locs.size() < 5) continue;
            std::array<float,3> pelvisPos = {0, 0, 0};
            if (c.default_pose.valid) pelvisPos = c.default_pose.pelvis_pos;
            setPos(c.bone_indices[TorsoBone::PELVIS], pelvisPos[0], pelvisPos[1], pelvisPos[2]);
            chainBone(c.bone_indices[TorsoBone::SPINE],  c.bone_indices[TorsoBone::PELVIS], c.offset_locs[0]);
            chainBone(c.bone_indices[TorsoBone::SPINE1], c.bone_indices[TorsoBone::SPINE],  c.offset_locs[1]);
            chainBone(c.bone_indices[TorsoBone::SPINE2], c.bone_indices[TorsoBone::SPINE1], c.offset_locs[2]);
            chainBone(c.bone_indices[TorsoBone::NECK],   c.bone_indices[TorsoBone::SPINE2], c.offset_locs[3]);
            chainBone(c.bone_indices[TorsoBone::HEAD],   c.bone_indices[TorsoBone::NECK],   c.offset_locs[4]);
            break;
        }
        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::LegsFeet_IK && c.type_id != NalCompType::LegsFeet_Compressed) continue;
            if (c.bone_indices.size() < 8 || c.offset_locs.size() < 8) continue;
            if (c.type_id == NalCompType::LegsFeet_IK) {
                int pelvisIdx = (c.bone_indices.size() > LegBone::PELVIS) ? c.bone_indices[LegBone::PELVIS] : 0;
                if (!nalBonePositions.count(pelvisIdx)) setPos(pelvisIdx, 0, 0, 0);
                chainBone(c.bone_indices[LegBone::L_THIGH], pelvisIdx, c.offset_locs[LegBone::L_THIGH]);
                chainBone(c.bone_indices[LegBone::L_CALF],  c.bone_indices[LegBone::L_THIGH], c.offset_locs[LegBone::L_CALF]);
                chainBone(c.bone_indices[LegBone::L_FOOT],  c.bone_indices[LegBone::L_CALF],  c.offset_locs[LegBone::L_FOOT]);
                chainBone(c.bone_indices[LegBone::L_TOE],   c.bone_indices[LegBone::L_FOOT],  c.offset_locs[LegBone::L_TOE]);
                chainBone(c.bone_indices[LegBone::R_THIGH], pelvisIdx, c.offset_locs[LegBone::R_THIGH]);
                chainBone(c.bone_indices[LegBone::R_CALF],  c.bone_indices[LegBone::R_THIGH], c.offset_locs[LegBone::R_CALF]);
                chainBone(c.bone_indices[LegBone::R_FOOT],  c.bone_indices[LegBone::R_CALF],  c.offset_locs[LegBone::R_FOOT]);
                chainBone(c.bone_indices[LegBone::R_TOE],   c.bone_indices[LegBone::R_FOOT],  c.offset_locs[LegBone::R_TOE]);
            } else {
                int rootIdx = (c.bone_indices.size() > LegStdBone::ROOT) ? c.bone_indices[LegStdBone::ROOT] : 0;
                if (!nalBonePositions.count(rootIdx)) setPos(rootIdx, 0, 0, 0);
                chainBone(c.bone_indices[LegStdBone::L_THIGH], rootIdx, c.offset_locs[LegStdBone::L_THIGH]);
                chainBone(c.bone_indices[LegStdBone::L_CALF], c.bone_indices[LegStdBone::L_THIGH], c.offset_locs[LegStdBone::L_CALF]);
                chainBone(c.bone_indices[LegStdBone::L_FOOT], c.bone_indices[LegStdBone::L_CALF], c.offset_locs[LegStdBone::L_FOOT]);
                chainBone(c.bone_indices[LegStdBone::L_TOE], c.bone_indices[LegStdBone::L_FOOT], c.offset_locs[LegStdBone::L_TOE]);
                chainBone(c.bone_indices[LegStdBone::R_THIGH], rootIdx, c.offset_locs[LegStdBone::R_THIGH]);
                chainBone(c.bone_indices[LegStdBone::R_CALF], c.bone_indices[LegStdBone::R_THIGH], c.offset_locs[LegStdBone::R_CALF]);
                chainBone(c.bone_indices[LegStdBone::R_FOOT], c.bone_indices[LegStdBone::R_CALF], c.offset_locs[LegStdBone::R_FOOT]);
                chainBone(c.bone_indices[LegStdBone::R_TOE], c.bone_indices[LegStdBone::R_FOOT], c.offset_locs[LegStdBone::R_TOE]);
            }
            break;
        }
        for (auto& c : loadedSkeleton->components) {
            if (c.type_id != NalCompType::ArmsHands_IK && c.type_id != NalCompType::ArmsHands_Compressed) continue;
            if (c.offset_locs.size() < 8) continue;
            int armParent = 0;
            for (auto& [idx, name] : loadedSkeleton->bone_map) {
                if (name == "spine2" && nalBonePositions.count(idx)) { armParent = idx; break; }
            }
            int l_clav = c.bone_indices[0], l_upper, l_fore, l_hand, r_clav, r_upper, r_fore, r_hand;
            if (c.type_id == NalCompType::ArmsHands_IK) {
                l_clav = c.bone_indices[ArmIKBone::L_CLAV]; r_clav = c.bone_indices[ArmIKBone::R_CLAV];
                l_upper = c.bone_indices[ArmIKBone::L_UPPER]; r_upper = c.bone_indices[ArmIKBone::R_UPPER];
                l_fore = c.bone_indices[ArmIKBone::L_FORE]; r_fore = c.bone_indices[ArmIKBone::R_FORE];
                l_hand = c.bone_indices[ArmIKBone::L_HAND]; r_hand = c.bone_indices[ArmIKBone::R_HAND];
            } else {
                l_clav = c.bone_indices[ArmBone::L_CLAV]; r_clav = c.bone_indices[ArmBone::R_CLAV];
                l_upper = c.bone_indices[ArmBone::L_UPPER]; r_upper = c.bone_indices[ArmBone::R_UPPER];
                l_fore = c.bone_indices[ArmBone::L_FORE]; r_fore = c.bone_indices[ArmBone::R_FORE];
                l_hand = c.bone_indices[ArmBone::L_HAND]; r_hand = c.bone_indices[ArmBone::R_HAND];
            }
            chainBone(l_clav, armParent, c.offset_locs[0]);
            chainBone(l_upper, l_clav, c.offset_locs[1]);
            chainBone(l_fore, l_upper, c.offset_locs[2]);
            chainBone(l_hand, l_fore, c.offset_locs[3]);
            chainBone(r_clav, armParent, c.offset_locs[4]);
            chainBone(r_upper, r_clav, c.offset_locs[5]);
            chainBone(r_fore, r_upper, c.offset_locs[6]);
            chainBone(r_hand, r_fore, c.offset_locs[7]);

            if (c.fore_twist_locs.size() >= 4) {
                int lt0=-1, lt1=-1, rt0=-1, rt1=-1;
                if (c.type_id == NalCompType::ArmsHands_IK && (int)c.bone_indices.size() > ArmIKBone::R_TWIST1) {
                    lt0=c.bone_indices[ArmIKBone::L_TWIST0]; lt1=c.bone_indices[ArmIKBone::L_TWIST1];
                    rt0=c.bone_indices[ArmIKBone::R_TWIST0]; rt1=c.bone_indices[ArmIKBone::R_TWIST1];
                } else if ((int)c.bone_indices.size() > ArmBone::R_TWIST1) {
                    lt0=c.bone_indices[ArmBone::L_TWIST0]; lt1=c.bone_indices[ArmBone::L_TWIST1];
                    rt0=c.bone_indices[ArmBone::R_TWIST0]; rt1=c.bone_indices[ArmBone::R_TWIST1];
                }
                if (lt0>=0) chainBone(lt0, l_fore, c.fore_twist_locs[0]);
                if (lt1>=0 && lt0>=0) chainBone(lt1, lt0, c.fore_twist_locs[1]);
                if (rt0>=0) chainBone(rt0, r_fore, c.fore_twist_locs[2]);
                if (rt1>=0 && rt0>=0) chainBone(rt1, rt0, c.fore_twist_locs[3]);
            }
            break;
        }
    }

    if ((int)nalBonePositions.size() < 3) {
        nalBonePositions.clear();
        nalMaxBoneIndex = -1;

        for (const auto& [idx, name] : loadedSkeleton->bone_map) {
            auto pos = nameToPos(name);
            setPos(idx, pos[0], pos[1], pos[2]);
        }
    }

}

void SpiderManTool::BuildSkeletonVisual(const std::vector<uint8_t>& pcmData) {
    if (skeletonVao) { glDeleteVertexArrays(1, &skeletonVao); skeletonVao = 0; }
    if (skeletonVbo) { glDeleteBuffers(1, &skeletonVbo); skeletonVbo = 0; }
    skeletonBoneCount = 0;
    skeletonLineVertCount = 0;
    skeletonBones.clear();
    selectedBoneIndex = -1;
    isRotatingBone = false;
    manualBoneRotations.clear();
    boneRotationsBeforeEdit.clear();

    if (pcmData.size() >= 16) {
        BinaryReader br(pcmData);
        br.Seek(8);
        uint32_t numEntries = br.Read<uint32_t>();
        uint32_t entryTableOfs = br.Read<uint32_t>();
        if (numEntries <= 1000 && entryTableOfs < pcmData.size()) {
            uint32_t boneCount = 0, bonesOffset = 0;
            br.Seek(entryTableOfs);
            for (uint32_t i = 0; i < numEntries; i++) {
                uint16_t sz = br.Read<uint16_t>(); uint16_t tag = br.Read<uint16_t>();
                uint32_t dataOfs = br.Read<uint32_t>(); br.Skip(4);
                if (tag == 512 && dataOfs + 24 <= pcmData.size()) {
                    br.Seek(dataOfs + 16);
                    boneCount = br.Read<uint32_t>();
                    bonesOffset = br.Read<uint32_t>();
                    break;
                }
            }
            if (boneCount > 0 && boneCount <= 200 && bonesOffset + boneCount * 64 <= pcmData.size()) {
                skeletonBones.resize(boneCount);
                for (uint32_t i = 0; i < boneCount; i++) {
                    memcpy(skeletonBones[i].bindMatrix, &pcmData[bonesOffset + i * 64], 64);
                    skeletonBones[i].position[0] = skeletonBones[i].bindMatrix[12];
                    skeletonBones[i].position[1] = skeletonBones[i].bindMatrix[13];
                    skeletonBones[i].position[2] = skeletonBones[i].bindMatrix[14];
                    InvertMatrix(skeletonBones[i].bindMatrix, skeletonBones[i].invBindMatrix);
                }
            }
        }
    }

    ComputeNALBonePositions();
    if (nalBonePositions.empty()) {
        return;
    }

    std::vector<int> sortedIndices;
    for (auto& [idx, pos] : nalBonePositions) sortedIndices.push_back(idx);
    std::sort(sortedIndices.begin(), sortedIndices.end());
    nalBoneVboOrder = sortedIndices;

    std::map<int, int> nalToVbo;
    for (int i = 0; i < (int)sortedIndices.size(); i++)
        nalToVbo[sortedIndices[i]] = i;

    skeletonBoneCount = (int)sortedIndices.size();

    struct BoneVert { float x, y, z, r, g, b; };
    std::vector<BoneVert> pointVerts;
    std::vector<BoneVert> lineVerts;

    for (int nalIdx : sortedIndices) {
        auto& p = nalBonePositions[nalIdx];
        pointVerts.push_back({p[0], p[1], p[2], 1.0f, 1.0f, 0.0f});
    }

    if (loadedSkeleton) {
        for (const auto& [childIdx, parentIdx] : loadedSkeleton->parent_map) {
            if (parentIdx < 0) continue;
            if (!nalBonePositions.count(childIdx) || !nalBonePositions.count(parentIdx)) continue;
            auto& cp = nalBonePositions[childIdx];
            auto& pp = nalBonePositions[parentIdx];
            lineVerts.push_back({pp[0], pp[1], pp[2], 0.0f, 0.9f, 1.0f});
            lineVerts.push_back({cp[0], cp[1], cp[2], 0.0f, 0.9f, 1.0f});
        }
    }

    skeletonLineVertCount = (int)lineVerts.size();

    std::vector<BoneVert> allVerts;
    allVerts.insert(allVerts.end(), pointVerts.begin(), pointVerts.end());
    allVerts.insert(allVerts.end(), lineVerts.begin(), lineVerts.end());

    glGenVertexArrays(1, &skeletonVao);
    glGenBuffers(1, &skeletonVbo);
    glBindVertexArray(skeletonVao);
    glBindBuffer(GL_ARRAY_BUFFER, skeletonVbo);
    glBufferData(GL_ARRAY_BUFFER, allVerts.size() * sizeof(BoneVert), allVerts.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BoneVert), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(BoneVert), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glBindVertexArray(0);

}

// Rebuilds the collision wireframe from the current model's bounds.
//
// The engine never stores a per-model collision shape for actors: collision_capsule::compute_dimensions
// derives it from the visual bounding sphere every time --
//     base   = center + Y * (0.125 * R)
//     end    = base   + Y * (0.500 * R)
//     radius = 0.25 * R
// so we reproduce that here rather than inventing our own capsule. The bounding sphere is drawn too,
// because it is what the broad phase actually rejects against (collide_sphere_entity).
void SpiderManTool::BuildCollisionVisual() {
    collisionVertCount = 0;

    float mn[3] = { 1e30f, 1e30f, 1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };
    bool any = false;
    for (const auto& m : previewMeshes) {
        if (m.isHidden || m.positions.empty()) continue;
        for (int i = 0; i < 3; i++) {
            mn[i] = std::min(mn[i], m.bboxMin[i]);
            mx[i] = std::max(mx[i], m.bboxMax[i]);
        }
        any = true;
    }
    if (!any) return;

    const float center[3] = { (mn[0]+mx[0])*0.5f, (mn[1]+mx[1])*0.5f, (mn[2]+mx[2])*0.5f };
    float radius = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float half = (mx[i]-mn[i]) * 0.5f;
        radius += half * half;
    }
    radius = std::sqrt(radius);
    if (radius <= 0.0f) return;

    const float capBaseY = center[1] + 0.125f * radius;
    const float capEndY  = capBaseY  + 0.500f * radius;
    const float capR     = 0.250f * radius;

    struct LineVert { float x, y, z, r, g, b; };
    std::vector<LineVert> verts;

    auto addLine = [&](float ax, float ay, float az, float bx, float by, float bz,
                       float r, float g, float b) {
        verts.push_back({ax, ay, az, r, g, b});
        verts.push_back({bx, by, bz, r, g, b});
    };

    constexpr int SEG = 32;
    constexpr float TWO_PI = 6.2831853f;

    // --- capsule (green) --------------------------------------------------------------------
    // Collision is mutually exclusive in the engine (actor.cpp: is_flagged(2)=capsule,
    // is_flagged(4)=mesh, else no collision at all). A static prop therefore never gets a capsule,
    // so only skinned actors are given one here -- otherwise we would be inventing a shape the
    // engine would not have. Skinned/skeletal is our stand-in for the entity's capsule flag.
    const bool isActor = (skeletonBoneCount > 0);
    if (isActor) {
        auto ring = [&](float cy, float rad, float r, float g, float b) {
            for (int i = 0; i < SEG; i++) {
                const float a0 = TWO_PI * i / SEG, a1 = TWO_PI * (i+1) / SEG;
                addLine(center[0]+std::cos(a0)*rad, cy, center[2]+std::sin(a0)*rad,
                        center[0]+std::cos(a1)*rad, cy, center[2]+std::sin(a1)*rad, r, g, b);
            }
        };
        ring(capBaseY, capR, 0.2f, 1.0f, 0.3f);
        ring(capEndY,  capR, 0.2f, 1.0f, 0.3f);

        for (int i = 0; i < 4; i++) {
            const float a = TWO_PI * i / 4;
            const float px = center[0] + std::cos(a)*capR, pz = center[2] + std::sin(a)*capR;
            addLine(px, capBaseY, pz, px, capEndY, pz, 0.2f, 1.0f, 0.3f);

            // hemisphere arcs closing each end
            for (int s = 0; s < SEG/4; s++) {
                const float t0 = (float)s / (SEG/4) * (3.14159265f*0.5f);
                const float t1 = (float)(s+1) / (SEG/4) * (3.14159265f*0.5f);
                addLine(center[0]+std::cos(a)*capR*std::cos(t0), capEndY + capR*std::sin(t0), center[2]+std::sin(a)*capR*std::cos(t0),
                        center[0]+std::cos(a)*capR*std::cos(t1), capEndY + capR*std::sin(t1), center[2]+std::sin(a)*capR*std::cos(t1),
                        0.2f, 1.0f, 0.3f);
                addLine(center[0]+std::cos(a)*capR*std::cos(t0), capBaseY - capR*std::sin(t0), center[2]+std::sin(a)*capR*std::cos(t0),
                        center[0]+std::cos(a)*capR*std::cos(t1), capBaseY - capR*std::sin(t1), center[2]+std::sin(a)*capR*std::cos(t1),
                        0.2f, 1.0f, 0.3f);
            }
        }
    }

    // --- world/prop collision OBBs (cyan): 12 edges each, corners = center +/- X +/- Y +/- Z ---
    // Collision resources are named after the *pack entry*, not the per-section material name:
    // entry VCL_AMBULANCE -> VCL_AMBULANCE000. RenderMesh::meshName holds material names
    // ("muscle", "solid blue spidey"), so matching against that never hits -- match the loaded
    // entry instead, allowing the trailing NNN index.
    std::string ownerName;
    if (selectedFileIndex >= 0 && selectedFileIndex < (int)entries.size()) {
        ownerName = StrToLower(entries[selectedFileIndex].name);
        const size_t dot = ownerName.find_last_of('.');
        if (dot != std::string::npos) ownerName = ownerName.substr(0, dot);
    }

    std::vector<const CollisionObb*> visibleObbs;
    if (isWorldMode) {
        // World collision is authored per *region*, not per mesh -- BD.PCPACK holds meshes bdc,
        // boardsa, bd_seawall... and a single "bd_beach000" covering the lot. There is nothing to
        // match name-wise, so in world mode show every collision mesh the pack carries.
        for (const auto& group : collisionGroups)
            for (const auto& obb : group.obbs) visibleObbs.push_back(&obb);
    } else if (!ownerName.empty()) {
        for (const auto& group : collisionGroups) {
            if (group.name.size() < ownerName.size()) continue;
            if (group.name.compare(0, ownerName.size(), ownerName) != 0) continue;
            bool restIsDigits = true;
            for (size_t i = ownerName.size(); i < group.name.size(); ++i)
                if (!std::isdigit((unsigned char)group.name[i])) { restIsDigits = false; break; }
            if (!restIsDigits) continue;
            for (const auto& obb : group.obbs) visibleObbs.push_back(&obb);
        }
    }

    // Single-prop packs frequently carry exactly one collision mesh for the one model they hold;
    // fall back to it when the name lookup found nothing (dictionary misses are common).
    if (visibleObbs.empty() && !isWorldMode && collisionGroups.size() == 1) {
        for (const auto& obb : collisionGroups[0].obbs) visibleObbs.push_back(&obb);
    }

    collisionObbsDrawn = (int)visibleObbs.size();

    // LoadAllWorldGeometries places world geometry through a base transform with X negated, so the
    // authored collision has to be mirrored the same way to sit on top of it.
    const float mirrorX = isWorldMode ? -1.0f : 1.0f;

    for (const auto* obbPtr : visibleObbs) {
        const auto& obb = *obbPtr;
        float corner[8][3];
        for (int i = 0; i < 8; i++) {
            const float sx = (i & 1) ? 1.0f : -1.0f;
            const float sy = (i & 2) ? 1.0f : -1.0f;
            const float sz = (i & 4) ? 1.0f : -1.0f;
            for (int k = 0; k < 3; k++) {
                const float axisScale = (k == 0) ? mirrorX : 1.0f;
                corner[i][k] = axisScale *
                    (obb.center[k] + sx*obb.axisX[k] + sy*obb.axisY[k] + sz*obb.axisZ[k]);
            }
        }
        static const int edges[12][2] = {
            {0,1},{2,3},{4,5},{6,7},   // along X
            {0,2},{1,3},{4,6},{5,7},   // along Y
            {0,4},{1,5},{2,6},{3,7}    // along Z
        };
        for (const auto& e : edges) {
            addLine(corner[e[0]][0], corner[e[0]][1], corner[e[0]][2],
                    corner[e[1]][0], corner[e[1]][1], corner[e[1]][2],
                    0.25f, 0.85f, 1.0f);
        }
    }

    // --- bounding sphere (amber): three great circles -----------------------------------------
    // This is collision_geometry::get_bounding_sphere_radius, the broad-phase reject used by
    // collide_sphere_entity. It only exists when the entity actually has a collision geometry,
    // so it is suppressed when there is none to describe.
    if (isActor || collisionObbsDrawn > 0) {
        for (int axis = 0; axis < 3; axis++) {
            for (int i = 0; i < SEG; i++) {
                const float a0 = TWO_PI * i / SEG, a1 = TWO_PI * (i+1) / SEG;
                float p0[3] = {center[0], center[1], center[2]};
                float p1[3] = {center[0], center[1], center[2]};
                const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                p0[u] += std::cos(a0)*radius; p0[v] += std::sin(a0)*radius;
                p1[u] += std::cos(a1)*radius; p1[v] += std::sin(a1)*radius;
                addLine(p0[0], p0[1], p0[2], p1[0], p1[1], p1[2], 1.0f, 0.7f, 0.15f);
            }
        }
    }

    if (verts.empty()) return;

    if (!collisionVao) glGenVertexArrays(1, &collisionVao);
    if (!collisionVbo) glGenBuffers(1, &collisionVbo);
    glBindVertexArray(collisionVao);
    glBindBuffer(GL_ARRAY_BUFFER, collisionVbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(LineVert), verts.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVert), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(LineVert), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    collisionVertCount = (int)verts.size();
}

void SpiderManTool::RenderCollisionOverlay() {
    if (!collisionVao || collisionVertCount == 0 || skeletonProgram == 0) return;

    glUseProgram(skeletonProgram);

    float fov = 1.0f;
    float aspect = 3840.0f / 2160.0f;
    float znear = 0.1f, zfar = 20000.0f;
    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float target[3] = { camPos[0]+camFront[0], camPos[1]+camFront[1], camPos[2]+camFront[2] };
    LookAt(camPos, target, camUp, view);

    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "view"), 1, GL_FALSE, view);

    glBindVertexArray(collisionVao);
    glEnable(GL_DEPTH_TEST);
    glLineWidth(1.0f);
    glDrawArrays(GL_LINES, 0, collisionVertCount);
    glBindVertexArray(0);
}

void SpiderManTool::RenderSkeletonOverlay() {
    if (!skeletonVao || skeletonBoneCount == 0 || skeletonProgram == 0) return;

    glUseProgram(skeletonProgram);

    float fov = 1.0f;
    float aspect = 3840.0f / 2160.0f;
    float znear = 0.1f, zfar = 20000.0f;
    float proj[16] = {0};
    float tanHalfFov = tan(fov / 2.0f);
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float target[3] = { camPos[0]+camFront[0], camPos[1]+camFront[1], camPos[2]+camFront[2] };
    LookAt(camPos, target, camUp, view);

    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "projection"), 1, GL_FALSE, proj);
    glUniformMatrix4fv(glGetUniformLocation(skeletonProgram, "view"), 1, GL_FALSE, view);

    glBindVertexArray(skeletonVao);
    glDisable(GL_DEPTH_TEST);

    if (skeletonLineVertCount > 0) {
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, skeletonBoneCount, skeletonLineVertCount);
    }

    glPointSize(6.0f);
    glDrawArrays(GL_POINTS, 0, skeletonBoneCount);

    if (selectedBoneIndex >= 0 && selectedBoneIndex < skeletonBoneCount) {
        struct BoneVert { float x, y, z, r, g, b; };

        glBindBuffer(GL_ARRAY_BUFFER, skeletonVbo);
        BoneVert sel;

        glGetBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
        sel.r = 1.0f; sel.g = 0.2f; sel.b = 0.2f;
        glBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
        glPointSize(12.0f);
        glDrawArrays(GL_POINTS, selectedBoneIndex, 1);
        sel.r = 1.0f; sel.g = 1.0f; sel.b = 0.0f;
        glBufferSubData(GL_ARRAY_BUFFER, selectedBoneIndex * sizeof(BoneVert), sizeof(BoneVert), &sel);
    }

    glEnable(GL_DEPTH_TEST);
    glPointSize(1.0f);
    glLineWidth(1.0f);
    glBindVertexArray(0);
    glUseProgram(modelProgram);
}

int SpiderManTool::PickBoneAtScreenPos(float screenX, float screenY, float vpW, float vpH) {
    if (skeletonBoneCount == 0 || nalBoneVboOrder.empty()) return -1;

    float fov = 1.0f;
    float aspect = 3840.0f / 2160.0f;
    float znear = 0.1f;
    float zfar = 20000.0f;
    float tanHalfFov = tan(fov / 2.0f);

    float proj[16] = {0};
    proj[0] = 1.0f / (aspect * tanHalfFov);
    proj[5] = 1.0f / tanHalfFov;
    proj[10] = -(zfar + znear) / (zfar - znear);
    proj[11] = -1.0f;
    proj[14] = -(2.0f * zfar * znear) / (zfar - znear);

    float view[16];
    float target[3] = {camPos[0] + camFront[0], camPos[1] + camFront[1], camPos[2] + camFront[2]};
    LookAt(camPos, target, camUp, view);

    float viewProj[16];
    Mat4Multiply(proj, view, viewProj);

    int closest = -1;
    float closestScore = 1e30f;
    const float pickRadiusPx = 16.0f;

    for (int vboIdx = 0; vboIdx < (int)nalBoneVboOrder.size(); vboIdx++) {
        int nalIdx = nalBoneVboOrder[vboIdx];
        if (!nalBonePositions.count(nalIdx)) continue;
        auto& pos = nalBonePositions[nalIdx];

        float world[4] = {pos[0], pos[1], pos[2], 1.0f};
        float clip[4];
        Mat4TransformVec4(viewProj, world, clip);
        if (clip[3] <= 0.0001f) continue;

        float ndcX = clip[0] / clip[3];
        float ndcY = clip[1] / clip[3];
        float ndcZ = clip[2] / clip[3];
        if (ndcX < -1.2f || ndcX > 1.2f || ndcY < -1.2f || ndcY > 1.2f || ndcZ < -1.0f || ndcZ > 1.0f) {
            continue;
        }

        float px = (ndcX * 0.5f + 0.5f) * vpW;
        float py = (0.5f - ndcY * 0.5f) * vpH;
        float dx = px - screenX;
        float dy = py - screenY;
        float distPx = sqrtf(dx * dx + dy * dy);
        if (distPx > pickRadiusPx) continue;

        float depthBias = clip[3] * 0.001f;
        float score = distPx + depthBias;
        if (score < closestScore) {
            closestScore = score;
            closest = vboIdx;
        }
    }
    return closest;
}

void SpiderManTool::ApplyBoneRotation(int boneIdx, float angle, int axis) {
    if (boneIdx < 0 || boneIdx >= skeletonBoneCount) return;
    int nalIdx = (boneIdx >= 0 && boneIdx < (int)nalBoneVboOrder.size()) ? nalBoneVboOrder[boneIdx] : boneIdx;
    if (nalIdx < 0 || nalIdx >= MAX_GLOBAL_BONES) return;
    axis = std::max(0, std::min(axis, 2));
    auto& angles = manualBoneRotations[nalIdx];
    angles[axis] += angle;
}

void SpiderManTool::ResetBoneRotation() {
    manualBoneRotations.clear();
    boneRotationsBeforeEdit.clear();
    boneRotationAngle = 0.0f;
}

void SpiderManTool::CloseDdsPreview() {
    if (ddsTextureId != 0) {
        glDeleteTextures(1, &ddsTextureId);
        ddsTextureId = 0;
    }
    showDdsPopup = false;
}

void SpiderManTool::StoreActivePreviewTab() {
    if (activePreviewTab < 0 || activePreviewTab >= (int)previewTabs.size()) return;
    auto& tab = previewTabs[activePreviewTab];
    if (tab.hasCachedState) return;

    tab.modelLoaded = isModelLoaded;
    tab.modelPreview = isModelPreview;
    tab.worldMode = isWorldMode;
    tab.meshes = std::move(previewMeshes);
    tab.proceduralTentacleMesh = std::move(proceduralTentacleMesh);
    proceduralTentacleMesh = {};

    tab.selectedMeshIndex = selectedMeshIndex;
    tab.selectedMeshInstanceIndex = selectedMeshInstanceIndex;
    tab.selectedMeshPcmData = std::move(selectedMeshPcmData);
    tab.showWorldMeshDetails = showWorldMeshDetails;

    tab.showSkeleton = showSkeleton;
    tab.selectedBoneIndex = selectedBoneIndex;
    tab.isRotatingBone = isRotatingBone;
    tab.boneRotationAngle = boneRotationAngle;
    tab.boneRotationAxis = boneRotationAxis;
    tab.manualBoneRotations = std::move(manualBoneRotations);
    tab.boneRotationsBeforeEdit = std::move(boneRotationsBeforeEdit);
    tab.skeletonVao = skeletonVao;
    tab.skeletonVbo = skeletonVbo;
    tab.skeletonBoneCount = skeletonBoneCount;
    tab.skeletonLineVertCount = skeletonLineVertCount;
    tab.skeletonBones = std::move(skeletonBones);
    tab.nalBonePositions = std::move(nalBonePositions);
    tab.nalBoneVboOrder = std::move(nalBoneVboOrder);
    tab.nalMaxBoneIndex = nalMaxBoneIndex;

    std::copy(modelCenter, modelCenter + 3, tab.modelCenter.begin());
    std::copy(modelHeadTarget, modelHeadTarget + 3, tab.modelHeadTarget.begin());
    tab.modelRadius = modelRadius;
    tab.orbitDistance = orbitDistance;
    std::copy(camPos, camPos + 3, tab.camPos.begin());
    std::copy(camFront, camFront + 3, tab.camFront.begin());
    std::copy(camUp, camUp + 3, tab.camUp.begin());
    tab.camYaw = camYaw;
    tab.camPitch = camPitch;
    tab.camSpeed = camSpeed;

    tab.loadedSkeleton = std::move(loadedSkeleton);
    tab.loadedAnimFile = std::move(loadedAnimFile);
    tab.loadedMorphFile = std::move(loadedMorphFile);
    tab.morphTargetWeights = std::move(morphTargetWeights);
    tab.loadedVisemeStreams = std::move(loadedVisemeStreams);
    tab.selectedVisemeIndex = selectedVisemeIndex;
    tab.selectedAnimIndex = selectedAnimIndex;
    tab.currentAnimFrame = currentAnimFrame;
    tab.animFrameFraction = animFrameFraction;
    tab.isAnimPlaying = isAnimPlaying;
    tab.animPlaybackTime = animPlaybackTime;
    tab.loadedSkeletonName = std::move(loadedSkeletonName);
    tab.loadedAnimName = std::move(loadedAnimName);
    tab.skeletonCandidates = std::move(skeletonCandidates);
    tab.activeSkeletonCandidate = activeSkeletonCandidate;
    tab.activeBoneMapping = std::move(activeBoneMapping);
    tab.activeBoneMappingHash = activeBoneMappingHash;
    tab.hasCachedState = true;

    isModelLoaded = false;
    isModelPreview = false;
    isWorldMode = false;
    previewMeshes.clear();
    proceduralTentacleMesh = {};
    selectedMeshIndex = -1;
    selectedMeshInstanceIndex = -1;
    selectedMeshPcmData.clear();
    showWorldMeshDetails = false;
    showSkeleton = false;
    collisionVertCount = 0;   // bounds changed; overlay rebuilds on next draw
    selectedBoneIndex = -1;
    isRotatingBone = false;
    boneRotationAngle = 0.0f;
    manualBoneRotations.clear();
    boneRotationsBeforeEdit.clear();
    skeletonVao = 0;
    skeletonVbo = 0;
    skeletonBoneCount = 0;
    skeletonLineVertCount = 0;
    skeletonBones.clear();
    nalBonePositions.clear();
    nalBoneVboOrder.clear();
    nalMaxBoneIndex = -1;
    loadedSkeleton.reset();
    loadedAnimFile.reset();
    loadedMorphFile = {};
    morphTargetWeights.clear();
    loadedVisemeStreams.clear();
    selectedVisemeIndex = -1;
    loadedSkeletonName.clear();
    loadedAnimName.clear();
    skeletonCandidates.clear();
    activeSkeletonCandidate = -1;
    activeBoneMapping = {};
    activeBoneMappingHash = 0;
    selectedAnimIndex = -1;
    currentAnimFrame = 0;
    animFrameFraction = 0.0f;
    isAnimPlaying = false;
    animPlaybackTime = 0.0f;
}

void SpiderManTool::RestorePreviewTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)previewTabs.size()) return;
    auto& tab = previewTabs[tabIndex];
    if (!tab.hasCachedState) return;

    isModelLoaded = tab.modelLoaded;
    isModelPreview = tab.modelPreview;
    isWorldMode = tab.worldMode;
    previewMeshes = std::move(tab.meshes);
    proceduralTentacleMesh = std::move(tab.proceduralTentacleMesh);
    tab.proceduralTentacleMesh = {};

    selectedMeshIndex = tab.selectedMeshIndex;
    selectedMeshInstanceIndex = tab.selectedMeshInstanceIndex;
    selectedMeshPcmData = std::move(tab.selectedMeshPcmData);
    showWorldMeshDetails = tab.showWorldMeshDetails;

    showSkeleton = tab.showSkeleton;
    selectedBoneIndex = tab.selectedBoneIndex;
    isRotatingBone = tab.isRotatingBone;
    boneRotationAngle = tab.boneRotationAngle;
    boneRotationAxis = tab.boneRotationAxis;
    manualBoneRotations = std::move(tab.manualBoneRotations);
    boneRotationsBeforeEdit = std::move(tab.boneRotationsBeforeEdit);
    skeletonVao = tab.skeletonVao;
    skeletonVbo = tab.skeletonVbo;
    skeletonBoneCount = tab.skeletonBoneCount;
    skeletonLineVertCount = tab.skeletonLineVertCount;
    skeletonBones = std::move(tab.skeletonBones);
    nalBonePositions = std::move(tab.nalBonePositions);
    nalBoneVboOrder = std::move(tab.nalBoneVboOrder);
    nalMaxBoneIndex = tab.nalMaxBoneIndex;

    std::copy(tab.modelCenter.begin(), tab.modelCenter.end(), modelCenter);
    std::copy(tab.modelHeadTarget.begin(), tab.modelHeadTarget.end(), modelHeadTarget);
    modelRadius = tab.modelRadius;
    orbitDistance = tab.orbitDistance;
    std::copy(tab.camPos.begin(), tab.camPos.end(), camPos);
    std::copy(tab.camFront.begin(), tab.camFront.end(), camFront);
    std::copy(tab.camUp.begin(), tab.camUp.end(), camUp);
    camYaw = tab.camYaw;
    camPitch = tab.camPitch;
    camSpeed = tab.camSpeed;

    loadedSkeleton = std::move(tab.loadedSkeleton);
    loadedAnimFile = std::move(tab.loadedAnimFile);
    loadedMorphFile = std::move(tab.loadedMorphFile);
    morphTargetWeights = std::move(tab.morphTargetWeights);
    loadedVisemeStreams = std::move(tab.loadedVisemeStreams);
    selectedVisemeIndex = tab.selectedVisemeIndex;
    selectedAnimIndex = tab.selectedAnimIndex;
    currentAnimFrame = tab.currentAnimFrame;
    animFrameFraction = tab.animFrameFraction;
    isAnimPlaying = tab.isAnimPlaying;
    animPlaybackTime = tab.animPlaybackTime;
    loadedSkeletonName = std::move(tab.loadedSkeletonName);
    loadedAnimName = std::move(tab.loadedAnimName);
    skeletonCandidates = std::move(tab.skeletonCandidates);
    activeSkeletonCandidate = tab.activeSkeletonCandidate;
    activeBoneMapping = std::move(tab.activeBoneMapping);
    activeBoneMappingHash = tab.activeBoneMappingHash;
    if (loadedMorphFile.valid && morphTargetWeights.size() > 1) focusMorphsTab = true;
    tab.hasCachedState = false;
}

void SpiderManTool::DestroyPreviewTabResources(PreviewTabState& tab) {
    for (auto& mesh : tab.meshes) {
        if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
        if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
        if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
        if (mesh.instanceVbo) glDeleteBuffers(1, &mesh.instanceVbo);
    }
    tab.meshes.clear();
    if (tab.proceduralTentacleMesh.vao)
        glDeleteVertexArrays(1, &tab.proceduralTentacleMesh.vao);
    if (tab.proceduralTentacleMesh.vbo)
        glDeleteBuffers(1, &tab.proceduralTentacleMesh.vbo);
    if (tab.proceduralTentacleMesh.ebo)
        glDeleteBuffers(1, &tab.proceduralTentacleMesh.ebo);
    if (tab.proceduralTentacleMesh.instanceVbo)
        glDeleteBuffers(1, &tab.proceduralTentacleMesh.instanceVbo);
    tab.proceduralTentacleMesh = {};
    if (tab.skeletonVao) glDeleteVertexArrays(1, &tab.skeletonVao);
    if (tab.skeletonVbo) glDeleteBuffers(1, &tab.skeletonVbo);
    tab.skeletonVao = 0;
    tab.skeletonVbo = 0;
}

void SpiderManTool::ActivatePreviewTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)previewTabs.size()) return;
    if (tabIndex == activePreviewTab) return;
    StoreActivePreviewTab();
    activePreviewTab = tabIndex;
    RestorePreviewTab(tabIndex);
}

void SpiderManTool::ClosePreviewTab(int tabIndex) {
    if (tabIndex < 0 || tabIndex >= (int)previewTabs.size()) return;

    const bool wasActive = tabIndex == activePreviewTab;
    if (wasActive) StoreActivePreviewTab();
    DestroyPreviewTabResources(previewTabs[tabIndex]);
    previewTabs.erase(previewTabs.begin() + tabIndex);

    if (previewTabs.empty()) {
        activePreviewTab = -1;
        isModelLoaded = false;
        isModelPreview = false;
        isWorldMode = false;
        return;
    }

    if (!wasActive) {
        if (activePreviewTab > tabIndex) --activePreviewTab;
        return;
    }

    activePreviewTab = std::min(tabIndex, (int)previewTabs.size() - 1);
    RestorePreviewTab(activePreviewTab);
    previewTabs[activePreviewTab].requestSelect = true;
}

void SpiderManTool::OpenPcmPreviewTab(int entryIndex) {
    if (entryIndex < 0 || entryIndex >= (int)entries.size()) return;
    const FileEntry entry = entries[entryIndex];
    if (!entry.isPcm || pcPackData.empty()) return;

    const std::string key = loadedPCPackPath + "#" +
        std::to_string(entry.offset) + "#" + std::to_string(entry.size);
    for (int i = 0; i < (int)previewTabs.size(); ++i) {
        if (previewTabs[i].key != key) continue;
        ActivatePreviewTab(i);
        previewTabs[i].requestSelect = true;
        return;
    }

    const bool replacingActiveTab = activePreviewTab >= 0;
    if (replacingActiveTab) StoreActivePreviewTab();

    const bool packHasNalResources = !currentDir.skeletons.empty() ||
                                     !currentDir.animFiles.empty();
    if (replacingActiveTab || (packHasNalResources &&
        skeletonCandidates.empty() && !loadedAnimFile)) {
        LoadSkeletonForCurrentPack();
        LoadAnimationForCurrentPack();
    }

    PreviewTabState tab;
    tab.id = nextPreviewTabId++;
    tab.key = key;
    tab.label = entry.name;
    tab.packPath = loadedPCPackPath;
    tab.resourceHash = entry.hash;
    tab.resourceOffset = entry.offset;
    tab.requestSelect = true;
    previewTabs.push_back(std::move(tab));
    activePreviewTab = (int)previewTabs.size() - 1;

    selectedFileIndex = entryIndex;
    LoadPreview(entryIndex);

}

void SpiderManTool::OpenGlobalSearchPcmTab(int resultIndex) {
    if (resultIndex < 0 || resultIndex >= (int)globalSearchResults.size()) return;
    if (!globalSearchResults[resultIndex].isPcm) return;
    SelectGlobalSearchResult(resultIndex);
    if (selectedFileIndex >= 0) OpenPcmPreviewTab(selectedFileIndex);
}

void SpiderManTool::LoadPreview(int index) {
    if (index < 0 || index >= (int)entries.size()) return;
    if (pcPackData.empty()) return;
    const auto& e = entries[index];

    if (e.isPcm) {
        InitModelPreview();
        isModelPreview = true;

        fs::path p(loadedPCPackPath);
        std::string packStem = StrToLower(p.stem().string());
        std::string fileStem = StrToLower(fs::path(e.name).stem().string());

        if (IsWorldPack(packStem) && (fileStem == packStem + "c" || fileStem == packStem)) {
            isWorldMode = true;
            selectedMeshIndex = -1;
            selectedMeshInstanceIndex = -1;
            selectedMeshPcmData.clear();
            showWorldMeshDetails = false;

            camPos[0] = 0.0f; camPos[1] = 200.0f; camPos[2] = -600.0f;
            camFront[0] = 0.0f; camFront[1] = -0.3f; camFront[2] = -1.0f;
            camUp[0] = 0.0f; camUp[1] = 1.0f; camUp[2] = 0.0f;
            camYaw = -90.0f;
            camPitch = -15.0f;
            camSpeed = 500.0f;

            float transformMatrix[16] = {0};
            transformMatrix[0] = -1.0f;
            transformMatrix[5] = 1.0f;
            transformMatrix[10] = 1.0f;
            transformMatrix[15] = 1.0f;

            std::vector<uint8_t> pcmData(pcPackData.begin() + e.offset, pcPackData.begin() + e.offset + e.size);
            AddMeshFromDataWithTransform(pcmData, e.name, nullptr, loadedPCPackPath, e.offset, transformMatrix);

            LoadPackEntities(loadedPCPackPath, transformMatrix);

            LoadSkybox();
        } else {
            LoadModelToGL(index);
        }

        isModelLoaded = true;
        return;
    }

    if (!e.isDds) return;

    CloseDdsPreview();
    isModelPreview = false;

    if (e.offset + e.size > pcPackData.size()) return;
    if (e.size < sizeof(DDS_HEADER) + 4) return;
    const uint8_t* rawData = &pcPackData[e.offset];
    if (*(uint32_t*)rawData != 0x20534444) return;

    const DDS_HEADER* header = (const DDS_HEADER*)(rawData + 4);
    ddsWidth = header->dwWidth;
    ddsHeight = header->dwHeight;

    std::vector<uint8_t> ddsData(rawData, rawData + e.size);
    ddsTextureId = LoadTextureFromData(ddsData);
    if (ddsTextureId == 0) return;
    showDdsPopup = true;
}

bool SpiderManTool::RayIntersectAABB(const float rayOrigin[3], const float rayDir[3],
                                      const float bboxMin[3], const float bboxMax[3], float& tMin) {
    float tmax = 1e30f;
    tMin = -1e30f;

    for (int i = 0; i < 3; i++) {
        if (fabs(rayDir[i]) < 1e-8f) {
            if (rayOrigin[i] < bboxMin[i] || rayOrigin[i] > bboxMax[i]) {
                return false;
            }
        } else {
            float ood = 1.0f / rayDir[i];
            float t1 = (bboxMin[i] - rayOrigin[i]) * ood;
            float t2 = (bboxMax[i] - rayOrigin[i]) * ood;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tMin) tMin = t1;
            if (t2 < tmax) tmax = t2;
            if (tMin > tmax) return false;
        }
    }

    return tMin >= 0;
}

static bool RayIntersectTriangle(const float rayOrigin[3], const float rayDir[3],
                                  const float v0[3], const float v1[3], const float v2[3],
                                  float& t) {
    const float EPSILON = 1e-7f;

    float edge1[3] = { v1[0] - v0[0], v1[1] - v0[1], v1[2] - v0[2] };
    float edge2[3] = { v2[0] - v0[0], v2[1] - v0[1], v2[2] - v0[2] };

    float h[3];
    Cross(rayDir, edge2, h);
    float a = Dot(edge1, h);

    if (fabs(a) < EPSILON) return false;

    float f = 1.0f / a;
    float s[3] = { rayOrigin[0] - v0[0], rayOrigin[1] - v0[1], rayOrigin[2] - v0[2] };
    float u = f * Dot(s, h);

    if (u < 0.0f || u > 1.0f) return false;

    float q[3];
    Cross(s, edge1, q);
    float v = f * Dot(rayDir, q);

    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * Dot(edge2, q);
    return t > EPSILON;
}

int SpiderManTool::PickMeshAtScreenPos(float screenX, float screenY, float vpWidth, float vpHeight,
                                       int* instanceIndexOut) {
    if (instanceIndexOut) *instanceIndexOut = -1;
    float ndcX = (2.0f * screenX / vpWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / vpHeight);

    const float fbAspect = 3840.0f / 2160.0f;
    float fov = 1.0f;
    float tanHalfFov = tan(fov / 2.0f);

    float right[3];
    Cross(camFront, camUp, right);
    Normalize(right);

    float up[3];
    Cross(right, camFront, up);
    Normalize(up);

    float rayDir[3] = {
        camFront[0] + right[0] * ndcX * tanHalfFov * fbAspect + up[0] * ndcY * tanHalfFov,
        camFront[1] + right[1] * ndcX * tanHalfFov * fbAspect + up[1] * ndcY * tanHalfFov,
        camFront[2] + right[2] * ndcX * tanHalfFov * fbAspect + up[2] * ndcY * tanHalfFov
    };
    Normalize(rayDir);

    float rayOrigin[3] = { camPos[0], camPos[1], camPos[2] };

    int closestMesh = -1;
    int closestInstance = -1;
    float closestT = 1e30f;

    for (int i = 0; i < (int)previewMeshes.size(); i++) {
        const auto& m = previewMeshes[i];

        if (m.skipPicking) continue;

        if (m.isFakeShadow || m.isColorVolume || m.isShadowVolume ||
            m.isDebugTransparent) continue;

        if (!m.instances.empty()) {
            for (int instanceIndex = 0; instanceIndex < (int)m.instances.size(); ++instanceIndex) {
                const auto& instance = m.instances[instanceIndex];
                if (instance.isHidden) continue;

                float bboxT;
                if (!RayIntersectAABB(rayOrigin, rayDir, instance.bboxMin, instance.bboxMax, bboxT)) continue;
                if (bboxT >= closestT || m.positions.empty() || m.indices.empty()) continue;

                auto testTriangle = [&](uint16_t i0, uint16_t i1, uint16_t i2) {
                    if (i0 * 3 + 2 >= m.positions.size() ||
                        i1 * 3 + 2 >= m.positions.size() ||
                        i2 * 3 + 2 >= m.positions.size()) return;
                    float local0[3] = {m.positions[i0*3], m.positions[i0*3+1], m.positions[i0*3+2]};
                    float local1[3] = {m.positions[i1*3], m.positions[i1*3+1], m.positions[i1*3+2]};
                    float local2[3] = {m.positions[i2*3], m.positions[i2*3+1], m.positions[i2*3+2]};
                    float v0[3], v1[3], v2[3];
                    Mat4TransformPoint(instance.transform.data(), local0, v0);
                    Mat4TransformPoint(instance.transform.data(), local1, v1);
                    Mat4TransformPoint(instance.transform.data(), local2, v2);
                    float t;
                    if (RayIntersectTriangle(rayOrigin, rayDir, v0, v1, v2, t) && t < closestT) {
                        closestT = t;
                        closestMesh = i;
                        closestInstance = instanceIndex;
                    }
                };

                if (m.mode == GL_TRIANGLES) {
                    for (size_t j = 0; j + 2 < m.indices.size(); j += 3) {
                        testTriangle(m.indices[j], m.indices[j + 1], m.indices[j + 2]);
                    }
                } else {
                    for (size_t j = 0; j + 2 < m.indices.size(); ++j) {
                        uint16_t i0 = m.indices[j];
                        uint16_t i1 = m.indices[j + 1];
                        uint16_t i2 = m.indices[j + 2];
                        if (i0 == i1 || i1 == i2 || i0 == i2) continue;
                        if ((j & 1) == 0) testTriangle(i0, i1, i2);
                        else testTriangle(i0, i2, i1);
                    }
                }
            }
            continue;
        }

        float bboxT;
        if (!RayIntersectAABB(rayOrigin, rayDir, m.bboxMin, m.bboxMax, bboxT)) continue;
        if (bboxT >= closestT) continue;

        if (m.positions.empty() || m.indices.empty()) continue;

        if (m.mode == GL_TRIANGLES) {
            for (size_t j = 0; j + 2 < m.indices.size(); j += 3) {
                uint16_t i0 = m.indices[j];
                uint16_t i1 = m.indices[j + 1];
                uint16_t i2 = m.indices[j + 2];

                if (i0 * 3 + 2 >= m.positions.size() ||
                    i1 * 3 + 2 >= m.positions.size() ||
                    i2 * 3 + 2 >= m.positions.size()) continue;

                float v0[3] = { m.positions[i0*3], m.positions[i0*3+1], m.positions[i0*3+2] };
                float v1[3] = { m.positions[i1*3], m.positions[i1*3+1], m.positions[i1*3+2] };
                float v2[3] = { m.positions[i2*3], m.positions[i2*3+1], m.positions[i2*3+2] };

                float t;
                if (RayIntersectTriangle(rayOrigin, rayDir, v0, v1, v2, t)) {
                    if (t < closestT) {
                        closestT = t;
                        closestMesh = i;
                    }
                }
            }
        } else {
            for (size_t j = 0; j + 2 < m.indices.size(); j++) {
                uint16_t i0 = m.indices[j];
                uint16_t i1 = m.indices[j + 1];
                uint16_t i2 = m.indices[j + 2];

                if (i0 == i1 || i1 == i2 || i0 == i2) continue;

                if (i0 * 3 + 2 >= m.positions.size() ||
                    i1 * 3 + 2 >= m.positions.size() ||
                    i2 * 3 + 2 >= m.positions.size()) continue;

                float v0[3] = { m.positions[i0*3], m.positions[i0*3+1], m.positions[i0*3+2] };
                float v1[3] = { m.positions[i1*3], m.positions[i1*3+1], m.positions[i1*3+2] };
                float v2[3] = { m.positions[i2*3], m.positions[i2*3+1], m.positions[i2*3+2] };

                float t;
                if (j % 2 == 0) {
                    if (RayIntersectTriangle(rayOrigin, rayDir, v0, v1, v2, t)) {
                        if (t < closestT) {
                            closestT = t;
                            closestMesh = i;
                        }
                    }
                } else {
                    if (RayIntersectTriangle(rayOrigin, rayDir, v0, v2, v1, t)) {
                        if (t < closestT) {
                            closestT = t;
                            closestMesh = i;
                        }
                    }
                }
            }
        }
    }

    if (instanceIndexOut) *instanceIndexOut = closestInstance;
    return closestMesh;
}

void SpiderManTool::HandleMeshPicking(float viewportX, float viewportY, float viewportWidth, float viewportHeight) {
    if (!isWorldMode || !isModelLoaded) return;

    int pickedInstance = -1;
    int pickedMesh = PickMeshAtScreenPos(viewportX, viewportY, viewportWidth, viewportHeight,
                                         &pickedInstance);

    if (pickedMesh >= 0) {
        selectedMeshIndex = pickedMesh;
        selectedMeshInstanceIndex = pickedInstance;
        LoadSelectedMeshPcmData();
        showWorldMeshDetails = true;

        const auto& m = previewMeshes[pickedMesh];
        const std::string selectedName =
            (pickedInstance >= 0 && pickedInstance < (int)m.instances.size() &&
             !m.instances[pickedInstance].name.empty())
                ? m.instances[pickedInstance].name
                : m.meshName;
        if (!selectedName.empty()) {

        } else {

        }
    }
}

void SpiderManTool::LoadSelectedMeshPcmData() {
    selectedMeshPcmData.clear();

    if (selectedMeshIndex < 0 || selectedMeshIndex >= (int)previewMeshes.size()) return;

    const auto& m = previewMeshes[selectedMeshIndex];
    if (m.sourcePack.empty() || m.sourceSize == 0) return;

    std::ifstream file(m.sourcePack, std::ios::binary);
    if (!file.is_open()) return;

    file.seekg(m.sourceOffset);
    if (!file.good()) return;

    selectedMeshPcmData.resize(m.sourceSize);
    file.read((char*)selectedMeshPcmData.data(), m.sourceSize);
    file.close();
}
