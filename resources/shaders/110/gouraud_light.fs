#version 110

uniform vec4 uniform_color;
uniform float emission_factor;
// PetkosOrca: filament FINISH.
//
// Orca paints every filament as a flat colour, so silver PLA arrives as light grey and a
// silver-and-yellow part reads as one colour with a dull half. Silver is not a colour though - it
// is grey with a metallic finish, exactly as gold is yellow with one. So the property belongs to
// the material rather than the palette, and every filament becomes renderable as what it is.
//
// The shading difference between a metal and a plastic is one fact: a metal has almost no diffuse
// and TINTS its reflection with its own colour, where a plastic reflects the light source's colour
// (white) and gets its colour from the diffuse term. That is the whole model, and it is what makes
// silver look like silver instead of pale grey.
//
// Both uniforms default to 0 when nothing sets them, and at 0 every expression below collapses to
// the original line exactly - mix(white, c, 0) is white, (1 - 0.85*0) is 1, and the gloss branch
// is not taken. So a build that never uploads a finish is pixel-identical to before.
uniform float material_metalness;   // 0 = plastic, 1 = metal
uniform float material_gloss;       // <=1 = leave the highlight alone, >1 tightens it


// x = tainted, y = specular;
varying vec2 intensity;

void main()
{
    // See material_metalness above: a metal tints its reflection, a plastic does not.
    float spec = material_gloss > 1.0 ? pow(max(intensity.y, 0.0), material_gloss) : intensity.y;
    vec3  reflected = mix(vec3(1.0), uniform_color.rgb, material_metalness) * spec * (1.0 + 2.0 * material_metalness);
    vec3  body = uniform_color.rgb * (intensity.x + emission_factor) * (1.0 - 0.85 * material_metalness);
    gl_FragColor = vec4(reflected + body, uniform_color.a);
}
