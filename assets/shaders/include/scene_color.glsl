#ifndef IRIDIUM_SCENE_COLOR_GLSL
#define IRIDIUM_SCENE_COLOR_GLSL

vec3 linearSrgbToAcesCg(vec3 value) {
    return vec3(
        dot(vec3(0.6130974024, 0.3395231462, 0.0473794514), value),
        dot(vec3(0.0701937225, 0.9163538791, 0.0134523984), value),
        dot(vec3(0.0206155929, 0.1095697729, 0.8698146342), value));
}

vec3 acesCgToLinearSrgb(vec3 value) {
    return vec3(
        dot(vec3(1.705050992697, -0.621792120673, -0.083258872024), value),
        dot(vec3(-0.130256417562, 1.140804736533, -0.010548318971), value),
        dot(vec3(-0.024003356839, -0.128968975993, 1.152972332832), value));
}

#endif
