#version 450

#include "include/scene_color.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D aces2Lut;
layout(set = 0, binding = 2) uniform sampler2D selectionMask;
layout(push_constant) uniform OutputPushConstants {
    float manualExposureEv;
    uint outputOperator;
    uint outputTransport;
    float paperWhiteNits;
    float peakNits;
    uint selectionActive;
} push;

const int LUT_SIZE = 128;
const float LUT_MIN_LOG2 = -10.0;
const float LUT_MAX_LOG2 = 16.0;

vec3 lutTexel(ivec3 coordinate) {
    return texelFetch(aces2Lut,
        ivec2(coordinate.x + coordinate.z * LUT_SIZE, coordinate.y), 0).rgb;
}

vec3 sampleAces2Encoded(vec3 sceneAcesCg) {
    vec3 safeValue;
    for (int channel = 0; channel < 3; ++channel) {
        float value = sceneAcesCg[channel];
        safeValue[channel] = isnan(value) || value < 0.0 ? 0.0 :
            (isinf(value) ? 65536.0 : value);
    }
    float logRange = log2(exp2(LUT_MAX_LOG2) + exp2(LUT_MIN_LOG2)) -
        LUT_MIN_LOG2;
    vec3 shaper = clamp((log2(safeValue + exp2(LUT_MIN_LOG2)) -
        LUT_MIN_LOG2) / logRange, 0.0, 1.0) *
        float(LUT_SIZE - 1);
    ivec3 lower = ivec3(floor(shaper));
    ivec3 upper = min(lower + ivec3(1), ivec3(LUT_SIZE - 1));
    vec3 f = shaper - vec3(lower);
    vec3 result;
    if (f.r >= f.g) {
        if (f.g >= f.b) {
            result = (1.0 - f.r) * lutTexel(lower);
            result += (f.r - f.g) * lutTexel(ivec3(upper.r, lower.g, lower.b));
            result += (f.g - f.b) * lutTexel(ivec3(upper.r, upper.g, lower.b));
            result += f.b * lutTexel(upper);
        } else if (f.r >= f.b) {
            result = (1.0 - f.r) * lutTexel(lower);
            result += (f.r - f.b) * lutTexel(ivec3(upper.r, lower.g, lower.b));
            result += (f.b - f.g) * lutTexel(ivec3(upper.r, lower.g, upper.b));
            result += f.g * lutTexel(upper);
        } else {
            result = (1.0 - f.b) * lutTexel(lower);
            result += (f.b - f.r) * lutTexel(ivec3(lower.r, lower.g, upper.b));
            result += (f.r - f.g) * lutTexel(ivec3(upper.r, lower.g, upper.b));
            result += f.g * lutTexel(upper);
        }
    } else if (f.b >= f.g) {
        result = (1.0 - f.b) * lutTexel(lower);
        result += (f.b - f.g) * lutTexel(ivec3(lower.r, lower.g, upper.b));
        result += (f.g - f.r) * lutTexel(ivec3(lower.r, upper.g, upper.b));
        result += f.r * lutTexel(upper);
    } else if (f.b >= f.r) {
        result = (1.0 - f.g) * lutTexel(lower);
        result += (f.g - f.b) * lutTexel(ivec3(lower.r, upper.g, lower.b));
        result += (f.b - f.r) * lutTexel(ivec3(lower.r, upper.g, upper.b));
        result += f.r * lutTexel(upper);
    } else {
        result = (1.0 - f.g) * lutTexel(lower);
        result += (f.g - f.r) * lutTexel(ivec3(lower.r, upper.g, lower.b));
        result += (f.r - f.b) * lutTexel(ivec3(upper.r, upper.g, lower.b));
        result += f.b * lutTexel(upper);
    }
    return clamp(result, 0.0, 1.0);
}

vec3 decodeSrgb(vec3 encoded) {
    bvec3 low = lessThanEqual(encoded, vec3(0.04045));
    vec3 linearLow = encoded / 12.92;
    vec3 linearHigh = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(linearHigh, linearLow, low);
}

vec3 decodeSt2084ToNits(vec3 encoded) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    vec3 p = pow(clamp(encoded, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p,
        vec3(1e-7)), vec3(1.0 / m1));
}

vec3 linearRec2020ToLinearSrgb(vec3 value) {
    return mat3(
         1.660491, -0.124550, -0.018151,
        -0.587641,  1.132900, -0.100579,
        -0.072850, -0.008349,  1.118730) * value;
}

float selectionOutline() {
    ivec2 center = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(selectionMask, 0);
    float centerMask = texelFetch(selectionMask,
        clamp(center, ivec2(0), size - ivec2(1)), 0).a < 0.0 ? 1.0 : 0.0;
    const ivec2 offsets[12] = ivec2[](
        ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
        ivec2(-3, 0), ivec2(3, 0), ivec2(0, -3), ivec2(0, 3),
        ivec2(-2, -2), ivec2(2, -2), ivec2(-2, 2), ivec2(2, 2));
    float difference = 0.0;
    for (int index = 0; index < offsets.length(); ++index) {
        float neighbor = texelFetch(selectionMask,
            clamp(center + offsets[index], ivec2(0), size - ivec2(1)),
            0).a < 0.0 ? 1.0 : 0.0;
        difference = max(difference, abs(centerMask - neighbor));
    }
    return difference;
}

vec3 applySelectionOutline(vec3 outputValue) {
    if (push.selectionActive == 0u) return outputValue;
    if (selectionOutline() == 0.0) return outputValue;
    vec3 cyanLinearSrgb = vec3(0.0, 1.0, 1.0);
    if (push.outputTransport == 2u) {
        // HDR10's intermediate UI-composition target is linear Rec.2020 at
        // paper-white-relative scale.
        return mat3(
            0.627404, 0.069097, 0.016391,
            0.329283, 0.919540, 0.088013,
            0.043313, 0.011362, 0.895595) * cyanLinearSrgb;
    }
    return cyanLinearSrgb;
}

void main() {
    vec3 sceneAcesCg = texture(sceneColor, fragTexCoord).rgb *
        exp2(push.manualExposureEv);
    if (push.outputOperator == 0u) {
        vec3 encoded = sampleAces2Encoded(sceneAcesCg);
        if (push.outputTransport == 0u) {
            outColor = vec4(applySelectionOutline(decodeSrgb(encoded)), 1.0);
            return;
        }
        vec3 rec2020Nits = decodeSt2084ToNits(encoded);
        vec3 targetNits = push.outputTransport == 1u
            ? linearRec2020ToLinearSrgb(rec2020Nits) : rec2020Nits;
        outColor = vec4(applySelectionOutline(
            min(targetNits, vec3(push.peakNits)) /
                push.paperWhiteNits), 1.0);
        return;
    }
    vec3 linearSrgb = max(acesCgToLinearSrgb(sceneAcesCg), vec3(0.0));
    if (push.outputOperator == 2u) {
        outColor = vec4(applySelectionOutline(
            clamp(linearSrgb, 0.0, 1.0)), 1.0);
        return;
    }
    float a = 2.51; float b = 0.03; float c = 2.43;
    float d = 0.59; float e = 0.14;
    vec3 compatibility = clamp(
        (linearSrgb * (a * linearSrgb + b)) /
        (linearSrgb * (c * linearSrgb + d) + e), 0.0, 1.0);
    outColor = vec4(applySelectionOutline(compatibility), 1.0);
}
