#ifndef IRIDIUM_TRANSPARENCY_TRANSPORT_GLSL
#define IRIDIUM_TRANSPARENCY_TRANSPORT_GLSL

const float IRIDIUM_TRANSPARENCY_MIN_IOR = 1.0e-4;
const float IRIDIUM_TRANSPARENCY_MAX_IOR = 10.0;
const float IRIDIUM_TRANSPARENCY_MIN_COLOR = 1.0e-6;
const float IRIDIUM_TRANSPARENCY_MIN_DISTANCE = 1.0e-6;
const float IRIDIUM_TRANSPARENCY_MIN_INCIDENCE_COSINE = 0.05;
const float IRIDIUM_TRANSPARENCY_MAX_CONE_TANGENT = 1.0;
const float IRIDIUM_TRANSPARENCY_MIN_EDGE_BAND_PIXELS = 8.0;
const uint IRIDIUM_VIEW_REFRACTION_PYRAMIDS_AVAILABLE = 1u << 0u;

struct IridiumRefractionProjection {
    vec2 sourceUv;
    vec2 sampleUv;
    float footprintPixels;
    float lod;
    float edgeConfidence;
    float expectedViewDepthMeters;
    bool onScreen;
};

float iridiumTransparencyFiniteOr(float value, float fallback) {
    return isnan(value) || isinf(value) ? fallback : value;
}

float iridiumDielectricFresnel(float incidentIor, float transmittedIor,
    float incidentCosine, out bool totalInternalReflection) {
    incidentIor = clamp(iridiumTransparencyFiniteOr(incidentIor, 1.0),
        IRIDIUM_TRANSPARENCY_MIN_IOR, IRIDIUM_TRANSPARENCY_MAX_IOR);
    transmittedIor = clamp(iridiumTransparencyFiniteOr(transmittedIor, 1.0),
        IRIDIUM_TRANSPARENCY_MIN_IOR, IRIDIUM_TRANSPARENCY_MAX_IOR);
    float cosIncident = clamp(abs(iridiumTransparencyFiniteOr(
        incidentCosine, 1.0)), 0.0, 1.0);
    float eta = incidentIor / transmittedIor;
    float sinTransmittedSquared = eta * eta *
        (1.0 - cosIncident * cosIncident);
    totalInternalReflection = sinTransmittedSquared >= 1.0;
    if (totalInternalReflection) return 1.0;

    float cosTransmitted = sqrt(max(1.0 - sinTransmittedSquared, 0.0));
    float rs = (incidentIor * cosIncident -
        transmittedIor * cosTransmitted) /
        max(incidentIor * cosIncident + transmittedIor * cosTransmitted,
            IRIDIUM_TRANSPARENCY_MIN_IOR);
    float rp = (incidentIor * cosTransmitted -
        transmittedIor * cosIncident) /
        max(incidentIor * cosTransmitted + transmittedIor * cosIncident,
            IRIDIUM_TRANSPARENCY_MIN_IOR);
    return clamp(0.5 * (rs * rs + rp * rp), 0.0, 1.0);
}

vec3 iridiumBeerLambert(vec3 attenuationColor,
    float attenuationDistanceMeters, float pathLengthMeters) {
    pathLengthMeters = iridiumTransparencyFiniteOr(pathLengthMeters, 0.0);
    if (pathLengthMeters <= 0.0 || isinf(attenuationDistanceMeters))
        return vec3(1.0);
    attenuationColor = vec3(
        iridiumTransparencyFiniteOr(attenuationColor.r, 1.0),
        iridiumTransparencyFiniteOr(attenuationColor.g, 1.0),
        iridiumTransparencyFiniteOr(attenuationColor.b, 1.0));
    attenuationDistanceMeters = iridiumTransparencyFiniteOr(
        attenuationDistanceMeters, IRIDIUM_TRANSPARENCY_MIN_DISTANCE);
    vec3 boundedColor = clamp(attenuationColor,
        vec3(IRIDIUM_TRANSPARENCY_MIN_COLOR), vec3(1.0));
    float boundedDistance = max(attenuationDistanceMeters,
        IRIDIUM_TRANSPARENCY_MIN_DISTANCE);
    vec3 extinction = -log(boundedColor) / boundedDistance;
    return exp(-extinction * pathLengthMeters);
}

float iridiumThinSheetPathLength(float sheetThicknessMeters,
    float worldThicknessScale, float incidenceCosine) {
    sheetThicknessMeters = iridiumTransparencyFiniteOr(
        sheetThicknessMeters, 0.0);
    worldThicknessScale = iridiumTransparencyFiniteOr(
        worldThicknessScale, 0.0);
    incidenceCosine = iridiumTransparencyFiniteOr(incidenceCosine, 1.0);
    if (sheetThicknessMeters <= 0.0 || worldThicknessScale <= 0.0)
        return 0.0;
    return sheetThicknessMeters * worldThicknessScale /
        max(abs(incidenceCosine), IRIDIUM_TRANSPARENCY_MIN_INCIDENCE_COSINE);
}

// x: effective material transmission, y: scalar destination weight. A local
// zero-distance interface uses these with premultiplied blending so transparent
// surfaces already rendered behind it are not replaced by the opaque snapshot.
vec2 iridiumThinGlassLocalCompositionWeights(float transmission,
    float metallic, vec3 fresnel) {
    float effectiveTransmission = clamp(iridiumTransparencyFiniteOr(
        transmission, 0.0), 0.0, 1.0) *
        (1.0 - clamp(iridiumTransparencyFiniteOr(metallic, 0.0), 0.0, 1.0));
    vec3 boundedFresnel = clamp(vec3(
        iridiumTransparencyFiniteOr(fresnel.r, 1.0),
        iridiumTransparencyFiniteOr(fresnel.g, 1.0),
        iridiumTransparencyFiniteOr(fresnel.b, 1.0)),
        vec3(0.0), vec3(1.0));
    float interfaceOpacity = max(boundedFresnel.r,
        max(boundedFresnel.g, boundedFresnel.b));
    return vec2(effectiveTransmission,
        effectiveTransmission * (1.0 - interfaceOpacity));
}

vec3 iridiumReconstructViewPosition(vec2 screenUv, float deviceDepth,
    mat4 inverseProjection) {
    vec4 view = inverseProjection * vec4(
        screenUv * 2.0 - 1.0, deviceDepth, 1.0);
    return view.xyz / max(abs(view.w), 1.0e-7) * sign(view.w);
}

vec3 iridiumRefractTransparencyRay(vec3 incidentDirection,
    vec3 interfaceNormal, float incidentIor, float transmittedIor,
    out bool totalInternalReflection) {
    vec3 incident = normalize(incidentDirection);
    vec3 normal = normalize(interfaceNormal);
    if (dot(incident, normal) > 0.0) normal = -normal;
    float boundedIncidentIor = clamp(iridiumTransparencyFiniteOr(
        incidentIor, 1.0), IRIDIUM_TRANSPARENCY_MIN_IOR,
        IRIDIUM_TRANSPARENCY_MAX_IOR);
    float boundedTransmittedIor = clamp(iridiumTransparencyFiniteOr(
        transmittedIor, 1.0), IRIDIUM_TRANSPARENCY_MIN_IOR,
        IRIDIUM_TRANSPARENCY_MAX_IOR);
    float eta = boundedIncidentIor / boundedTransmittedIor;
    float cosine = clamp(-dot(incident, normal), 0.0, 1.0);
    float discriminant = 1.0 - eta * eta *
        (1.0 - cosine * cosine);
    totalInternalReflection = discriminant <= 0.0;
    if (totalInternalReflection)
        return normalize(reflect(incident, normal));
    return normalize(eta * incident +
        (eta * cosine - sqrt(discriminant)) * normal);
}

bool iridiumProjectWorldPosition(vec3 worldPosition, mat4 view,
    mat4 projection, out vec2 uv, out float clipW) {
    vec4 clip = projection * view * vec4(worldPosition, 1.0);
    clipW = clip.w;
    if (abs(clip.w) <= 1.0e-7) {
        uv = vec2(0.5);
        return false;
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return !(any(isnan(uv)) || any(isinf(uv)));
}

IridiumRefractionProjection iridiumProjectTransparencyRay(
    vec3 worldPosition, vec3 transmittedDirection,
    float pathLengthMeters, float metresPerWorldUnit,
    float perceptualRoughness, float incidentToTransmittedEta,
    mat4 view, mat4 projection, uvec2 renderExtent, uint mipLevels) {
    IridiumRefractionProjection result;
    result.sourceUv = vec2(0.5);
    result.sampleUv = vec2(0.5);
    result.footprintPixels = 1.0;
    result.lod = 0.0;
    result.edgeConfidence = 0.0;
    result.expectedViewDepthMeters = 0.0;
    result.onScreen = false;
    if (renderExtent.x == 0u || renderExtent.y == 0u || mipLevels == 0u)
        return result;

    float metresPerUnit = max(iridiumTransparencyFiniteOr(
        metresPerWorldUnit, 1.0), 1.0e-7);
    float pathWorld = max(iridiumTransparencyFiniteOr(
        pathLengthMeters, 0.0), 0.0) / metresPerUnit;
    vec3 direction = normalize(transmittedDirection);
    vec3 endpoint = worldPosition + direction * pathWorld;
    float sourceW = 0.0;
    float endpointW = 0.0;
    bool sourceValid = iridiumProjectWorldPosition(worldPosition,
        view, projection, result.sourceUv, sourceW);
    bool endpointValid = iridiumProjectWorldPosition(endpoint,
        view, projection, result.sampleUv, endpointW);
    result.expectedViewDepthMeters = abs(
        (view * vec4(endpoint, 1.0)).z) * metresPerUnit;

    float alpha = clamp(iridiumTransparencyFiniteOr(
        perceptualRoughness, 0.0), 0.0, 1.0);
    float eta = max(iridiumTransparencyFiniteOr(
        incidentToTransmittedEta, 1.0), IRIDIUM_TRANSPARENCY_MIN_IOR);
    float coneTangent = clamp(alpha * alpha * eta, 0.0,
        IRIDIUM_TRANSPARENCY_MAX_CONE_TANGENT);
    float radiusWorld = pathWorld * coneTangent;
    if (radiusWorld > 0.0 && endpointValid) {
        vec3 reference = abs(direction.z) < 0.999
            ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
        vec3 tangent = normalize(cross(reference, direction));
        vec3 bitangent = normalize(cross(direction, tangent));
        vec2 tangentUv;
        vec2 bitangentUv;
        float tangentW = 0.0;
        float bitangentW = 0.0;
        bool tangentValid = iridiumProjectWorldPosition(
            endpoint + tangent * radiusWorld, view, projection,
            tangentUv, tangentW);
        bool bitangentValid = iridiumProjectWorldPosition(
            endpoint + bitangent * radiusWorld, view, projection,
            bitangentUv, bitangentW);
        vec2 extent = vec2(renderExtent);
        if (tangentValid && tangentW > 0.0)
            result.footprintPixels = max(result.footprintPixels,
                2.0 * length((tangentUv - result.sampleUv) * extent));
        if (bitangentValid && bitangentW > 0.0)
            result.footprintPixels = max(result.footprintPixels,
                2.0 * length((bitangentUv - result.sampleUv) * extent));
    }
    result.lod = clamp(log2(max(result.footprintPixels, 1.0)),
        0.0, float(mipLevels - 1u));
    result.onScreen = sourceValid && endpointValid && sourceW > 0.0 &&
        endpointW > 0.0 && all(greaterThanEqual(result.sampleUv,
            vec2(0.0))) && all(lessThanEqual(result.sampleUv, vec2(1.0)));
    if (result.onScreen) {
        vec2 edgeUv = min(result.sampleUv, vec2(1.0) - result.sampleUv);
        float edgePixels = min(edgeUv.x * float(renderExtent.x),
            edgeUv.y * float(renderExtent.y));
        result.edgeConfidence = clamp(edgePixels /
            max(IRIDIUM_TRANSPARENCY_MIN_EDGE_BAND_PIXELS,
                result.footprintPixels), 0.0, 1.0);
    }
    return result;
}

#endif
