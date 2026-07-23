#version 450

layout(set = 0, binding = 0) uniform sampler2D source_texture;

layout(push_constant) uniform DrawParameters {
    int effect;
    int mode;
    vec2 texture_size;
    vec2 target_size;
    vec2 padding;
    vec4 color;
} parameters;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 output_color;

ivec2 clamped_coordinate(ivec2 coordinate) {
    return clamp(coordinate, ivec2(0), ivec2(parameters.texture_size) - 1);
}

vec4 fetch_pixel(ivec2 coordinate) {
    return texelFetch(source_texture, clamped_coordinate(coordinate), 0);
}

vec4 nearest_pixel(vec2 coordinate) {
    return fetch_pixel(ivec2(floor(coordinate * parameters.texture_size)));
}

vec4 quilez_pixel(vec2 coordinate) {
    vec2 position = coordinate * parameters.texture_size + 0.5;
    vec2 base = floor(position);
    vec2 fraction = fract(position);
    fraction = fraction * fraction * fraction *
               (fraction * (fraction * 6.0 - 15.0) + 10.0);
    return texture(source_texture,
                   (base + fraction - 0.5) / parameters.texture_size);
}

vec2 scanline_resolution;

float to_linear_component(float component) {
    return component <= 0.04045 ? component / 12.92
        : pow((component + 0.055) / 1.055, 2.4);
}

vec3 to_linear(vec3 color) {
    return vec3(to_linear_component(color.r), to_linear_component(color.g),
                to_linear_component(color.b));
}

float to_srgb_component(float component) {
    return component < 0.0031308 ? component * 12.92
        : 1.055 * pow(component, 0.41666) - 0.055;
}

vec3 to_srgb(vec3 color) {
    return vec3(to_srgb_component(color.r), to_srgb_component(color.g),
                to_srgb_component(color.b));
}

vec3 scanline_fetch(vec2 position, vec2 offset) {
    position = floor(position * scanline_resolution + offset) /
               scanline_resolution;
    if (max(abs(position.x - 0.5), abs(position.y - 0.5)) > 0.5)
        return vec3(0.0);
    return to_linear(texture(source_texture, position, -16.0).rgb);
}

vec2 scanline_distance(vec2 position) {
    position *= scanline_resolution;
    return -((position - floor(position)) - vec2(0.5));
}

float gaussian(float position, float scale) {
    return exp2(scale * position * position);
}

vec3 horizontal_three(vec2 position, float offset) {
    vec3 b = scanline_fetch(position, vec2(-1.0, offset));
    vec3 c = scanline_fetch(position, vec2( 0.0, offset));
    vec3 d = scanline_fetch(position, vec2( 1.0, offset));
    float distance = scanline_distance(position).x;
    float wb = gaussian(distance - 1.0, -3.0);
    float wc = gaussian(distance,       -3.0);
    float wd = gaussian(distance + 1.0, -3.0);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

vec3 horizontal_five(vec2 position, float offset) {
    vec3 a = scanline_fetch(position, vec2(-2.0, offset));
    vec3 b = scanline_fetch(position, vec2(-1.0, offset));
    vec3 c = scanline_fetch(position, vec2( 0.0, offset));
    vec3 d = scanline_fetch(position, vec2( 1.0, offset));
    vec3 e = scanline_fetch(position, vec2( 2.0, offset));
    float distance = scanline_distance(position).x;
    float wa = gaussian(distance - 2.0, -3.0);
    float wb = gaussian(distance - 1.0, -3.0);
    float wc = gaussian(distance,       -3.0);
    float wd = gaussian(distance + 1.0, -3.0);
    float we = gaussian(distance + 2.0, -3.0);
    return (a * wa + b * wb + c * wc + d * wd + e * we) /
           (wa + wb + wc + wd + we);
}

float scan_weight(vec2 position, float offset) {
    return gaussian(scanline_distance(position).y + offset, -8.0);
}

vec3 scanline_tri(vec2 position) {
    vec3 a = horizontal_three(position, -1.0);
    vec3 b = horizontal_five(position, 0.0);
    vec3 c = horizontal_three(position, 1.0);
    return a * scan_weight(position, -1.0) +
           b * scan_weight(position,  0.0) +
           c * scan_weight(position,  1.0);
}

vec3 shadow_mask(vec2 position) {
    position.x += position.y * 3.0;
    vec3 mask = vec3(0.5);
    position.x = fract(position.x / 6.0);
    if (position.x < 0.333) mask.r = 1.5;
    else if (position.x < 0.666) mask.g = 1.5;
    else mask.b = 1.5;
    return mask;
}

vec4 scanline_pixel(vec2 coordinate) {
    scanline_resolution = parameters.target_size;
    vec2 screen_coordinate = coordinate * parameters.target_size;
    vec3 color = scanline_tri(coordinate) * shadow_mask(screen_coordinate);
    return vec4(to_srgb(color), 1.0);
}

void main() {
    if (parameters.mode == 1) {
        output_color = parameters.color;
        return;
    }

    vec4 color;
    if (parameters.effect == 1)
        color = texture(source_texture, texcoord);
    else if (parameters.effect == 2)
        color = quilez_pixel(texcoord);
    else if (parameters.effect == 3)
        color = scanline_pixel(texcoord);
    else
        color = nearest_pixel(texcoord);
    output_color = color * parameters.color;
    if (parameters.mode == 2)
        output_color.a = 1.0;
}
