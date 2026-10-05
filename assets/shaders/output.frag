#version 450

#include "include/scene_color.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D aces2Lut;
layout(set = 0, binding = 2) uniform sampler2D selectionMask;
layout(set = 0, binding = 3) uniform sampler2D opaqueDepth;
// M9.1: per-pixel motion (current minus previous UV), for its debug view.
layout(set = 0, binding = 4) uniform sampler2D motionVectors;
// M9.5: this frame's adapted exposure (y: the multiplier, compensation
// included). Read only in auto-exposure mode; manual mode uses the push EV.
layout(set = 0, binding = 5, std430) readonly buffer ExposureState {
    vec4 exposureState;
};
// M9.4: the bloom chain's level 0 (half resolution, scene-linear, already
// normalised to the scene's energy). Read only when bloom is active.
layout(set = 0, binding = 6) uniform sampler2D bloomChain;
layout(push_constant) uniform OutputPushConstants {
    mat4 inverseViewProjection;
    vec4 gridPlane;
    vec4 gridAxisU;
    vec4 gridSettings;
    float manualExposureEv;
    float paperWhiteNits;
    float peakNits;
    uint packedModes;
    vec4 bloom;   // M9.4: x intensity, y 1 = additive (threshold set)
} push;

uint outputOperator() { return push.packedModes & 0x3u; }
uint outputTransport() { return (push.packedModes >> 2u) & 0x3u; }
bool selectionActive() { return (push.packedModes & (1u << 4u)) != 0u; }
bool gridActive() { return (push.packedModes & (1u << 5u)) != 0u; }
bool motionVectorView() { return (push.packedModes & (1u << 6u)) != 0u; }
bool autoExposure() { return (push.packedModes & (1u << 7u)) != 0u; }
bool bloomActive() { return (push.packedModes & (1u << 8u)) != 0u; }

// M9.4: four bilinear taps half a chain texel apart form a 3x3 tent on the
// half-resolution level 0. Without a threshold the composite is the
// energy-conserving lerp; with one, the selected energy is added.
vec3 compositeBloom(vec3 scene) {
    vec2 texel = 0.5 / vec2(textureSize(bloomChain, 0));
    vec3 bloom = (textureLod(bloomChain, fragTexCoord + vec2(-texel.x, -texel.y), 0.0).rgb +
        textureLod(bloomChain, fragTexCoord + vec2(texel.x, -texel.y), 0.0).rgb +
        textureLod(bloomChain, fragTexCoord + vec2(-texel.x, texel.y), 0.0).rgb +
        textureLod(bloomChain, fragTexCoord + vec2(texel.x, texel.y), 0.0).rgb) * 0.25;
    float intensity = push.bloom.x;
    // lerp written so that intensity 0 returns the scene exactly (also +Inf).
    return push.bloom.y > 0.5 ? scene + bloom * intensity
        : scene * (1.0 - intensity) + bloom * intensity;
}

// Hue is the screen-space direction, brightness log2 of the pixels moved
// (white at 64 px); black is no motion and magenta no previous position.
vec3 motionVectorColor(vec2 motion) {
    if (any(greaterThanEqual(abs(motion), vec2(3.5)))) return vec3(1.0, 0.0, 1.0);
    vec2 pixels = motion * vec2(textureSize(motionVectors, 0));
    float magnitude = length(pixels);
    if (magnitude < 1.0e-4) return vec3(0.0);
    float hue = atan(pixels.y, pixels.x) / 6.2831853 + 0.5;
    vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0,
        0.0, 1.0);
    float brightness = clamp(log2(1.0 + magnitude) / log2(65.0), 0.0, 1.0);
    return rgb * brightness;
}

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

uint selectionFeedbackAt(ivec2 coordinate) {
    ivec2 size = textureSize(selectionMask, 0);
    float mask = texelFetch(selectionMask,
        clamp(coordinate, ivec2(0), size - ivec2(1)), 0).a;
    // Nonnegative alpha belongs to the regular surface cache, not editor masks.
    return mask < 0.0 ? uint(clamp(-mask, 0.0, 3.0)) : 0u;
}

float selectionOutline() {
    ivec2 center = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(selectionMask, 0);
    float centerMask = float(selectionFeedbackAt(center) & 1u);
    const ivec2 offsets[12] = ivec2[](
        ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
        ivec2(-3, 0), ivec2(3, 0), ivec2(0, -3), ivec2(0, 3),
        ivec2(-2, -2), ivec2(2, -2), ivec2(-2, 2), ivec2(2, 2));
    float difference = 0.0;
    for (int index = 0; index < offsets.length(); ++index) {
        float neighbor = float(selectionFeedbackAt(center + offsets[index]) & 1u);
        difference = max(difference, abs(centerMask - neighbor));
    }
    return difference;
}

vec3 applySelectionOutline(vec3 outputValue) {
    if (!selectionActive()) return outputValue;
    bool hovered = (selectionFeedbackAt(ivec2(gl_FragCoord.xy)) & 2u) != 0u;
    bool outlined = selectionOutline() != 0.0;
    if (!hovered && !outlined) return outputValue;
    vec3 cyanLinearSrgb = vec3(0.0, 1.0, 1.0);
    // Tint is an editor display overlay, never a material or scene-HDR edit.
    vec3 feedbackColor = outlined ? cyanLinearSrgb : vec3(1.0, 0.65, 0.15);
    if (outputTransport() == 2u) {
        // HDR10's intermediate UI-composition target is linear Rec.2020 at
        // paper-white-relative scale.
        feedbackColor = mat3(
            0.627404, 0.069097, 0.016391,
            0.329283, 0.919540, 0.088013,
            0.043313, 0.011362, 0.895595) * feedbackColor;
    }
    return outlined ? feedbackColor : mix(outputValue, feedbackColor, 0.18);
}

vec3 editorColor(vec3 linearSrgb) {
    if (outputTransport() != 2u) return linearSrgb;
    return mat3(
        0.627404, 0.069097, 0.016391,
        0.329283, 0.919540, 0.088013,
        0.043313, 0.011362, 0.895595) * linearSrgb;
}

vec3 axisColor(vec3 axis) {
    vec3 absoluteAxis = abs(axis);
    if (absoluteAxis.x >= absoluteAxis.y &&
        absoluteAxis.x >= absoluteAxis.z) {
        return vec3(1.0, 0.36, 0.36);
    }
    if (absoluteAxis.y >= absoluteAxis.z) {
        return vec3(0.36, 0.86, 0.44);
    }
    return vec3(0.28, 0.57, 1.0);
}

float gridLines(vec2 coordinate, float spacing, float widthPixels) {
    vec2 scaled = coordinate / max(spacing, 1e-7);
    vec2 derivative = max(fwidth(scaled), vec2(1e-6));
    vec2 distanceToLine = abs(fract(scaled - 0.5) - 0.5) /
        derivative;
    float distancePixels = min(distanceToLine.x, distanceToLine.y);
    return 1.0 - smoothstep(0.15 * widthPixels, widthPixels,
        distancePixels);
}

float axisLine(float coordinate) {
    float derivative = max(fwidth(coordinate), 1e-6);
    return 1.0 - smoothstep(0.4 * derivative, 1.8 * derivative,
        abs(coordinate));
}

float interleavedGradientNoise(vec2 pixel) {
    // Stable screen-space dither. This is deliberately sub-LSB and only used
    // while the SDR grid is fading, so it breaks quantization bands without
    // becoming visible grain or introducing temporal shimmer.
    return fract(52.9829189 * fract(dot(pixel,
        vec2(0.06711056, 0.00583715))));
}

vec3 applyViewportGrid(vec3 outputValue) {
    if (!gridActive()) return outputValue;

    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 nearHomogeneous = push.inverseViewProjection *
        vec4(ndc, 0.0, 1.0);
    vec4 farHomogeneous = push.inverseViewProjection *
        vec4(ndc, 1.0, 1.0);
    if (abs(nearHomogeneous.w) < 1e-7 ||
        abs(farHomogeneous.w) < 1e-7) {
        return outputValue;
    }
    vec3 nearWorld = nearHomogeneous.xyz / nearHomogeneous.w;
    vec3 farWorld = farHomogeneous.xyz / farHomogeneous.w;
    vec3 ray = farWorld - nearWorld;
    float rayLength = length(ray);
    if (rayLength < 1e-7) return outputValue;

    vec3 normal = normalize(push.gridPlane.xyz);
    float denominator = dot(normal, ray);
    if (abs(denominator) < 1e-7) return outputValue;
    float rayParameter = -(dot(normal, nearWorld) + push.gridPlane.w) /
        denominator;
    if (rayParameter < 0.0) return outputValue;
    vec3 gridWorld = nearWorld + ray * rayParameter;
    float gridDistance = rayLength * rayParameter;

    vec3 axisU = normalize(push.gridAxisU.xyz);
    vec3 axisV = normalize(cross(normal, axisU));
    vec2 coordinate = vec2(dot(gridWorld, axisU) - push.gridSettings.x,
        dot(gridWorld, axisV) - push.gridSettings.y);
    float baseSpacing = max(push.gridSettings.z, 1e-6);
    float footprint = max(length(vec2(dFdx(coordinate.x),
        dFdy(coordinate.x))), length(vec2(dFdx(coordinate.y),
        dFdy(coordinate.y))));
    float logarithmicLevel = clamp(log(max(footprint * 6.0 /
        baseSpacing, 1e-6)) / log(10.0),
        -3.0, 5.0);
    float level = floor(logarithmicLevel);
    float spacing = baseSpacing * pow(10.0, level);
    float transition = smoothstep(0.0, 1.0, fract(logarithmicLevel));
    float fine = gridLines(coordinate, spacing, 0.90);
    float coarse = gridLines(coordinate, spacing * 10.0, 0.90);
    // At every decade boundary the outgoing coarse grid is exactly the
    // incoming fine grid, so this interpolation has no opacity discontinuity.
    float gridSignal = mix(fine, coarse, transition);
    // The authored world-grid step remains the visual anchor. It is wider and
    // brighter than adaptive subdivisions, but fades once its spacing is too
    // fine for the current pixel footprint to avoid distant moire.
    float authoredResolvable = smoothstep(0.85, 2.5,
        baseSpacing / max(footprint, 1e-7));
    float authoredSignal = gridLines(coordinate, baseSpacing, 1.75) *
        authoredResolvable;

    vec3 rayDirection = ray / rayLength;
    float incidence = abs(dot(rayDirection, normal));
    float horizonFade = smoothstep(0.0, 0.22, incidence);
    float adaptiveAlpha = gridSignal * 0.20;
    float authoredAlpha = authoredSignal * 0.43;
    float alpha = max(adaptiveAlpha, authoredAlpha) *
        horizonFade * push.gridSettings.w;
    vec3 minorColor = editorColor(vec3(0.52, 0.56, 0.63));
    vec3 majorColor = editorColor(vec3(0.70, 0.74, 0.82));
    vec3 color = mix(minorColor, majorColor, authoredSignal);
    float depthVisibility = 1.0;

    // Resolve visibility only after evaluating derivatives so line filtering
    // remains well-defined on pixels adjacent to an opaque silhouette.
    ivec2 depthSize = textureSize(opaqueDepth, 0);
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0),
        depthSize - ivec2(1));
    float sceneDepth = texelFetch(opaqueDepth, pixel, 0).r;
    if (sceneDepth < 0.999999) {
        vec4 sceneHomogeneous = push.inverseViewProjection *
            vec4(ndc, sceneDepth, 1.0);
        if (abs(sceneHomogeneous.w) > 1e-7) {
            vec3 sceneWorld = sceneHomogeneous.xyz / sceneHomogeneous.w;
            float sceneDistance = dot(sceneWorld - nearWorld, rayDirection);
            float depthBias = max(0.001, sceneDistance * 2e-5);
            if (gridDistance >= sceneDistance - depthBias)
                depthVisibility = 0.0;
        }
    }

    float alongU = axisLine(coordinate.y);
    float alongV = axisLine(coordinate.x);
    if (alongU > 0.0) {
        color = mix(color, editorColor(axisColor(axisU)), alongU);
        alpha = max(alpha, 0.72 * alongU * horizonFade *
            push.gridSettings.w);
    }
    if (alongV > 0.0) {
        color = mix(color, editorColor(axisColor(axisV)), alongV);
        alpha = max(alpha, 0.72 * alongV * horizonFade *
            push.gridSettings.w);
    }
    if (outputTransport() == 0u) {
        float fadeRegion = smoothstep(0.0, 0.035, alpha) *
            (1.0 - smoothstep(0.35, 0.70, alpha));
        alpha += (interleavedGradientNoise(gl_FragCoord.xy) - 0.5) *
            (1.0 / 255.0) * fadeRegion;
    }
    alpha = clamp(alpha, 0.0, 0.82) * depthVisibility;
    return mix(outputValue, color, alpha);
}

vec3 applyEditorOverlays(vec3 outputValue) {
    return applySelectionOutline(applyViewportGrid(outputValue));
}

void main() {
    if (motionVectorView()) {
        outColor = vec4(motionVectorColor(texelFetch(motionVectors,
            ivec2(gl_FragCoord.xy), 0).xy), 1.0);
        return;
    }
    vec3 sceneLinear = texture(sceneColor, fragTexCoord).rgb;
    if (bloomActive()) sceneLinear = compositeBloom(sceneLinear);
    vec3 sceneAcesCg = sceneLinear *
        (autoExposure() ? exposureState.y : exp2(push.manualExposureEv));
    if (outputOperator() == 0u) {
        vec3 encoded = sampleAces2Encoded(sceneAcesCg);
        if (outputTransport() == 0u) {
            outColor = vec4(applyEditorOverlays(decodeSrgb(encoded)), 1.0);
            return;
        }
        vec3 rec2020Nits = decodeSt2084ToNits(encoded);
        vec3 targetNits = outputTransport() == 1u
            ? linearRec2020ToLinearSrgb(rec2020Nits) : rec2020Nits;
        outColor = vec4(applyEditorOverlays(
            min(targetNits, vec3(push.peakNits)) /
                push.paperWhiteNits), 1.0);
        return;
    }
    vec3 linearSrgb = max(acesCgToLinearSrgb(sceneAcesCg), vec3(0.0));
    if (outputOperator() == 2u) {
        outColor = vec4(applyEditorOverlays(
            clamp(linearSrgb, 0.0, 1.0)), 1.0);
        return;
    }
    float a = 2.51; float b = 0.03; float c = 2.43;
    float d = 0.59; float e = 0.14;
    vec3 compatibility = clamp(
        (linearSrgb * (a * linearSrgb + b)) /
        (linearSrgb * (c * linearSrgb + d) + e), 0.0, 1.0);
    outColor = vec4(applyEditorOverlays(compatibility), 1.0);
}
