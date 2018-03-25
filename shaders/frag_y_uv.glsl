#version 100

varying highp vec2 uvpos;
precision mediump float;

uniform sampler2D smp;
uniform sampler2D smp1;
uniform mediump vec4      color;

void main()
{
    float y = 1.16438356 * (texture2D(smp, uvpos).x - 0.0625);
    float u = texture2D(smp1, uvpos).r - 0.5;
    float v = texture2D(smp1, uvpos).g - 0.5;

    gl_FragColor.r = y + 1.59602678 * v;
    gl_FragColor.g = y - 0.39176229 * u - 0.81296764 * v;
    gl_FragColor.b = y + 2.01723214 * u;
    gl_FragColor.a = 1.0;

    gl_FragColor = gl_FragColor * color;
}
