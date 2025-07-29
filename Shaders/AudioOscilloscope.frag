// Copyright MediaZ Teknoloji A.S. All Rights Reserved.

#version 450

layout(binding = 0) uniform sampler2D TraceData;

layout(binding = 1) uniform OscilloscopeParams
{
    float Thickness;
    float Amplitude;
    float Intensity;
    vec4 Color;
    float AudioScale;
    float GlowIntensity;
    float GlowFalloff;
} Params;

layout(location = 0) out vec4 OutColor;
layout(location = 0) in vec2 uv;

#define P 3.14159
#define E .001

void main()
{
    vec2 c = uv;

    float s = texture(TraceData, vec2(c.x, 0.5)).r;
    
    s = s * Params.AudioScale;
    
    // Create oscilloscope trace at the center of the screen
    float trace = 0.5 + Params.Amplitude * s;
    
    // Calculate distance from current pixel to the oscilloscope trace
    float distance = abs(c.y - trace);
    
    // Create main line with sharp edge
    float line = distance < Params.Thickness ? 1.0 : 0.0;
    
    // Create exponential glow that extends to the edges
    float maxDistance = max(c.y, 1.0 - c.y); // Distance to nearest edge
    float normalizedDistance = distance / maxDistance;
    float glow = exp(-normalizedDistance * Params.GlowFalloff); // Exponential falloff
    
    // Combine line and glow
    float intensity = max(line, glow * Params.GlowIntensity); // Use parameter for glow intensity
    
    vec3 color = intensity * Params.Color.rgb * Params.Intensity;
    
    OutColor = vec4(color, 1.0);
}
