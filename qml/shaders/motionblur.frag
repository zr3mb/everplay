#version 440
// Motion blur along one direction (MotionBlur.qml): the average of the texture along the way the content
// moved during the frame — a 360° shutter. Compiled to motionblur.frag.qsb with Qt's qsb (see the README).

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 blur;      // the frame's motion, in texture coordinates
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    const int samples = 16;
    vec4 sum = vec4(0.0);
    float weights = 0.0;
    for (int i = 0; i < samples; ++i) {
        float t = float(i) / float(samples - 1) - 0.5;
        // A slightly heavier middle: the trail fades out instead of ending in a hard ghost.
        float w = 1.0 - abs(t);
        sum += texture(source, qt_TexCoord0 - blur * t) * w;
        weights += w;
    }
    fragColor = sum / weights * qt_Opacity;
}
