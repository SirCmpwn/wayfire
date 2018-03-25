#version 100
#extension GL_OES_EGL_image_external : require

varying highp vec2 uvpos;

uniform samplerExternalOES smp;
uniform mediump vec4      color;

void main()
{
    gl_FragColor = texture2D(smp, uvpos) * color;
}
